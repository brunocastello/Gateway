/*
 * ssl3.h — Public API for the minimal SSL 3.0 engine.
 *
 * This module implements the SSL 3.0 record layer, PRF, MAC, and
 * Finished message for vintage browsers (Netscape 3, IE 3/4) that
 * can only speak SSL 3.0. It uses BearSSL for SHA-1 and MD5
 * hash primitives. RC2-CBC and RC4 are implemented separately
 * in ssl3_rc2.c and ssl3_rc4.c.
 *
 * The SSL 3.0 MAC is concat-based (not HMAC):
 *   MAC_hash(secret, seq, type, len, data) =
 *     MD5(pad1 || hash(pad2 || seq || type || len || data))
 *     || SHA1(pad1 || hash(pad2 || seq || type || len || data))
 *
 * The SSL 3.0 PRF is:
 *   P_hash(secret, seed) = HASH(secret || seed) || HASH(secret || HASH(secret || seed)) || ...
 *   PRF = P_MD5 XOR P_SHA1
 */

#ifndef GW_SSL3_H
#define GW_SSL3_H

#include <stddef.h>
#include <stdint.h>

#include "bearssl_ssl.h"
#include "bearssl_prf.h"

/*
 * SSL 3.0 PRF. Computes P_MD5(secret, label+seed) XOR P_SHA1(secret, label+seed).
 */
void ssl3_prf(void *dst, size_t len,
	const void *secret, size_t secret_len, const char *label,
	size_t seed_num, const br_tls_prf_seed_chunk *seed);

/*
 * SSL 3.0 P_hash: HASH(secret || seed) || HASH(secret || HASH(secret || seed)) || ...
 * Used for key block derivation.
 */
void ssl3_phash(void *dst, size_t len,
	const br_hash_class *dig, size_t hash_size,
	const void *secret, size_t secret_len,
	const unsigned char *seed, size_t seed_len);

/*
 * SSL 3.0 MAC. Returns 36 bytes (16 MD5 + 20 SHA-1).
 * The secret is NOT used in the hash chain (it is used in key derivation only).
 */
void ssl3_mac(const void *secret, size_t secret_len,
	uint64_t seq, unsigned char type, unsigned char *len,
	const unsigned char *data, size_t data_len,
	unsigned char *mac);

/*
 * SSL 3.0 Finished message computation.
 */
int ssl3_finished(const br_ssl_engine_context *cc,
	int is_client, unsigned char *verify_data, size_t *verify_len);

/*
 * Initialize the SSL 3.0 server context.
 * Sets version to 0x0300, installs the SSL 3.0 PRF.
 * Must be called after br_ssl_server_init_full_rsa().
 */
void ssl3_server_init(br_ssl_server_context *sc);

/*
 * Register RC2-CBC and RC4 cipher suites with the SSL 3.0 engine.
 */
void ssl3_register_ciphers(br_ssl_engine_context *cc);

/*
 * RC4 cipher primitive.
 */
void *rc4_setup(const unsigned char *key, size_t key_len);
void rc4_crypt(void *ctx, const unsigned char *input, unsigned char *output, size_t len);
void rc4_cleanup(void *ctx);

/*
 * RC2-CBC cipher primitive.
 */
void *rc2_setup(const unsigned char *key, size_t key_len);
void rc2_cbc_encrypt(void *ctx, const unsigned char *iv, unsigned char *data, size_t len);
void rc2_cbc_decrypt(void *ctx, const unsigned char *iv, unsigned char *data, size_t len);
void rc2_cleanup(void *ctx);

#endif /* GW_SSL3_H */
