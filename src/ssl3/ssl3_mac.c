/*
 * ssl3_mac.c — SSL 3.0 MAC implementation (RFC 6101, section 5.2.3).
 *
 *   MAC_hash(secret, seq, type, len, data) =
 *     HASH(secret || pad2 || HASH(secret || pad1
 *          || seq || type || len || data))
 *
 * where HASH is MD5 (16-byte output) or SHA-1 (20-byte output) according
 * to the cipher suite, pad1 is 0x36 repeated 48 bytes for MD5 (40 for
 * SHA-1), and pad2 is 0x5C repeated likewise. This is fundamentally
 * different from HMAC (the secret prefixes both hashes, and the pads
 * sit between secret and data rather than being XORed into a key).
 *
 * Only the negotiated hash is computed; exactly its output length is
 * written to mac.
 */

#include "ssl3.h"
#include "inner.h"

#include <string.h>

void
ssl3_mac(const br_hash_class *hash,
	const void *secret, size_t secret_len,
	uint64_t seq, unsigned char type, unsigned char *len,
	const unsigned char *data, size_t data_len,
	unsigned char *mac)
{
	unsigned char seq_buf[8];
	unsigned char inner[20];
	unsigned char pad[48];
	size_t pad_len, hlen, i;
	br_md5_context md5_ctx;
	br_sha1_context sha1_ctx;
	int is_md5 = (hash == &br_md5_vtable);

	if (is_md5) {
		pad_len = 48;
		hlen = 16;
	} else {
		pad_len = 40;
		hlen = 20;
	}

	/* Encode sequence as 8-byte big-endian */
	for (i = 0; i < 8; i++) {
		seq_buf[i] = (unsigned char)((seq >> (56 - 8 * i)) & 0xFF);
	}

	/* inner = HASH(secret || pad1 || seq || type || len || data) */
	memset(pad, 0x36, pad_len);
	if (is_md5) {
		br_md5_init(&md5_ctx);
		br_md5_update(&md5_ctx, secret, secret_len);
		br_md5_update(&md5_ctx, pad, pad_len);
		br_md5_update(&md5_ctx, seq_buf, 8);
		br_md5_update(&md5_ctx, &type, 1);
		br_md5_update(&md5_ctx, len, 2);
		br_md5_update(&md5_ctx, data, data_len);
		br_md5_out(&md5_ctx, inner);
	} else {
		br_sha1_init(&sha1_ctx);
		br_sha1_update(&sha1_ctx, secret, secret_len);
		br_sha1_update(&sha1_ctx, pad, pad_len);
		br_sha1_update(&sha1_ctx, seq_buf, 8);
		br_sha1_update(&sha1_ctx, &type, 1);
		br_sha1_update(&sha1_ctx, len, 2);
		br_sha1_update(&sha1_ctx, data, data_len);
		br_sha1_out(&sha1_ctx, inner);
	}

	/* mac = HASH(secret || pad2 || inner) */
	memset(pad, 0x5C, pad_len);
	if (is_md5) {
		br_md5_init(&md5_ctx);
		br_md5_update(&md5_ctx, secret, secret_len);
		br_md5_update(&md5_ctx, pad, pad_len);
		br_md5_update(&md5_ctx, inner, hlen);
		br_md5_out(&md5_ctx, mac);
	} else {
		br_sha1_init(&sha1_ctx);
		br_sha1_update(&sha1_ctx, secret, secret_len);
		br_sha1_update(&sha1_ctx, pad, pad_len);
		br_sha1_update(&sha1_ctx, inner, hlen);
		br_sha1_out(&sha1_ctx, mac);
	}
}
