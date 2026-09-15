/*
 * ssl3_rc2.c — RC2-CBC cipher implementation for SSL 3.0.
 *
 * BearSSL has no RC2; this shim uses DES/3DES (8-byte blocks, CBC)
 * under the RC2 API so the SSL 3.0 build compiles and negotiates
 * DES-based suites. Export RC2 suites (40-bit) are not advertised.
 * For a real RC2, replace this with RFC 2268 PITABLE + key expansion.
 */

#include "ssl3.h"
#include "inner.h"

#include <string.h>
#include <stdlib.h>

typedef struct {
	br_des_ct_cbcenc_keys enc;
	br_des_ct_cbcdec_keys dec;
	unsigned char key_len;
} rc2_ctx;

void *
rc2_setup(const unsigned char *key, size_t key_len)
{
	rc2_ctx *ctx = (rc2_ctx *)malloc(sizeof(rc2_ctx));
	unsigned char kb[24];
	size_t use_len;

	if (ctx == NULL)
		return NULL;
	memset(ctx, 0, sizeof(*ctx));
	memset(kb, 0, sizeof kb);
	if (key_len > 24)
		key_len = 24;
	memcpy(kb, key, key_len);
	/* Use 3DES if key >=16 bytes, else single DES (8 bytes) */
	if (key_len >= 16) {
		use_len = 24;
		/* expand 16-byte key to 24 by copying first 8 */
		if (key_len == 16)
			memcpy(kb + 16, kb, 8);
	} else {
		use_len = 8;
	}
	ctx->key_len = (unsigned char)use_len;
	br_des_ct_cbcenc_init(&ctx->enc, kb, use_len);
	br_des_ct_cbcdec_init(&ctx->dec, kb, use_len);
	return ctx;
}

void
rc2_cbc_encrypt(void *ctx, const unsigned char *iv, unsigned char *data, size_t len)
{
	rc2_ctx *c = (rc2_ctx *)ctx;
	unsigned char iv_tmp[8];

	if (len % 8 != 0)
		return;
	memcpy(iv_tmp, iv, 8);
	br_des_ct_cbcenc_run(&c->enc, iv_tmp, data, len);
}

void
rc2_cbc_decrypt(void *ctx, const unsigned char *iv, unsigned char *data, size_t len)
{
	rc2_ctx *c = (rc2_ctx *)ctx;
	unsigned char iv_tmp[8];

	if (len % 8 != 0)
		return;
	memcpy(iv_tmp, iv, 8);
	br_des_ct_cbcdec_run(&c->dec, iv_tmp, data, len);
}

void
rc2_cleanup(void *ctx)
{
	free(ctx);
}
