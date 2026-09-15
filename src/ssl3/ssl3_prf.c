/*
 * ssl3_prf.c — SSL 3.0 key derivation (RFC 6101, section 6).
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
 * Note the random order differs between the two (client-first for the
 * master secret, server-first for the key block); both functions take
 * (cli, srv) and the key block reorders internally, so every call site
 * passes client_random first. Corroborated against OpenSSL 1.0.1e
 * (ssl3_generate_master_secret, ssl3_generate_key_block) and against
 * live vintage peers.
 */

#include "ssl3.h"
#include "bearssl_prf.h"
#include "inner.h"

#include <string.h>

/* Letter pad for round r: ('A'+r) repeated (r+1) times. */
static void
ssl3_round_pad(unsigned char *pad, int round)
{
	int i;

	for (i = 0; i <= round; i++)
		pad[i] = (unsigned char)('A' + round);
}

void
ssl3_master_secret(unsigned char out[48],
	const void *pms, size_t pms_len,
	const unsigned char cli[32], const unsigned char srv[32])
{
	br_md5_context md5_ctx;
	br_sha1_context sha1_ctx;
	unsigned char sh[20];
	unsigned char pad[3];
	int round;

	for (round = 0; round < 3; round++) {
		ssl3_round_pad(pad, round);
		br_sha1_init(&sha1_ctx);
		br_sha1_update(&sha1_ctx, pad, (size_t)round + 1);
		br_sha1_update(&sha1_ctx, pms, pms_len);
		br_sha1_update(&sha1_ctx, cli, 32);
		br_sha1_update(&sha1_ctx, srv, 32);
		br_sha1_out(&sha1_ctx, sh);
		br_md5_init(&md5_ctx);
		br_md5_update(&md5_ctx, pms, pms_len);
		br_md5_update(&md5_ctx, sh, sizeof sh);
		br_md5_out(&md5_ctx, out + (size_t)round * 16);
	}
}

void
ssl3_key_block(unsigned char *out, size_t len,
	const void *secret, size_t secret_len,
	const unsigned char cli[32], const unsigned char srv[32])
{
	br_md5_context md5_ctx;
	br_sha1_context sha1_ctx;
	unsigned char sh[20];
	unsigned char md[16];
	unsigned char pad[16];
	size_t off = 0;
	int round = 0;

	while (off < len && round < 16) {
		size_t n = len - off;
		if (n > 16)
			n = 16;
		ssl3_round_pad(pad, round);
		br_sha1_init(&sha1_ctx);
		br_sha1_update(&sha1_ctx, pad, (size_t)round + 1);
		br_sha1_update(&sha1_ctx, secret, secret_len);
		br_sha1_update(&sha1_ctx, srv, 32);
		br_sha1_update(&sha1_ctx, cli, 32);
		br_sha1_out(&sha1_ctx, sh);
		br_md5_init(&md5_ctx);
		br_md5_update(&md5_ctx, secret, secret_len);
		br_md5_update(&md5_ctx, sh, sizeof sh);
		br_md5_out(&md5_ctx, md);
		memcpy(out + off, md, n);
		off += n;
		round++;
	}
}
