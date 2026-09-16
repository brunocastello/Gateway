/*
 * ssl3.h — Public API for the minimal SSL 3.0 engine.
 *
 * This module implements SSL 3.0 record-layer cryptography for vintage
 * browsers (Netscape 3, IE 3/4) that can only speak SSL 3.0, on top of
 * BearSSL hash primitives. RC4 record handling lives in BearSSL proper
 * (br_sslrec_in/out_rc4); RC2-CBC and standalone RC4 primitives live in
 * ssl3_rc2.c and ssl3_rc4.c.
 *
 * Key derivation follows RFC 6101 section 6 (A/BB/CCC letter pads),
 * NOT the P_MD5-XOR-P_SHA1 PRF — that construction is TLS 1.0's.
 *
 * The SSL 3.0 record MAC (RFC 6101 section 5.2.3.1) is:
 *   MAC = HASH(secret || pad2 || HASH(secret || pad1
 *          || seq || type || len || data))
 * with 48-byte pads for MD5 and 40-byte pads for SHA-1.
 */

#ifndef GW_SSL3_H
#define GW_SSL3_H

#include <stddef.h>
#include <stdint.h>

#include "bearssl_ssl.h"
#include "bearssl_prf.h"

/*
 * SSL 3.0 master secret (RFC 6101 section 6.1):
 *   MD5(pms + SHA('A'   + pms + cli + srv)) +
 *   MD5(pms + SHA('BB'  + pms + cli + srv)) +
 *   MD5(pms + SHA('CCC' + pms + cli + srv))
 */
void ssl3_master_secret(unsigned char out[48],
	const void *pms, size_t pms_len,
	const unsigned char cli[32], const unsigned char srv[32]);

/*
 * SSL 3.0 key block (RFC 6101 section 6.2.2): iterated
 *   MD5(secret + SHA(pad + secret + srv + cli))
 * with pads 'A', 'BB', 'CCC', ... until len bytes are produced.
 * Note the reversed random order versus the master secret; both
 * functions take (cli, srv) and reorder internally, so call sites
 * always pass client_random first.
 */
void ssl3_key_block(unsigned char *out, size_t len,
	const void *secret, size_t secret_len,
	const unsigned char cli[32], const unsigned char srv[32]);

/*
 * SSL 3.0 MAC (RFC 6101, section 5.2.3) with the negotiated hash:
 *   MAC = HASH(secret || pad2 || HASH(secret || pad1
 *          || seq || type || len || data))
 * with 48-byte pads for MD5 and 40-byte pads for SHA-1. Writes exactly
 * the hash output length (16 for MD5, 20 for SHA-1) to mac.
 */
void ssl3_mac(const br_hash_class *hash,
	const void *secret, size_t secret_len,
	uint64_t seq, unsigned char type, unsigned char *len,
	const unsigned char *data, size_t data_len,
	unsigned char *mac);

/*
 * Initialize the SSL 3.0 server context.
 * Widens the BearSSL full-RSA profile down to SSL 3.0 (versions
 * 0x0300..TLS 1.2) and appends the SSL 3.0 export suites. The TLS
 * PRFs stay installed: SSL 3.0 derivation is selected by version at
 * the compute_master / compute_key_block call sites, never by
 * replacing prf10 (which TLS 1.0/1.1 still need).
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
 * RC2-CBC cipher primitive. Effective bits is 40 for export suites
 * (RC2_40) and 128 otherwise; when 0, key_len*8 is used.
 */
void *rc2_setup(const unsigned char *key, size_t key_len);
void *rc2_setup_bits(const unsigned char *key, size_t key_len, unsigned effective_bits);
void rc2_cbc_encrypt(void *ctx, const unsigned char *iv, unsigned char *data, size_t len);
void rc2_cbc_decrypt(void *ctx, const unsigned char *iv, unsigned char *data, size_t len);
void rc2_cleanup(void *ctx);

#endif /* GW_SSL3_H */
