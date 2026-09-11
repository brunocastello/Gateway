/*
 * gw_x509write.c - see gw_x509write.h.
 *
 * DER is encoded back to front. Every constructed value carries the length of
 * its content before the content, and when you write forwards you do not know
 * that length until you are past it -- so you either leave a hole and come back
 * to patch it, which needs the hole to be the right size and so needs the
 * length you do not have yet, or you write the content first and put the header
 * on the front afterwards. The second is what this does. It reads oddly, with
 * fields appearing in reverse, and it is one pass with no fixups.
 */

#include "gw_x509write.h"

#include <string.h>

/* A cursor that fills a buffer from its end towards its front. */
typedef struct {
    unsigned char *buf;
    size_t         cap;
    size_t         at;      /* index of the last byte written; cap == empty */
    int            err;
} Der;

static void der_init(Der *d, unsigned char *buf, size_t cap)
{
    d->buf = buf;
    d->cap = cap;
    d->at  = cap;
    d->err = 0;
}

static size_t der_len(const Der *d) { return d->cap - d->at; }

static void der_raw(Der *d, const void *p, size_t n)
{
    if (d->err || n == 0) return;       /* memcpy(_, NULL, 0) is still UB */
    if (n > d->at) { d->err = 1; return; }
    d->at -= n;
    memcpy(d->buf + d->at, p, n);
}

static void der_byte(Der *d, unsigned char b)
{
    if (d->err) return;
    if (d->at == 0) { d->err = 1; return; }
    d->buf[--d->at] = b;
}

/*
 * The length octets. Short form below 128, else a count of bytes followed by
 * the value big-endian -- and written back to front like everything else, so
 * the value goes down first and the count on top of it.
 */
static void der_length(Der *d, size_t n)
{
    if (n < 0x80) {
        der_byte(d, (unsigned char)n);
        return;
    }
    {
        unsigned char tmp[sizeof(size_t)];
        size_t        i = 0;
        size_t        v = n;

        while (v > 0 && i < sizeof(tmp)) {
            tmp[i++] = (unsigned char)(v & 0xFF);
            v >>= 8;
        }
        if (v != 0) { d->err = 1; return; }      /* absurdly large */
        {
            size_t k;
            for (k = 0; k < i; k++) der_byte(d, tmp[k]);
        }
        der_byte(d, (unsigned char)(0x80 | i));
    }
}

/* Put a tag and the length of the `content` bytes already written on top. */
static void der_head(Der *d, unsigned char tag, size_t content)
{
    der_length(d, content);
    der_byte(d, tag);
}

/* A complete primitive value: tag, length, body. */
static void der_prim(Der *d, unsigned char tag, const void *body, size_t n)
{
    der_raw(d, body, n);
    der_head(d, tag, n);
}

#define DER_INTEGER      0x02
#define DER_BITSTRING    0x03
#define DER_OCTETSTRING  0x04
#define DER_NULL         0x05
#define DER_OID          0x06
#define DER_UTF8STRING   0x0C
#define DER_PRINTABLE    0x13
#define DER_IA5STRING    0x16
#define DER_UTCTIME      0x17
#define DER_SEQUENCE     0x30
#define DER_SET          0x31
#define DER_CTX(n)       (unsigned char)(0xA0 | (n))   /* constructed [n] */

/*
 * INTEGER from a big-endian magnitude. DER integers are signed, so a value
 * whose top bit is set needs a leading zero or it reads as negative -- which
 * for an RSA modulus is the difference between a certificate openssl accepts
 * and one it calls malformed. Leading zero bytes in the input are dropped,
 * since DER wants the shortest form.
 */
static void der_uint(Der *d, const unsigned char *p, size_t n)
{
    while (n > 1 && p[0] == 0x00) { p++; n--; }

    if (n == 0) {                       /* the value zero is one 0x00 byte */
        unsigned char z = 0x00;
        der_prim(d, DER_INTEGER, &z, 1);
        return;
    }

    der_raw(d, p, n);
    if (p[0] & 0x80) {
        der_byte(d, 0x00);
        der_head(d, DER_INTEGER, n + 1);
    } else {
        der_head(d, DER_INTEGER, n);
    }
}

static void der_small_int(Der *d, unsigned char v)
{
    der_prim(d, DER_INTEGER, &v, 1);
}

/*
 * BIT STRING wrapping bytes that are already whole octets, so the "unused
 * bits" octet is always zero. Both places this is needed -- a public key and a
 * signature -- are like that.
 */
static void der_bits(Der *d, const unsigned char *p, size_t n)
{
    der_raw(d, p, n);
    der_byte(d, 0x00);
    der_head(d, DER_BITSTRING, n + 1);
}

/* OID bodies, without the tag and length. */
static const unsigned char kOidRsaEncryption[] =
    { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01 };
