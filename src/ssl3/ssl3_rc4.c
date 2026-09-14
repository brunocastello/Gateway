/*
 * ssl3_rc4.c — RC4 cipher implementation for SSL 3.0.
 *
 * RC4 is a stream cipher used in SSL 3.0 cipher suites:
 *   TLS_RSA_WITH_RC4_128_MD5  (0x0004)
 *   TLS_RSA_WITH_RC4_128_SHA  (0x0005)
 *
 * This implementation is self-contained with no BearSSL
 * dependency beyond the standard types.
 */

#include "ssl3.h"

#include <string.h>
#include <stdlib.h>

/*
 * RC4 key scheduling algorithm.
 */
static void
rc4_init(unsigned char *S, const unsigned char *key, size_t key_len)
{
	unsigned int i, j;
	unsigned char tmp;

	for (i = 0; i < 256; i++)
		S[i] = (unsigned char)i;

	j = 0;
	for (i = 0; i < 256; i++) {
		j = (j + S[i] + key[i % key_len]) & 0xFF;
		tmp = S[i];
		S[i] = S[j];
		S[j] = tmp;
	}
}

/*
 * RC4 pseudo-random generation algorithm.
 */
static void
rc4_stream(unsigned char *S, unsigned char *data, size_t len)
{
	unsigned int i = 0, j = 0;
	unsigned char tmp;

	while (len--) {
		i = (i + 1) & 0xFF;
		j = (j + S[i]) & 0xFF;
		tmp = S[i];
		S[i] = S[j];
		S[j] = tmp;
		*data++ ^= S[(S[i] + S[j]) & 0xFF];
	}
}

/*
 * RC4 context.
 */
typedef struct {
	unsigned char S[256];
} rc4_ctx;

void *
rc4_setup(const unsigned char *key, size_t key_len)
{
	rc4_ctx *ctx = malloc(sizeof(rc4_ctx));
	if (ctx == NULL)
		return NULL;
	rc4_init(ctx->S, key, key_len);
	return ctx;
}

void
rc4_crypt(void *ctx, const unsigned char *input, unsigned char *output, size_t len)
{
	memcpy(output, input, len);
	rc4_stream(((rc4_ctx *)ctx)->S, output, len);
}

void
rc4_cleanup(void *ctx)
{
	free(ctx);
}
