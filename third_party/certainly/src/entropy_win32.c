/*
 * entropy_win32.c - entropy.h on Windows 95 OSR2 and up.
 *
 * The Mac implementation stirs Microseconds, TickCount, GetMouse, LMGetTicks
 * and ReadLocation into a pool. All five are Toolbox calls, so this is a
 * rewrite rather than a port, but it is the same shape and the same argument:
 * no single source here is good, and the pool is what makes them adequate.
 *
 * CryptGenRandom is deliberately not the only source. It exists from Windows
 * 95 OSR2 onward, which is exactly Gateway's floor, but it lives in
 * ADVAPI32 behind a CryptAcquireContext that can fail on a machine whose
 * default container was never created -- a real state on old systems. So it is
 * used when it works and mixed with the rest either way.
 */

#include "entropy.h"

#include <windows.h>
#include <wincrypt.h>

#include <string.h>

#include <bearssl.h>

#define POOL_BYTES 64

static unsigned char sPool[POOL_BYTES];
static size_t        sAt;
static int           sReady;

/*
 * Fold bytes into the pool rather than overwrite it. Every source is weak on
 * its own; mixing means a later good one cannot be undone by an earlier poor
 * one, and the rotation stops repeated small additions landing in one place.
 */
void entropy_add(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i;

    for (i = 0; i < len; i++) {
        sPool[sAt] ^= p[i];
        sAt = (sAt + 1) % POOL_BYTES;
        sPool[sAt] = (unsigned char)(sPool[sAt] * 31u + p[i]);
    }
}

static void add_crypt_api(void)
{
    HCRYPTPROV prov = 0;
    unsigned char buf[32];

    /* VERIFYCONTEXT: no key container is created or needed, which is what
     * makes this work on a machine that has never had one. */
    if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT))
        return;

    if (CryptGenRandom(prov, sizeof(buf), buf))
        entropy_add(buf, sizeof(buf));

    CryptReleaseContext(prov, 0);
}

static void add_timing(void)
{
    LARGE_INTEGER qpc;
    FILETIME      ft;
    DWORD         t;
    POINT         pt;
    MEMORYSTATUS  mem;

    /*
     * QueryPerformanceCounter is the best of these by far: it is a hardware
     * counter, and its low bits are unpredictable at the resolution anything
     * else here is measured in. It can fail on machines without one, hence the
     * check rather than trusting it.
     */
    if (QueryPerformanceCounter(&qpc))
        entropy_add(&qpc, sizeof(qpc));

    t = GetTickCount();
    entropy_add(&t, sizeof(t));

    GetSystemTimeAsFileTime(&ft);
    entropy_add(&ft, sizeof(ft));

    /* Where the user last left the pointer, as the Mac build reads GetMouse. */
    if (GetCursorPos(&pt))
        entropy_add(&pt, sizeof(pt));

    t = GetCurrentProcessId();
    entropy_add(&t, sizeof(t));
    t = GetCurrentThreadId();
    entropy_add(&t, sizeof(t));

    mem.dwLength = sizeof(mem);
    GlobalMemoryStatus(&mem);
    entropy_add(&mem, sizeof(mem));
}

void entropy_init(void)
{
    if (sReady) return;

    add_crypt_api();
    add_timing();
    sReady = 1;
}

void entropy_seed_engine(br_ssl_engine_context *eng)
{
    unsigned char seed[32];
    br_sha256_context sha;

    entropy_init();

    /*
     * Stirred once more at the point of use, so two connections opened in the
     * same second do not start from the same pool state, and hashed so that
     * what BearSSL receives does not expose the pool itself.
     */
    add_timing();

    br_sha256_init(&sha);
    br_sha256_update(&sha, sPool, sizeof(sPool));
    br_sha256_out(&sha, seed);

    br_ssl_engine_inject_entropy(eng, seed, sizeof(seed));

    /* Do not leave the seed we just handed out sitting in the pool. */
    entropy_add(seed, sizeof(seed));
    memset(seed, 0, sizeof(seed));
}
