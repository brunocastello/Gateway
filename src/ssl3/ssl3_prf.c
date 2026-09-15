/*
 * ssl3_prf.c — SSL 3.0 key derivation (RFC 6101, section 6).
 *
 * SSL 3.0 has no generic PRF. The master secret and key block use
 * dedicated MD5/SHA-1 constructions with 'A'/'BB'/'CCC'... letter pads:
 *
 *   master_secret (48 bytes) =
 *     MD5(secret + SHA('A'   + secret + CliRnd + SrvRnd)) +
 *     MD5(secret + SHA('BB'  + secret + CliRnd + SrvRnd)) +
 *     MD5(secret + SHA('CCC' + secret + CliRnd + SrvRnd))
 *
 *   key_block (as many bytes as needed) =
 *     MD5(secret + SHA('A'    + secret + SrvRnd + CliRnd)) +
 *     MD5(secret + SHA('BB'   + secret + SrvRnd + CliRnd)) + ...
 *
 * This file still exports the BearSSL br_tls_prf_impl signature as
 * ssl3_prf() (installed as prf10 for SSL 3.0 sessions, which never
 * reach TLS 1.2 so the PRF id is irrelevant) and dispatches on the
 * label BearSSL passes: "master secret" and "key expansion" use the
 * true constructions above (seed chunks arrive as {Cli,Srv} and
 * {Srv,Cli} respectively, matching BearSSL's compute_master and
 * compute_key_block callers). Anything else (notably the Finished
 * labels, whose SSL 3.0 construction needs the raw transcript rather
 * than digests) falls back to the legacy P_MD5-XOR-P_SHA1 helper
 * below; Finished verification is handled separately.
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
	unsigned char A[128];
	unsigned char tmp[128];
	unsigned char hash_out[64];
	size_t hlen;
	size_t off = 0;
	br_md5_context md5_ctx;
	br_sha1_context sha1_ctx;

	hlen = hash_size;
	if (hlen > sizeof A || seed_len > sizeof A
		|| secret_len + seed_len > sizeof tmp)
		return;

	memcpy(A, seed, seed_len);

	{
	size_t alen = seed_len;
	while (off < len) {
		size_t i;

		/* tmp = secret || A */
		memcpy(tmp, secret, secret_len);
		memcpy(tmp + secret_len, A, alen);

		/* hash_out = HASH(secret || A) */
		if (dig == &br_md5_vtable) {
			br_md5_init(&md5_ctx);
			br_md5_update(&md5_ctx, tmp, secret_len + alen);
			br_md5_out(&md5_ctx, hash_out);
		} else {
			br_sha1_init(&sha1_ctx);
			br_sha1_update(&sha1_ctx, tmp, secret_len + alen);
			br_sha1_out(&sha1_ctx, hash_out);
		}

		/* A = HASH(secret || A) = hash_out */
		memcpy(A, hash_out, hlen);
		alen = hlen;

		/* XOR hash_out into dst */
		for (i = 0; i < hlen && off < len; i++) {
			buf[off++] ^= hash_out[i];
		}
	}
	}
}

void
ssl3_prf(void *dst, size_t len,
	const void *secret, size_t secret_len, const char *label,
	size_t seed_num, const br_tls_prf_seed_chunk *seed)
{
	unsigned char *buf = (unsigned char *)dst;
	br_md5_context md5_ctx;
	br_sha1_context sha1_ctx;
	unsigned char sh[20];
	unsigned char md[16];
	unsigned char pad[16];
	size_t off;
	int round;

	if (label != NULL && seed_num == 2
		&& seed[0].len == 32 && seed[1].len == 32
		&& secret_len == 48
		&& strcmp(label, "master secret") == 0 && len == 48)
	{
		/* True SSL 3.0 master secret: chunks are {CliRnd, SrvRnd}. */
		for (round = 0; round < 3; round++) {
			int i;
			for (i = 0; i <= round; i++)
				pad[i] = (unsigned char)('A' + round);
			br_sha1_init(&sha1_ctx);
			br_sha1_update(&sha1_ctx, pad, (size_t)round + 1);
			br_sha1_update(&sha1_ctx, secret, secret_len);
			br_sha1_update(&sha1_ctx, seed[0].data, seed[0].len);
			br_sha1_update(&sha1_ctx, seed[1].data, seed[1].len);
			br_sha1_out(&sha1_ctx, sh);
			br_md5_init(&md5_ctx);
			br_md5_update(&md5_ctx, secret, secret_len);
			br_md5_update(&md5_ctx, sh, sizeof sh);
			br_md5_out(&md5_ctx, buf + (size_t)round * 16);
		}
		return;
	}
	if (label != NULL && seed_num == 2
		&& seed[0].len == 32 && seed[1].len == 32
		&& secret_len == 48
		&& strcmp(label, "key expansion") == 0)
	{
		/* True SSL 3.0 key block: chunks are {SrvRnd, CliRnd}. */
		off = 0;
		round = 0;
		while (off < len && round < 16) {
			size_t n = len - off;
			int i;
			if (n > 16)
				n = 16;
			for (i = 0; i <= round; i++)
				pad[i] = (unsigned char)('A' + round);
			br_sha1_init(&sha1_ctx);
			br_sha1_update(&sha1_ctx, pad, (size_t)round + 1);
			br_sha1_update(&sha1_ctx, secret, secret_len);
			br_sha1_update(&sha1_ctx, seed[0].data, seed[0].len);
			br_sha1_update(&sha1_ctx, seed[1].data, seed[1].len);
			br_sha1_out(&sha1_ctx, sh);
			br_md5_init(&md5_ctx);
			br_md5_update(&md5_ctx, secret, secret_len);
			br_md5_update(&md5_ctx, sh, sizeof sh);
			br_md5_out(&md5_ctx, md);
			memcpy(buf + off, md, n);
			off += n;
			round++;
		}
		return;
	}

	{
	unsigned char *seed_buf;
	size_t seed_len = 0;
	size_t label_len;
	size_t u;

	/* Legacy fallback (Finished labels): P_MD5 XOR P_SHA1. The real
	 * SSL 3.0 Finished construction needs the raw transcript and is
	 * handled at the record layer instead. */
	if (label == NULL)
		return;
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
}
