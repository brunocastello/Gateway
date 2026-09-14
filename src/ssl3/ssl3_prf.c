/*
 * ssl3_prf.c — SSL 3.0 PRF implementation.
 *
 * SSL 3.0 PRF is defined in RFC 6101, section 5:
 *   P_hash(secret, seed) = HASH(secret || seed) || HASH(secret || HASH(secret || seed)) || ...
 * where HASH is MD5 or SHA-1, and the iteration uses
 *   A(0) = seed
 *   A(i) = HASH(secret || A(i-1))
 *
 * The PRF output is P_MD5 XOR P_SHA1, same size as the hash output.
 */

#include "ssl3.h"
#include "bearssl_prf.h"
#include "inner.h"

#include <string.h>
#include <stdlib.h>

void
ssl3_phash(void *dst, size_t len,
	const br_hash_class *dig, size_t hash_size,
	const void *secret, size_t secret_len,
	const unsigned char *seed, size_t seed_len)
{
	unsigned char *buf = (unsigned char *)dst;
	unsigned char A[64];
	unsigned char tmp[128];
	unsigned char hash_out[64];
	size_t hlen;
	size_t off = 0;
	br_md5_context md5_ctx;
	br_sha1_context sha1_ctx;

	hlen = hash_size;
	if (hlen > sizeof A)
		return;

	memcpy(A, seed, seed_len);

	while (off < len) {
		size_t i;

		/* tmp = secret || A */
		memcpy(tmp, secret, secret_len);
		memcpy(tmp + secret_len, A, hlen);

		/* hash_out = HASH(secret || A) */
		if (dig == &br_md5_vtable) {
			br_md5_init(&md5_ctx);
			br_md5_update(&md5_ctx, tmp, secret_len + hlen);
			br_md5_out(&md5_ctx, hash_out);
		} else {
			br_sha1_init(&sha1_ctx);
			br_sha1_update(&sha1_ctx, tmp, secret_len + hlen);
			br_sha1_out(&sha1_ctx, hash_out);
		}

		/* A = HASH(secret || A) = hash_out */
		memcpy(A, hash_out, hlen);

		/* XOR hash_out into dst */
		for (i = 0; i < hlen && off < len; i++) {
			buf[off++] ^= hash_out[i];
		}
	}
}

void
ssl3_prf(void *dst, size_t len,
	const void *secret, size_t secret_len, const char *label,
	size_t seed_num, const br_tls_prf_seed_chunk *seed)
{
	unsigned char *seed_buf;
	size_t seed_len = 0;
	size_t label_len;
	size_t u;

	/* Compute total seed length: label + all seed chunks */
	for (label_len = 0; label[label_len]; label_len++)
		;
	seed_len = label_len;
	for (u = 0; u < seed_num; u++)
		seed_len += seed[u].len;

	/* Build seed buffer */
	seed_buf = malloc(seed_len);
	if (seed_buf == NULL)
		return;
	memcpy(seed_buf, label, label_len);
	for (u = 0, seed_len = label_len; u < seed_num; u++) {
		memcpy(seed_buf + seed_len, seed[u].data, seed[u].len);
		seed_len += seed[u].len;
	}

	/* Compute P_MD5 XOR P_SHA1 */
	memset(dst, 0, len);
	ssl3_phash(dst, len, &br_md5_vtable, br_digest_size(&br_md5_vtable), secret, secret_len, seed_buf, seed_len);
	ssl3_phash(dst, len, &br_sha1_vtable, br_digest_size(&br_sha1_vtable), secret, secret_len, seed_buf, seed_len);

	free(seed_buf);
}
