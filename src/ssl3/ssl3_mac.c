/*
 * ssl3_mac.c — SSL 3.0 MAC implementation.
 *
 * SSL 3.0 MAC (RFC 6101, section 3) is defined as:
 *   MAC_hash(seq, type, len, data) =
 *     MD5(pad1 || hash(pad2 || seq || type || len || data))
 *     || SHA1(pad1 || hash(pad2 || seq || type || len || data))
 * where pad1 = 0x36 repeated 48 bytes, pad2 = 0x5C repeated 48 bytes.
 * The secret is NOT part of the hash chain (it is used in key
 * derivation only). This is fundamentally different from HMAC.
 *
 * The output is 16 + 20 = 36 bytes.
 */

#include "ssl3.h"
#include "inner.h"

#include <string.h>

static const unsigned char pad1[48] = {
	0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,
	0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,
	0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,
	0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,
	0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,
	0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,
};

static const unsigned char pad2[48] = {
	0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,
	0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,
	0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,
	0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,
	0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,
};

void
ssl3_mac(const void *secret, size_t secret_len,
	uint64_t seq, unsigned char type, unsigned char *len,
	const unsigned char *data, size_t data_len,
	unsigned char *mac)
{
	unsigned char len_buf[2];
	unsigned char seq_buf[8];
	unsigned char inner_hash[64];
	size_t hlen;
	br_md5_context md5_ctx;
	br_sha1_context sha1_ctx;
	size_t i;

	(void)secret;
	(void)secret_len;

	/* Encode length as 2-byte big-endian */
	len_buf[0] = len[0];
	len_buf[1] = len[1];

	/* Encode sequence as 8-byte big-endian */
	for (i = 0; i < 8; i++) {
		seq_buf[i] = (seq >> (56 - 8 * i)) & 0xFF;
	}

	/* Inner hash: hash(pad2 || seq || type || len || data) */
	br_md5_init(&md5_ctx);
	br_md5_update(&md5_ctx, pad2, 48);
	br_md5_update(&md5_ctx, seq_buf, 8);
	br_md5_update(&md5_ctx, &type, 1);
	br_md5_update(&md5_ctx, len_buf, 2);
	br_md5_update(&md5_ctx, data, data_len);
	br_md5_out(&md5_ctx, inner_hash);
	hlen = br_digest_size(&br_md5_vtable);

	/* Outer hash: MD5(pad1 || inner_hash) */
	br_md5_init(&md5_ctx);
	br_md5_update(&md5_ctx, pad1, 48);
	br_md5_update(&md5_ctx, inner_hash, hlen);
	br_md5_out(&md5_ctx, mac);

	/* Inner hash: SHA1(pad2 || seq || type || len || data) */
	br_sha1_init(&sha1_ctx);
	br_sha1_update(&sha1_ctx, pad2, 48);
	br_sha1_update(&sha1_ctx, seq_buf, 8);
	br_sha1_update(&sha1_ctx, &type, 1);
	br_sha1_update(&sha1_ctx, len_buf, 2);
	br_sha1_update(&sha1_ctx, data, data_len);
	br_sha1_out(&sha1_ctx, inner_hash);
	hlen = br_digest_size(&br_sha1_vtable);

	/* Outer hash: SHA1(pad1 || inner_hash) */
	br_sha1_init(&sha1_ctx);
	br_sha1_update(&sha1_ctx, pad1, 48);
	br_sha1_update(&sha1_ctx, inner_hash, hlen);
	br_sha1_out(&sha1_ctx, mac + 16);
}
