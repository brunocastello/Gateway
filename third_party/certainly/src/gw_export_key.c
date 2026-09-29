/*
 * gw_export_key.c - see gw_export_key.h.
 */

#include "gw_export_key.h"

#include <string.h>

#include "certainly_compat.h"
#include "entropy.h"
#include "gw_log.h"

#define GW_EXPORT_BITS  512

/*
 * A tenth of a second (6 ticks) is the threshold below which a person
 * watching a page load would not notice a pause at all; above it, the
 * browser is the same one waiting through a from-scratch RSA handshake
 * (the leaf's own 1024-bit private-key operation, already unavoidable),
 * so this is the only place that can say afterwards whether the extra
 * 512-bit keygen added anything worth knowing about. There is no PPC to
 * measure this against from here (CLAUDE.md: builds in CI only), so the
 * line goes under log_debug rather than asserting a number nobody has
 * verified on the hardware this exists for.
 */
#define GW_EXPORT_SLOW_TICKS  6

static int                sTried;
static int                sReady;
static br_rsa_private_key sSk;
static br_rsa_public_key  sPk;
static unsigned char      sSkBuf[BR_RSA_KBUF_PRIV_SIZE(GW_EXPORT_BITS)];
static unsigned char      sPkBuf[BR_RSA_KBUF_PUB_SIZE(GW_EXPORT_BITS)];

const br_rsa_private_key *gw_export_key_get(const unsigned char **mod,
    size_t *mod_len, const unsigned char **exp, size_t *exp_len)
{
    if (!sTried) {
        br_hmac_drbg_context drbg;
        unsigned char        seed[64];
        uint32_t             before, elapsed;

        sTried = 1;
        before = TickCount();

        entropy_get(seed, sizeof(seed));
        br_hmac_drbg_init(&drbg, &br_sha256_vtable, seed, sizeof(seed));
        memset(seed, 0, sizeof(seed));

        if (br_rsa_keygen_get_default()(&drbg.vtable, &sSk, sSkBuf,
                                        &sPk, sPkBuf, GW_EXPORT_BITS, 0)) {
            sReady = 1;
        }

        elapsed = TickCount() - before;
        if (elapsed >= GW_EXPORT_SLOW_TICKS) {
            gw_logd("export suite: the one-time %d-bit temporary key took "
                    "%lu ticks to generate", GW_EXPORT_BITS,
                    (unsigned long)elapsed);
        }
    }

    if (!sReady) return NULL;

    *mod = sPk.n; *mod_len = sPk.nlen;
    *exp = sPk.e; *exp_len = sPk.elen;
    return &sSk;
}
