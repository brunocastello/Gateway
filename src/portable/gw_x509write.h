/*
 * gw_x509write.h - emitting X.509 certificates, which BearSSL only reads.
 *
 * Gateway needs this to terminate TLS on the *browser's* side of a CONNECT.
 * Internet Explorer 4 asked for https://host and will not accept plaintext in
 * the tunnel, so Gateway has to present a certificate for that host -- and
 * since no real authority will sign one for us, Gateway signs it itself with a
 * CA it generates on the machine it is running on. Nothing outside the vintage
 * machine is involved at any point.
 *
 * Deliberately no signing here. These functions build byte strings and nothing
 * else, so they stay in src/portable and can be tested by a plain Linux cc --
 * and, more to the point, checked against openssl in CI, which is worth far
 * more for hand-written DER than any assertion this project could make about
 * its own output. The SHA-1 and the RSA live with BearSSL in the caller.
 *
 * SHA-1 because the clients this exists for predate SHA-2 entirely: IE 4 and
 * Netscape 4 cannot verify a sha256WithRSAEncryption signature and will reject
 * the certificate rather than warn about it. It is the weakest thing here and
 * it is not a real weakness: a forged certificate would have to be presented
 * over a loopback connection on a machine the attacker is already running on.
 */
#ifndef GW_X509WRITE_H
#define GW_X509WRITE_H

#include <stddef.h>

/*
 * What to put in a certificate.
 *
 * The public key is passed as raw big-endian modulus and exponent -- the form
 * BearSSL's br_rsa_public_key already holds -- so no key format is invented
 * here on the way through.
 *
 * Times are UTCTime bodies: exactly "YYMMDDHHMMSSZ", 13 characters. UTCTime
 * rather than GeneralizedTime because these clients expect it, which also caps
 * the useful range at 2049; notAfter should stay below 2038 anyway, since a
 * 32-bit time_t on the machines in question cannot represent anything later.
 */
typedef struct {
    const char          *cn;            /* subject common name */
    const char          *issuer_cn;     /* the CA's common name */
    int                  is_ca;         /* 1: self-signed CA. 0: server leaf */
    const unsigned char *serial;        /* big-endian, non-empty */
    size_t               serial_len;
    const unsigned char *mod;           /* subject key modulus, big-endian */
    size_t               mod_len;
    const unsigned char *exp;           /* subject key public exponent */
    size_t               exp_len;
    const char          *not_before;    /* "YYMMDDHHMMSSZ" */
    const char          *not_after;
} GWCertReq;

/*
 * Build the TBSCertificate -- the part that gets hashed and signed.
 *
 * Written into the END of `out`, because DER puts a length before its content
 * and encoding backwards is what makes that a single pass instead of a pass
 * with holes in it to fill later. `*off` comes back as the offset where the
 * encoding starts; the bytes to hash are out + *off, for the returned length.
 *
 * Returns the length, or 0 if it would not fit or an argument is unusable.
 */
size_t gw_x509_tbs(const GWCertReq *req, unsigned char *out, size_t cap,
                   size_t *off);

/*
 * Wrap a TBSCertificate and its RSA signature into a Certificate. Same
 * end-of-buffer convention and the same return.
 *
 * The signature must be over the exact bytes gw_x509_tbs() produced, as
 * PKCS#1 v1.5 with SHA-1, which is what the algorithm identifier written here
 * claims. Nothing checks that claim; it is the caller's to keep.
 */
size_t gw_x509_cert(const unsigned char *tbs, size_t tbs_len,
                    const unsigned char *sig, size_t sig_len,
                    unsigned char *out, size_t cap, size_t *off);

#endif /* GW_X509WRITE_H */
