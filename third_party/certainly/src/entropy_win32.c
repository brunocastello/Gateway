/*
 * entropy_win32.c - entropy.h on Windows 95 and up.
 *
 * The Mac implementation stirs Microseconds, TickCount, GetMouse, LMGetTicks
 * and ReadLocation into a pool. All five are Toolbox calls, so this is a
 * rewrite rather than a port, but it is the same shape and the same argument:
 * no single source here is good, and the pool is what makes them adequate.
 *
 * The system PRNG is deliberately not the only source. It lives in ADVAPI32
 * behind a CryptAcquireContext that can fail on a machine whose default
 * container was never created -- a real state on old systems -- and on the
 * oldest systems in range it is not there at all. So it is used when it works
 * and mixed with the rest either way.
 */

#include "entropy.h"

#include <windows.h>

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

/*
 * The system PRNG, found at run time rather than imported.
 *
 * CryptoAPI arrived with Windows 95 OSR2 and NT 4.0. Naming CryptAcquireContextA
 * in the source puts it in the import table, and a machine whose ADVAPI32 does
 * not export it -- Windows 95 RTM, NT 3.51 -- refuses to load the image before
 * a line of this code runs, so the in-function failure handling below would
 * never get its chance. GetProcAddress is what turns "will not start" into
 * "one source short". Reported as brunocastello/Gateway#1.
 *
 * The declarations are local for the same reason. <wincrypt.h> is not itself
 * the problem, but with it included nothing stops a later edit from calling
 * one of these directly and quietly restoring the import.
 */
typedef ULONG_PTR GW_HCRYPTPROV;

#define GW_PROV_RSA_FULL       1
#define GW_CRYPT_VERIFYCONTEXT 0xF0000000

typedef BOOL (WINAPI *CryptAcquireContextA_fn)(GW_HCRYPTPROV *, LPCSTR, LPCSTR,
                                               DWORD, DWORD);
typedef BOOL (WINAPI *CryptReleaseContext_fn)(GW_HCRYPTPROV, DWORD);
typedef BOOL (WINAPI *CryptGenRandom_fn)(GW_HCRYPTPROV, DWORD, BYTE *);
typedef BOOLEAN (APIENTRY *RtlGenRandom_fn)(PVOID, ULONG);

/* Which of the three states this machine is in, for one line in the log. */
static const char *sSystemRng = "none -- timing pool only";

static void add_system_rng(void)
{
    HMODULE                 adv;
    RtlGenRandom_fn         pRtlGenRandom;
    CryptAcquireContextA_fn pAcquire;
    CryptReleaseContext_fn  pRelease;
    CryptGenRandom_fn       pGenRandom;
    GW_HCRYPTPROV           prov = 0;
    unsigned char           buf[32];

    adv = LoadLibraryA("advapi32.dll");
    if (adv == NULL)
        return;

    /*
     * RtlGenRandom first. It is exported only by ordinal name on XP and later,
     * where it is both the shortest path to the same generator and free of the
     * key-container question below. Nothing older exports it, which is exactly
     * the test we want.
     */
    pRtlGenRandom = (RtlGenRandom_fn)(void *)
        GetProcAddress(adv, "SystemFunction036");
    if (pRtlGenRandom != NULL) {
        if (pRtlGenRandom(buf, (ULONG)sizeof(buf))) {
            entropy_add(buf, sizeof(buf));
            memset(buf, 0, sizeof(buf));
            sSystemRng = "RtlGenRandom";
            FreeLibrary(adv);
            return;
        }
    }

    pAcquire = (CryptAcquireContextA_fn)(void *)
        GetProcAddress(adv, "CryptAcquireContextA");
    pRelease = (CryptReleaseContext_fn)(void *)
        GetProcAddress(adv, "CryptReleaseContext");
    pGenRandom = (CryptGenRandom_fn)(void *)
        GetProcAddress(adv, "CryptGenRandom");

    if (pAcquire == NULL || pRelease == NULL || pGenRandom == NULL) {
        FreeLibrary(adv);
        return;
    }

    /* VERIFYCONTEXT: no key container is created or needed, which is what
     * makes this work on a machine that has never had one. */
    if (pAcquire(&prov, NULL, NULL, GW_PROV_RSA_FULL, GW_CRYPT_VERIFYCONTEXT)) {
        if (pGenRandom(prov, (DWORD)sizeof(buf), buf)) {
            entropy_add(buf, sizeof(buf));
            memset(buf, 0, sizeof(buf));
            sSystemRng = "CryptGenRandom";
        }
        pRelease(prov, 0);
    }

    /*
     * Safe to release on every path: ADVAPI32 is already in the process from
     * our own static imports of the Reg* family, so this drops a reference
     * count rather than unmapping anything.
     */
    FreeLibrary(adv);
}

/*
 * Machine-specific state. None of this changes between two runs on the same
 * box, so it contributes nothing to the difference between one handshake and
 * the next -- what it does is separate this machine from an identical one
 * installed from the same disk, which matters most on exactly the systems that
 * have no system PRNG. NSS reaches further on that path, walking the shell
 * folders and reading up to 250 KB of file contents; on a 95-era disk that
 * would stall the cooperative loop for seconds, so this stops at the cheap
 * sources.
 */
static void add_machine(void)
{
    char  name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD len = sizeof(name);
    DWORD drives, serial, complen, flags;
    DWORD sectors, bytes, freeclusters, clusters;
    char  volume[128], fsname[128];

    drives = GetLogicalDrives();
    entropy_add(&drives, sizeof(drives));

    if (GetComputerNameA(name, &len))
        entropy_add(name, len);

    volume[0] = '\0';
    fsname[0] = '\0';
    if (GetVolumeInformationA(NULL, volume, sizeof(volume), &serial, &complen,
                              &flags, fsname, sizeof(fsname))) {
        entropy_add(volume, strlen(volume));
        entropy_add(&serial, sizeof(serial));
        entropy_add(fsname, strlen(fsname));
    }

    /* Free space moves with everything the machine has ever done. */
    if (GetDiskFreeSpaceA(NULL, &sectors, &bytes, &freeclusters, &clusters)) {
        entropy_add(&freeclusters, sizeof(freeclusters));
        entropy_add(&clusters, sizeof(clusters));
        entropy_add(&bytes, sizeof(bytes));
    }
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

    add_system_rng();
    add_machine();
    add_timing();
    sReady = 1;
}

const char *entropy_system_source(void)
{
    entropy_init();
    return sSystemRng;
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

void entropy_get(void *buf, size_t len)
{
    unsigned char    *p = (unsigned char *)buf;
    unsigned char     block[32];
    br_sha256_context sha;
    unsigned long     counter = 0;

    if (buf == NULL) return;
    entropy_init();

    while (len > 0) {
        size_t n = (len < sizeof(block)) ? len : sizeof(block);

        /*
         * Stirred per block, and the counter is hashed in, so two blocks
         * cannot come out equal even if nothing in the pool changed between
         * them -- which on a machine with no system generator and a stopped
         * clock is a real possibility rather than a theoretical one.
         */
        add_timing();
        br_sha256_init(&sha);
        br_sha256_update(&sha, sPool, sizeof(sPool));
        br_sha256_update(&sha, &counter, sizeof(counter));
        br_sha256_out(&sha, block);

        memcpy(p, block, n);
        entropy_add(block, sizeof(block));

        p += n;
        len -= n;
        counter++;
    }
    memset(block, 0, sizeof(block));
}