static const unsigned char kOidSha1WithRsa[] =
    { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x05 };
static const unsigned char kOidCommonName[]      = { 0x55, 0x04, 0x03 };
static const unsigned char kOidBasicConstraints[] = { 0x55, 0x1D, 0x13 };
static const unsigned char kOidKeyUsage[]        = { 0x55, 0x1D, 0x0F };
static const unsigned char kOidSubjectAltName[]  = { 0x55, 0x1D, 0x11 };
static const unsigned char kOidExtKeyUsage[]     = { 0x55, 0x1D, 0x25 };
static const unsigned char kOidServerAuth[] =
    { 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01 };

/* AlgorithmIdentifier ::= SEQUENCE { algorithm OID, parameters NULL } */
static void der_alg(Der *d, const unsigned char *oid, size_t oid_len)
{
    size_t mark = der_len(d);

    der_prim(d, DER_NULL, NULL, 0);
    der_prim(d, DER_OID, oid, oid_len);
    der_head(d, DER_SEQUENCE, der_len(d) - mark);
}

/*
 * Name ::= SEQUENCE OF RDN, and one RDN holding one CN is all any of this
 * needs. PrintableString would be the era-correct choice but excludes
 * characters a hostname can legally contain after IDN punycoding is undone by
 * nobody; UTF8String is accepted by every client in range and by openssl.
 */
static void der_name_cn(Der *d, const char *cn)
{
    size_t outer = der_len(d);

    {
        size_t set = der_len(d);
        {
            size_t seq = der_len(d);

            der_prim(d, DER_UTF8STRING, cn, strlen(cn));
            der_prim(d, DER_OID, kOidCommonName, sizeof(kOidCommonName));
            der_head(d, DER_SEQUENCE, der_len(d) - seq);
        }
        der_head(d, DER_SET, der_len(d) - set);
    }
    der_head(d, DER_SEQUENCE, der_len(d) - outer);
}

/* SubjectPublicKeyInfo ::= SEQUENCE { AlgorithmIdentifier, BIT STRING } */
static void der_spki(Der *d, const GWCertReq *req)
{
    size_t outer = der_len(d);

    /*
     * The RSAPublicKey SEQUENCE is built into the tail of the same buffer and
     * then wrapped in a BIT STRING, which means encoding it, measuring it, and
     * putting the bit-string header on the front -- the same trick as
     * everything else here, one level deeper.
     */
    {
        size_t key = der_len(d);

        der_uint(d, req->exp, req->exp_len);
        der_uint(d, req->mod, req->mod_len);
        der_head(d, DER_SEQUENCE, der_len(d) - key);

        /* Now turn those bytes into the bit string's content. */
        der_byte(d, 0x00);
        der_head(d, DER_BITSTRING, der_len(d) - key);
    }
    der_alg(d, kOidRsaEncryption, sizeof(kOidRsaEncryption));
    der_head(d, DER_SEQUENCE, der_len(d) - outer);
}

/* Extension ::= SEQUENCE { OID, critical BOOLEAN DEFAULT FALSE, OCTET STRING } */
static void der_ext(Der *d, const unsigned char *oid, size_t oid_len,
                    int critical, const unsigned char *val, size_t val_len)
{
    size_t outer = der_len(d);

    der_prim(d, DER_OCTETSTRING, val, val_len);
    if (critical) {
        static const unsigned char kTrue = 0xFF;
        der_prim(d, 0x01, &kTrue, 1);            /* BOOLEAN TRUE */
    }
    der_prim(d, DER_OID, oid, oid_len);
    der_head(d, DER_SEQUENCE, der_len(d) - outer);
}

/*
 * The extensions, which is where a CA and a leaf actually differ.
 *
 * basicConstraints is the one that matters and is marked critical on both: a
 * client that ignored it would accept a leaf certificate as an authority, and
 * then any host Gateway had ever served could sign for any other.
 *
 * subjectAltName is written for the leaf even though the clients this exists
 * for do not read it -- IE 4 and Netscape 4 match the CN. It costs a few bytes
 * and means a certificate Gateway produced is not rejected out of hand by
 * anything newer that stopped reading the CN in 2017.
 */
