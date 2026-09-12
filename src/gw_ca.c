/*
 * gw_ca.c - see gw_ca.h.
 */

#include "gw_ca.h"

#include <string.h>

#include <bearssl.h>

#include "gw_plat.h"
#include "portable/gw_log.h"
#include "portable/gw_url.h"
#include "portable/gw_util.h"
#include "portable/gw_x509write.h"

#include "entropy.h"

#define GW_CA_BITS      1024
#define GW_CA_FILE      "Gateway CA"
#define GW_CA_NAME      "Gateway Local CA"
#define GW_CA_CERT_MAX  1024
#define GW_CA_LEAF_MAX  1024
#define GW_CA_CACHE     6

/*
 * Validity, fixed rather than taken from the clock.
 *
 * A vintage machine whose battery has gone reads 1904, or 1980, or whatever
 * the last boot left behind. A certificate generated against that clock would
 * be rejected by the browser on the same machine the moment the clock was
 * corrected, and the failure would look like a Gateway bug. Starting in 1998
 * and ending in 2037 is valid under every clock those machines can hold, and
 * stops short of the 2038 boundary a 32-bit time_t cannot cross.
 */
#define GW_CA_FROM      "980101000000Z"
#define GW_CA_UNTIL     "370101000000Z"

static int                 sReady;
static int                 sTried;
static br_rsa_private_key  sSk;
static br_rsa_public_key   sPk;
static unsigned char       sSkBuf[BR_RSA_KBUF_PRIV_SIZE(GW_CA_BITS)];
static unsigned char       sPkBuf[BR_RSA_KBUF_PUB_SIZE(GW_CA_BITS)];
static unsigned char       sCaCert[GW_CA_CERT_MAX];
static size_t              sCaLen;

typedef struct {
    char           host[GW_MAX_HOST];
    unsigned char  der[GW_CA_LEAF_MAX];
    size_t         len;
} GWLeaf;

static GWLeaf sCache[GW_CA_CACHE];
static int    sCacheNext;

/* ------------------------------------------------------------------ */
/* The store                                                           */
/* ------------------------------------------------------------------ */

/*
 * Gateway's own format rather than PKCS#8 or PKCS#1.
 *
 * BearSSL holds a private key as its CRT components and has no writer for any
 * standard encoding of them, so storing one in a standard format would mean
 * writing that encoder and then a parser to read it back -- for a file only
 * this program ever opens. Sixteen-bit big-endian lengths and the components
 * end to end is the whole format.
 */
#define GW_CA_MAGIC  "GWCA"
#define GW_CA_VER    1
#define GW_CA_HDR    24

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xFF);
}

