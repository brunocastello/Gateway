/*
 * gw_skexport.h - wire encoding for the RSA_EXPORT ServerKeyExchange
 * (PATCHES.md section 38).
 *
 * PORTABLE: no Mac or Windows headers, and no BearSSL either -- these
 * functions only lay out bytes. The RSA keygen, MD5/SHA1 hashing and
 * PKCS#1 signing that feed them are BearSSL's job and live in
 * third_party/certainly/bearssl/src/ssl/ssl_hs_server.t0 (do_rsa_export_ske),
 * which calls straight into this file rather than duplicating the framing.
 *
 * RFC 6101 s5.6.3/s5.6.7 (SSL 3.0) and RFC 2246 s7.4.3 (TLS 1.0) define
 * the same message for the RSA_EXPORT suites:
 *
 *   ServerRSAParams { opaque rsa_modulus<1..2^16-1>;
 *                      opaque rsa_exponent<1..2^16-1>; }
 *
 * signed the "old style" way -- MD5(client_random + server_random +
 * params) concatenated with SHA1 of the same, 36 bytes, no ASN.1
 * DigestInfo -- with the signature itself carrying its own 2-byte
 * length prefix. That last part is what the last attempt at this
 * message (removed in 5e13663) got wrong: it wrote the signature with
 * no length prefix at all.
 */
#ifndef GW_SKEXPORT_H
#define GW_SKEXPORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Write ServerRSAParams: a 2-byte modulus length, the modulus, a 2-byte
 * exponent length, the exponent. Neither value is padded or trimmed --
 * whatever the caller hands in is written as given, so a modulus with a
 * leading zero byte would be written with one; RSA keygen never
 * produces one for the top byte of a key of the stated bit length, but
 * this function does not itself enforce that.
 *
 * Returns the number of bytes written (2 + mod_len + 2 + exp_len), or 0
 * if that does not fit in cap.
 */
size_t gw_ske_write_params(const unsigned char *mod, size_t mod_len,
                           const unsigned char *exp, size_t exp_len,
                           unsigned char *out, size_t cap);

/*
 * Assemble the bytes the signature is computed over: client_random (32
 * bytes) + server_random (32 bytes) + params, exactly as
 * gw_ske_write_params wrote them (the message's 4-byte handshake header
 * is not part of this). Returns the number of bytes written (64 +
 * params_len), or 0 if that does not fit in cap.
 */
size_t gw_ske_write_signed_input(const unsigned char client_random[32],
                                 const unsigned char server_random[32],
                                 const unsigned char *params,
                                 size_t params_len,
                                 unsigned char *out, size_t cap);

/*
 * Wrap params (as gw_ske_write_params wrote them) and an already
 * computed signature into a complete ServerKeyExchange handshake
 * message: a 4-byte header (handshake type 12, 3-byte big-endian
 * length), the params, a 2-byte signature length, and the signature.
 * params and sig may alias into the same buffer out already occupies
 * (the call sites write params in place and then wrap them).
 *
 * Returns the total number of bytes written, header included (4 +
 * params_len + 2 + sig_len), or 0 if that does not fit in cap.
 */
size_t gw_ske_write_message(const unsigned char *params, size_t params_len,
                            const unsigned char *sig, size_t sig_len,
                            unsigned char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* GW_SKEXPORT_H */
