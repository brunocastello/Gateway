/*
 * entropy.c — Mac OS 9 entropy gathering
 *
 * HOW THIS WORKS:
 *
 * We maintain a 256-byte "pool" that accumulates entropy. Each
 * time we gather entropy, we:
 *   1. Read values from multiple OS sources (timers, mouse, etc.)
 *   2. XOR each source's bytes into the pool at a rotating offset
 *   3. When it's time to seed BearSSL, inject the entire pool
 *
 * BearSSL's HMAC_DRBG (inside the SSL engine) does the heavy
 * lifting of turning our messy entropy into cryptographic-quality
 * random bytes. HMAC_DRBG is standardized (NIST SP 800-90A) and
 * designed to handle imperfect entropy inputs — it mixes everything
 * together using HMAC-SHA256.
 *
 * We inject entropy multiple times (at init, per-connection) so
 * the pool improves over time as more mouse movements and timing
 * variations accumulate.
 */

#include "entropy.h"

#include <Timer.h>       /* Microseconds() */
#include <Events.h>      /* TickCount(), GetMouse() */
#include <LowMem.h>      /* LMGetTicks() */
#include <Gestalt.h>     /* ReadLocation() */
#include <OpenTransport.h> /* OTGetTimeStamp() */
#include <string.h>

#define POOL_SIZE 256

static uint8_t  g_pool[POOL_SIZE];
static size_t   g_pool_offset = 0;

/*
 * XOR bytes into the pool at the current offset, wrapping around.
 *
 * Why XOR? It's the simplest operation that:
 *   - Never reduces entropy (XOR with random data keeps randomness)
 *   - Is fast (no divisions, no branches)
 *   - Spreads bits across the pool as offset advances
 */
static void pool_mix(const void *data, size_t len)
{
    const uint8_t *src = (const uint8_t *)data;

    for (size_t i = 0; i < len; i++) {
        g_pool[g_pool_offset] ^= src[i];
        g_pool_offset = (g_pool_offset + 1) % POOL_SIZE;
    }
}

/*
 * Gather entropy from all available Mac OS sources.
 *
 * Each source provides different quality of randomness:
 *
 * Microseconds() — returns a UnsignedWide (64-bit microsecond counter).
 *   Quality: LOW. The counter itself is predictable, but the exact
 *   value at the moment we read it depends on how long the app has
 *   been running, which is somewhat unpredictable.
 *
 * TickCount() — returns ticks (1/60th sec) since boot.
 *   Quality: LOW. But boot time is unknown to a remote attacker.
 *
 * GetMouse() — returns cursor position.
 *   Quality: MEDIUM. Genuinely unpredictable if the user is moving
 *   the mouse. Even slight jitter contributes real entropy.
 *
 * OTGetTimeStamp() — OT's internal high-resolution timer.
 *   Quality: MEDIUM. Includes network interrupt timing jitter.
 *
 * Stack contents — read some bytes from our own stack.
 *   Quality: UNKNOWN. Whatever was left by previous function calls.
 *   Unpredictable, but could be zero on a clean stack.
 */
static void gather_sources(void)
{
    UnsignedWide    usecs;
    unsigned long   ticks;
    Point           mouse;
    OTTimeStamp     otTime;
    uint8_t         stackJunk[32];

    /* Microsecond timer */
    Microseconds(&usecs);
    pool_mix(&usecs, sizeof(usecs));

    /* Tick counter */
    ticks = TickCount();
    pool_mix(&ticks, sizeof(ticks));

    /* Mouse position */
    GetMouse(&mouse);
    pool_mix(&mouse, sizeof(mouse));

    /* Low-memory tick counter (slightly different path than TickCount) */
    ticks = LMGetTicks();
    pool_mix(&ticks, sizeof(ticks));

    /* OT high-resolution timestamp */
    OTGetTimeStamp(&otTime);
    pool_mix(&otTime, sizeof(otTime));

    /*
     * Stack junk — we intentionally do NOT initialize this array.
     * Whatever bytes happen to be on the stack get mixed in.
     * The compiler may warn about this — that's fine, it's deliberate.
     */
    pool_mix(stackJunk, sizeof(stackJunk));
}

/*
 * Harvest entropy from timer jitter.
 * Calls Microseconds() in a tight loop and extracts LSBs of deltas.
 * Von Neumann debiasing ensures unbiased output.
 * Targets at least 256 bits (~32 bytes) of entropy.
 */