static void der_extensions(Der *d, const GWCertReq *req)
{
    size_t wrap = der_len(d);
    size_t seq;

    seq = der_len(d);

    if (req->is_ca) {
        /* keyUsage: keyCertSign | cRLSign. Bit 5 and bit 6 of the first octet,
         * counted from the most significant end, so 0x06 with one unused bit. */
        static const unsigned char kCaUsage[]  = { 0x03, 0x02, 0x01, 0x06 };
        /* BasicConstraints ::= SEQUENCE { cA TRUE } */
        static const unsigned char kCaBasic[]  = { 0x30, 0x03, 0x01, 0x01, 0xFF };

        der_ext(d, kOidKeyUsage, sizeof(kOidKeyUsage), 1,
                kCaUsage, sizeof(kCaUsage));
        der_ext(d, kOidBasicConstraints, sizeof(kOidBasicConstraints), 1,
                kCaBasic, sizeof(kCaBasic));
    } else {
        /* digitalSignature | keyEncipherment: 0xA0, three unused bits. */
        static const unsigned char kSrvUsage[] = { 0x03, 0x02, 0x05, 0xA0 };
        static const unsigned char kNotCa[]    = { 0x30, 0x00 };

        /* extKeyUsage ::= SEQUENCE OF OID, holding id-kp-serverAuth. */
        {
            size_t eku = der_len(d);
            size_t body;

            der_prim(d, DER_OID, kOidServerAuth, sizeof(kOidServerAuth));
            der_head(d, DER_SEQUENCE, der_len(d) - eku);
            body = der_len(d) - eku;
            der_head(d, DER_OCTETSTRING, body);
            der_prim(d, DER_OID, kOidExtKeyUsage, sizeof(kOidExtKeyUsage));
            der_head(d, DER_SEQUENCE, der_len(d) - eku);
        }

        /* GeneralNames ::= SEQUENCE OF GeneralName; dNSName is [2] IA5String. */
        {
            size_t san = der_len(d);
            size_t body;

            der_prim(d, 0x82, req->cn, strlen(req->cn));
            der_head(d, DER_SEQUENCE, der_len(d) - san);
            body = der_len(d) - san;
            der_head(d, DER_OCTETSTRING, body);
            der_prim(d, DER_OID, kOidSubjectAltName, sizeof(kOidSubjectAltName));
            der_head(d, DER_SEQUENCE, der_len(d) - san);
        }

        der_ext(d, kOidKeyUsage, sizeof(kOidKeyUsage), 1,
                kSrvUsage, sizeof(kSrvUsage));
        der_ext(d, kOidBasicConstraints, sizeof(kOidBasicConstraints), 1,
                kNotCa, sizeof(kNotCa));
    }

    der_head(d, DER_SEQUENCE, der_len(d) - seq);
    der_head(d, DER_CTX(3), der_len(d) - wrap);
}

size_t gw_x509_tbs(const GWCertReq *req, unsigned char *out, size_t cap,
                   size_t *off)
{
    Der    d;
    size_t mark;

    if (req == NULL || out == NULL || off == NULL) return 0;
    if (req->cn == NULL || req->issuer_cn == NULL) return 0;
    if (req->mod == NULL || req->mod_len == 0) return 0;
    if (req->exp == NULL || req->exp_len == 0) return 0;
    if (req->serial == NULL || req->serial_len == 0) return 0;
    if (req->not_before == NULL || strlen(req->not_before) != 13) return 0;
    if (req->not_after == NULL || strlen(req->not_after) != 13) return 0;

    der_init(&d, out, cap);
    mark = der_len(&d);

    /* Reverse order of the TBSCertificate fields, which is how this reads. */
    der_extensions(&d, req);
    der_spki(&d, req);
    der_name_cn(&d, req->cn);
    {
        size_t v = der_len(&d);
        der_prim(&d, DER_UTCTIME, req->not_after, 13);
        der_prim(&d, DER_UTCTIME, req->not_before, 13);
        der_head(&d, DER_SEQUENCE, der_len(&d) - v);
    }
    der_name_cn(&d, req->issuer_cn);
    der_alg(&d, kOidSha1WithRsa, sizeof(kOidSha1WithRsa));
    der_uint(&d, req->serial, req->serial_len);
    {
        /* version [0] EXPLICIT INTEGER { v3(2) } */
        size_t v = der_len(&d);
        der_small_int(&d, 2);
        der_head(&d, DER_CTX(0), der_len(&d) - v);
    }
    der_head(&d, DER_SEQUENCE, der_len(&d) - mark);

    if (d.err) return 0;
    *off = d.at;
    return der_len(&d);
}

size_t gw_x509_cert(const unsigned char *tbs, size_t tbs_len,
                    const unsigned char *sig, size_t sig_len,
                    unsigned char *out, size_t cap, size_t *off)
{
    Der    d;
    size_t mark;

    if (tbs == NULL || tbs_len == 0 || sig == NULL || sig_len == 0) return 0;
    if (out == NULL || off == NULL) return 0;

    der_init(&d, out, cap);
    mark = der_len(&d);

    der_bits(&d, sig, sig_len);
    der_alg(&d, kOidSha1WithRsa, sizeof(kOidSha1WithRsa));
    der_raw(&d, tbs, tbs_len);                   /* already a SEQUENCE */
    der_head(&d, DER_SEQUENCE, der_len(&d) - mark);

    if (d.err) return 0;
    *off = d.at;
    return der_len(&d);
}