static unsigned get16(const unsigned char *p)
{
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

static int store_save(void)
{
    static unsigned char buf[4096];
    unsigned char *p = buf + GW_CA_HDR;
    size_t         room = sizeof(buf) - GW_CA_HDR;
    size_t         need;

    need = sSk.plen + sSk.qlen + sSk.dplen + sSk.dqlen + sSk.iqlen +
           sCaLen + sPk.nlen + sPk.elen;
    if (need > room) {
        gw_log("certificate authority: %lu bytes will not fit the store",
               (unsigned long)need);
        return 0;
    }

    memcpy(buf, GW_CA_MAGIC, 4);
    put16(buf + 4,  GW_CA_VER);
    put16(buf + 6,  sSk.n_bitlen);
    put16(buf + 8,  (unsigned)sSk.plen);
    put16(buf + 10, (unsigned)sSk.qlen);
    put16(buf + 12, (unsigned)sSk.dplen);
    put16(buf + 14, (unsigned)sSk.dqlen);
    put16(buf + 16, (unsigned)sSk.iqlen);
    put16(buf + 18, (unsigned)sCaLen);
    put16(buf + 20, (unsigned)sPk.nlen);
    put16(buf + 22, (unsigned)sPk.elen);

    memcpy(p, sSk.p,  sSk.plen);  p += sSk.plen;
    memcpy(p, sSk.q,  sSk.qlen);  p += sSk.qlen;
    memcpy(p, sSk.dp, sSk.dplen); p += sSk.dplen;
    memcpy(p, sSk.dq, sSk.dqlen); p += sSk.dqlen;
    memcpy(p, sSk.iq, sSk.iqlen); p += sSk.iqlen;
    memcpy(p, sCaCert, sCaLen);   p += sCaLen;
    memcpy(p, sPk.n,  sPk.nlen);  p += sPk.nlen;
    memcpy(p, sPk.e,  sPk.elen);  p += sPk.elen;

    return GWPlat_WriteFile(GW_CA_FILE, buf, (long)(p - buf));
}

static int store_load(void)
{
    static unsigned char buf[4096];
    long                 n;
    const unsigned char *p;
    size_t               plen, qlen, dplen, dqlen, iqlen, clen, nlen, elen;
    size_t               need;
    unsigned char       *k = sSkBuf;

    n = GWPlat_ReadFile(GW_CA_FILE, buf, sizeof(buf));
    if (n < GW_CA_HDR) return 0;
    if (memcmp(buf, GW_CA_MAGIC, 4) != 0) return 0;
    if (get16(buf + 4) != GW_CA_VER) return 0;

    plen  = get16(buf + 8);
    qlen  = get16(buf + 10);
    dplen = get16(buf + 12);
    dqlen = get16(buf + 14);
    iqlen = get16(buf + 16);
    clen  = get16(buf + 18);
    nlen  = get16(buf + 20);
    elen  = get16(buf + 22);

    need = plen + qlen + dplen + dqlen + iqlen + clen + nlen + elen;
    if ((size_t)n < GW_CA_HDR + need) return 0;
    if (clen == 0 || clen > sizeof(sCaCert)) return 0;
    if (plen + qlen + dplen + dqlen + iqlen > sizeof(sSkBuf)) return 0;
    if (nlen + elen > sizeof(sPkBuf)) return 0;

    /* The components are laid out in one buffer, as keygen would have. */
    p = buf + GW_CA_HDR;
    sSk.n_bitlen = get16(buf + 6);
    sSk.p  = k; memcpy(k, p, plen);  k += plen;  p += plen;  sSk.plen  = plen;
    sSk.q  = k; memcpy(k, p, qlen);  k += qlen;  p += qlen;  sSk.qlen  = qlen;
    sSk.dp = k; memcpy(k, p, dplen); k += dplen; p += dplen; sSk.dplen = dplen;
    sSk.dq = k; memcpy(k, p, dqlen); k += dqlen; p += dqlen; sSk.dqlen = dqlen;
    sSk.iq = k; memcpy(k, p, iqlen); k += iqlen; p += iqlen; sSk.iqlen = iqlen;

    memcpy(sCaCert, p, clen); p += clen;
    sCaLen = clen;

    k = sPkBuf;
    sPk.n = k; memcpy(k, p, nlen); k += nlen; p += nlen; sPk.nlen = nlen;
    sPk.e = k; memcpy(k, p, elen);                       sPk.elen = elen;

    return 1;
}

/* ------------------------------------------------------------------ */
/* Signing                                                             */
/* ------------------------------------------------------------------ */

/*
 * SHA-1 over the TBSCertificate, then PKCS#1 v1.5. SHA-1 because IE 4 and
 * Netscape 4 cannot verify anything newer and reject rather than warn; the
 * exposure is a forged certificate presented over loopback on a machine the
 * forger is already running code on.
 */
static size_t sign_cert(const unsigned char *tbs, size_t tbs_len,
                        unsigned char *out, size_t cap, size_t *off)
{
    unsigned char    hash[20];
    unsigned char    sig[GW_CA_BITS / 8];
    br_sha1_context  sha;

    if ((sSk.n_bitlen + 7) / 8 > sizeof(sig)) return 0;

    br_sha1_init(&sha);
    br_sha1_update(&sha, tbs, tbs_len);
    br_sha1_out(&sha, hash);

    if (!br_rsa_pkcs1_sign_get_default()(BR_HASH_OID_SHA1, hash, sizeof(hash),
                                         &sSk, sig))
        return 0;

    return gw_x509_cert(tbs, tbs_len, sig, (sSk.n_bitlen + 7) / 8,
                        out, cap, off);
}

/* ------------------------------------------------------------------ */
/* Generating                                                          */
/* ------------------------------------------------------------------ */

static int generate(void)
{
    br_hmac_drbg_context drbg;
    unsigned char        seed[64];
    static unsigned char tbsbuf[GW_CA_CERT_MAX];
    size_t               tbslen, tbsoff, certoff;
    GWCertReq            req;
    /* A fixed serial for the authority; the leaves vary theirs by host. */
    static const unsigned char kSerial[] = { 0x01 };

    gw_log("certificate authority: generating a %d-bit key, "
           "this takes a while", GW_CA_BITS);

    entropy_get(seed, sizeof(seed));
    br_hmac_drbg_init(&drbg, &br_sha256_vtable, seed, sizeof(seed));
    memset(seed, 0, sizeof(seed));

    if (!br_rsa_keygen_get_default()(&drbg.vtable, &sSk, sSkBuf, &sPk, sPkBuf,
                                     GW_CA_BITS, 0)) {
        gw_log("certificate authority: key generation failed");
        return 0;
    }

    memset(&req, 0, sizeof(req));
    req.cn         = GW_CA_NAME;
    req.issuer_cn  = GW_CA_NAME;          /* self-signed */
    req.is_ca      = 1;
    req.serial     = kSerial;
    req.serial_len = sizeof(kSerial);
    req.mod        = sPk.n;
    req.mod_len    = sPk.nlen;
    req.exp        = sPk.e;
    req.exp_len    = sPk.elen;
    req.not_before = GW_CA_FROM;
    req.not_after  = GW_CA_UNTIL;

    tbslen = gw_x509_tbs(&req, tbsbuf, sizeof(tbsbuf), &tbsoff);
    if (tbslen == 0) {
        gw_log("certificate authority: could not build its own certificate");
        return 0;
    }

    sCaLen = sign_cert(tbsbuf + tbsoff, tbslen, sCaCert, sizeof(sCaCert),
                       &certoff);
    if (sCaLen == 0) {
        gw_log("certificate authority: could not sign its own certificate");
        return 0;
    }
    /* sign_cert writes to the end of the buffer; move it to the front so
     * GWCa_Cert() can hand out a plain pointer. */
    memmove(sCaCert, sCaCert + certoff, sCaLen);

    if (!store_save())
        gw_log("certificate authority: generated but not saved -- "
               "it will be generated again next launch");

    return 1;
}

/* ------------------------------------------------------------------ */

int GWCa_Ready(void) { return sReady; }

int GWCa_Init(void)
{
    if (sReady) return 1;
    /*
     * One attempt per launch. A machine that cannot generate a key -- out of
     * memory, or a pool that produced nothing usable -- would otherwise spend
     * the rest of its run retrying it on every CONNECT, and each retry blocks
     * the loop for as long as the first one did.
     */
    if (sTried) return 0;
    sTried = 1;

    if (store_load()) {
        gw_log("certificate authority: loaded from \"%s\"", GW_CA_FILE);
        sReady = 1;
        return 1;
    }

    if (generate()) {
        sReady = 1;
        return 1;
    }
    return 0;
}

const unsigned char *GWCa_Cert(size_t *len)
{
    if (!sReady) return NULL;
    if (len != NULL) *len = sCaLen;
    return sCaCert;
}

const void *GWCa_Key(void)
{
    return sReady ? (const void *)&sSk : NULL;
}

const unsigned char *GWCa_Leaf(const char *host, size_t *len)
{
    static unsigned char tbsbuf[GW_CA_LEAF_MAX];
    unsigned char        serial[4];
    unsigned long        h;
    size_t               tbslen, tbsoff, certoff, certlen;
    GWCertReq            req;
    GWLeaf              *slot;
    int                  i;

    if (host == NULL || *host == '\0') return NULL;
    if (!sReady && !GWCa_Init()) return NULL;
    if (strlen(host) >= GW_MAX_HOST) return NULL;

    for (i = 0; i < GW_CA_CACHE; i++)
        if (sCache[i].len > 0 && gw_stricmp(sCache[i].host, host) == 0) {
            if (len != NULL) *len = sCache[i].len;
            return sCache[i].der;
        }

    /*
     * A serial derived from the name, so one host keeps the same serial across
     * launches. A browser that has cached a certificate and then meets a
     * different serial for the same name under the same issuer has been given
     * two reasons to distrust it; keeping it stable avoids the question.
     */
    h = 2166136261UL;
    for (i = 0; host[i] != '\0'; i++) {
        h ^= (unsigned long)(unsigned char)host[i];
        h *= 16777619UL;
    }
    serial[0] = (unsigned char)((h >> 24) & 0x7F);   /* clear the top bit */
    serial[1] = (unsigned char)((h >> 16) & 0xFF);
    serial[2] = (unsigned char)((h >> 8) & 0xFF);
    serial[3] = (unsigned char)(h & 0xFF);

    memset(&req, 0, sizeof(req));
    req.cn         = host;
    req.issuer_cn  = GW_CA_NAME;
    req.is_ca      = 0;
    req.serial     = serial;
    req.serial_len = sizeof(serial);
    req.mod        = sPk.n;
    req.mod_len    = sPk.nlen;
    req.exp        = sPk.e;
    req.exp_len    = sPk.elen;
    req.not_before = GW_CA_FROM;
    req.not_after  = GW_CA_UNTIL;

    tbslen = gw_x509_tbs(&req, tbsbuf, sizeof(tbsbuf), &tbsoff);
    if (tbslen == 0) return NULL;

    /* Round robin rather than least-recently-used. With six slots and a page
     * touching a handful of hosts, the bookkeeping would cost more than the
     * occasional re-mint it saves. */
    slot = &sCache[sCacheNext];
    sCacheNext = (sCacheNext + 1) % GW_CA_CACHE;

    certlen = sign_cert(tbsbuf + tbsoff, tbslen, slot->der, sizeof(slot->der),
                        &certoff);
    if (certlen == 0) {
        slot->len = 0;
        return NULL;
    }
    memmove(slot->der, slot->der + certoff, certlen);
    slot->len = certlen;
    gw_copy_n(slot->host, sizeof(slot->host), host, strlen(host));

    if (len != NULL) *len = certlen;
    return slot->der;
}
