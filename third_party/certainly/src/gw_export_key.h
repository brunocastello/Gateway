/*
 * gw_export_key.h - the temporary RSA key for SSL 3.0 / TLS 1.0 export
 * suites (PATCHES.md §38).
 *
 * RSA_EXPORT ServerKeyExchange (RFC 6101 s5.6.3/s5.6.7, RFC 2246 s7.4.3)
 * needs a temporary RSA key of at most 512 bits whenever the certificate
 * key is bigger, so a 40-bit-only client (no suite but 0x0003/0x0006)
 * gets something it is allowed to encrypt a pre-master secret to. This
 * generates one lazily, on the first handshake that needs it, and keeps
 * it for the rest of the process -- both RFCs permit reuse of the
 * temporary key across handshakes, and generating a fresh one each time
 * would stall the cooperative loop for no client that exists.
 */
#ifndef GW_EXPORT_KEY_H
#define GW_EXPORT_KEY_H

#include <bearssl.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns the temporary key, generating it on the first call, and hands
 * back its modulus and public exponent (no leading zero byte, as keygen
 * produces them). Returns NULL -- and leaves the out parameters
 * untouched -- when keygen failed once and every call thereafter: there
 * is no key to retry with, and no export handshake can be served, same
 * as before this existed.
 */
const br_rsa_private_key *gw_export_key_get(const unsigned char **mod,
    size_t *mod_len, const unsigned char **exp, size_t *exp_len);

#ifdef __cplusplus
}
#endif

#endif /* GW_EXPORT_KEY_H */