static void harvest_timer_jitter(void)
{
    UnsignedWide prev, curr;
    uint8_t      jitter_pool[64];
    int          byte_idx = 0;
    int          bit_idx = 0;
    int          pairs = 0;

    memset(jitter_pool, 0, sizeof(jitter_pool));
    Microseconds(&prev);

    while (byte_idx < (int)sizeof(jitter_pool)) {
        unsigned long delta1, delta2;
        int bit1, bit2;

        /* Get two consecutive timer deltas */
        Microseconds(&curr);
        delta1 = curr.lo - prev.lo;
        prev = curr;

        Microseconds(&curr);
        delta2 = curr.lo - prev.lo;
        prev = curr;

        /* Von Neumann debiasing: compare LSBs of the two deltas */
        bit1 = (int)(delta1 & 1);
        bit2 = (int)(delta2 & 1);

        if (bit1 != bit2) {
            /* Unequal pair — take the first bit */
            jitter_pool[byte_idx] |= (uint8_t)(bit1 << bit_idx);
            bit_idx++;
            if (bit_idx >= 8) {
                bit_idx = 0;
                byte_idx++;
            }
        }
        /* Equal pairs are discarded (Von Neumann debiasing) */

        pairs++;
        /* Safety valve — don't loop forever if timer has no jitter */
        if (pairs > 100000) break;
    }

    pool_mix(jitter_pool, sizeof(jitter_pool));

    /*
     * Overwrite local buffer with zeros — don't leave entropy on stack.
     * Must use volatile to prevent the compiler from optimizing this away.
     * Without volatile, the compiler sees "buffer is zeroed then never read"
     * and removes the zeroing entirely.
     */
    {
        volatile uint8_t *p = (volatile uint8_t *)jitter_pool;
        size_t i;
        for (i = 0; i < sizeof(jitter_pool); i++) p[i] = 0;
    }
}

void entropy_init(void)
{
    MachineLocation loc;

    /* Zero the pool */
    memset(g_pool, 0, POOL_SIZE);
    g_pool_offset = 0;

    /* Gather initial entropy */
    gather_sources();

    /*
     * ReadLocation gives us the machine's geographic settings
     * (latitude, longitude, timezone) from the Map control panel.
     * This doesn't change, but it's a one-time salt that a remote
     * attacker probably doesn't know.
     */
    ReadLocation(&loc);
    pool_mix(&loc, sizeof(loc));

    /* Gather again — the time elapsed during ReadLocation adds jitter */
    gather_sources();

    /*
     * Harvest timer jitter for additional high-quality entropy.
     * This runs AFTER the initial gather_sources() calls so that
     * any timing variation from those calls also contributes.
     */
    harvest_timer_jitter();

    /*
     * NOTE: MAC address would be a useful additional one-time salt here
     * (hardware address is unique per machine and unknown to a remote
     * attacker). However, reading the MAC requires Open Transport
     * (OTInetGetInterfaceInfo or similar), which may not be initialized
     * at the time entropy_init() is called. If OT is available at this
     * point in your startup sequence, read the MAC via
     * OTInetGetInterfaceInfo() and mix it in with pool_mix().
     */
}

void entropy_seed_engine(br_ssl_engine_context *eng)
{
    /* Gather fresh entropy right before seeding */
    gather_sources();

    /* Harvest timer jitter for additional entropy before injecting */
    harvest_timer_jitter();

    /*
     * Inject the entire pool into BearSSL's PRNG.
     *
     * br_ssl_engine_inject_entropy is ADDITIVE — it stirs our bytes
     * into whatever entropy the engine already has. It never replaces.
     * So even if our pool is weak, it can only make things better,
     * never worse.
     */
    br_ssl_engine_inject_entropy(eng, g_pool, POOL_SIZE);

    /*
     * Do NOT zero the pool here. The pool accumulates entropy over time —
     * each gather_sources() call XORs new data on top of existing data,
     * making subsequent connections STRONGER. Zeroing would discard the
     * one-time entropy from entropy_init() (ReadLocation, initial mouse
     * position, startup jitter) and weaken the seed for the 2nd+ connection.
     *
     * The pool itself is not secret material — the real secret is inside
     * BearSSL's HMAC_DRBG after injection. The pool is just a staging area.
     */
}

void entropy_add(const void *data, size_t len)
{
    pool_mix(data, len);
}

/*
 * NULL, and not a name. Mac OS has no system random generator to fall back
 * from, so there is no second state for a log line to distinguish — every
 * launch would print the same sentence. The Win32 implementation has three
 * states and returns which one it is in.
 */
const char *entropy_system_source(void)
{
    return NULL;
}
