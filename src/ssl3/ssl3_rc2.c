/*
 * ssl3_rc2.c — RC2-CBC for SSL 3.0, RFC 2268.
 *
 * BearSSL has no RC2. This implements the real cipher: PITABLE key
 * expansion, MIX/MASH rounds, 64-bit blocks. Effective key bits are
 * 40 for export suites (0006/0008) and 128 for full strength.
 */

#include "ssl3.h"
#include "inner.h"

#include <string.h>
#include <stdlib.h>

static const unsigned char pitab[256] = {
	0xd9,0x78,0xf9,0xc4,0x19,0xdd,0xb5,0xed,0x28,0xe9,0xfd,0x79,0x4a,0xa0,0xd8,0x9d,
	0xc6,0x7e,0x37,0x83,0x2b,0x76,0x53,0x8e,0x62,0x4c,0x64,0x88,0x44,0x8b,0xfb,0xa2,
	0x17,0x9a,0x59,0xf5,0x87,0xb3,0x4f,0x13,0x61,0x45,0x6d,0x8d,0x09,0x81,0x7d,0x32,
	0xbd,0x8f,0x40,0xeb,0x86,0xb7,0x7b,0x0b,0xf0,0x95,0x21,0x22,0x5c,0x6b,0x4e,0x82,
	0x54,0xd6,0x65,0x93,0xce,0x60,0xb2,0x1c,0x73,0x56,0xc0,0x14,0xa7,0x8c,0xf1,0xdc,
	0x12,0x75,0xca,0x1f,0x3b,0xbe,0xe4,0xd1,0x42,0x3d,0xd4,0x30,0xa3,0x3c,0xb6,0x26,
	0x6f,0xbf,0x0e,0xda,0x46,0x69,0x07,0x57,0x27,0xf2,0x1d,0x9b,0xbc,0x94,0x43,0x03,
	0xf8,0x11,0xc7,0xf6,0x90,0xef,0x3e,0xe7,0x06,0xc3,0xd5,0x2f,0xc8,0x66,0x1e,0xd7,
	0x08,0xe8,0xea,0xde,0x80,0x52,0xee,0xf7,0x84,0xaa,0x72,0xac,0x35,0x4d,0x6a,0x2a,
	0x96,0x1a,0xd2,0x71,0x5a,0x15,0x49,0x74,0x4b,0x9f,0xd0,0x5e,0x04,0x18,0xa4,0xec,
	0xc2,0xe0,0x41,0x6e,0x0f,0x51,0xcb,0xcc,0x24,0x91,0xaf,0x50,0xa1,0xf4,0x70,0x39,
	0x99,0x7c,0x3a,0x85,0x23,0xb8,0xb4,0x7a,0xfc,0x02,0x36,0x5b,0x25,0x55,0x97,0x31,
	0x2d,0x5d,0xfa,0x98,0xe3,0x8a,0x92,0xae,0x05,0xdf,0x29,0x10,0x67,0x6c,0xba,0xc9,
	0xd3,0x00,0xe6,0xcf,0xe1,0x9e,0xa8,0x2c,0x63,0x16,0x01,0x3f,0x58,0xe2,0x89,0xa9,
	0x0d,0x38,0x34,0x1b,0xab,0x33,0xff,0xb0,0xbb,0x48,0x0c,0x5f,0xb9,0xb1,0xcd,0x2e,
	0xc5,0xf3,0xdb,0x47,0xe5,0xa5,0x9c,0x77,0x0a,0xa6,0x20,0x68,0xfe,0x7f,0xc1,0xad
};

typedef struct {
	uint16_t K[64];
} rc2_ctx;

static inline uint16_t rol16(uint16_t x, unsigned k)
{
	return (uint16_t)(((x << k) | (x >> (16 - k))) & 0xFFFF);
}
static inline uint16_t ror16(uint16_t x, unsigned k)
{
	return (uint16_t)(((x >> k) | (x << (16 - k))) & 0xFFFF);
}

static void rc2_expand(const unsigned char *key, size_t t, unsigned t1, uint16_t K[64])
{
	unsigned char L[128];
	unsigned T = (unsigned)t;
	unsigned T8, i;
	unsigned char TM;

	if (T == 0) T = 1;
	if (T > 128) T = 128;
	if (t1 == 0) t1 = T * 8;
	if (t1 > 1024) t1 = 1024;
	T8 = (t1 + 7) / 8;
	TM = (unsigned char)(0xFFu >> (8 * T8 - t1));

	memset(L, 0, 128);
	memcpy(L, key, T);
	for (i = T; i < 128; i++)
		L[i] = pitab[(L[i - 1] + L[i - T]) & 0xFF];
	L[128 - T8] = pitab[L[128 - T8] & TM];
	for (i = 128 - T8; i-- > 0; )
		L[i] = pitab[L[i + 1] ^ L[i + T8]];
	for (i = 0; i < 64; i++)
		K[i] = (uint16_t)L[2 * i] | ((uint16_t)L[2 * i + 1] << 8);
}

static void rc2_encrypt_block(const uint16_t K[64], const unsigned char in[8], unsigned char out[8])
{
	uint16_t R[4];
	int j = 0, round;
	static const unsigned s[4] = {1,2,3,5};
	R[0] = in[0] | ((uint16_t)in[1] << 8);
	R[1] = in[2] | ((uint16_t)in[3] << 8);
	R[2] = in[4] | ((uint16_t)in[5] << 8);
	R[3] = in[6] | ((uint16_t)in[7] << 8);
	for (round = 0; round < 16; round++) {
		int i;
		for (i = 0; i < 4; i++) {
			R[i] = (uint16_t)(R[i] + K[j] + (R[(i+3)&3] & R[(i+2)&3]) + ((~R[(i+3)&3]) & R[(i+1)&3]));
			j++;
			R[i] = rol16(R[i], s[i]);
		}
		if (round == 4 || round == 10) {
			for (i = 0; i < 4; i++)
				R[i] = (uint16_t)(R[i] + K[R[(i+3) & 3] & 63]);
		}
	}
	out[0] = R[0] & 0xFF; out[1] = R[0] >> 8;
	out[2] = R[1] & 0xFF; out[3] = R[1] >> 8;
	out[4] = R[2] & 0xFF; out[5] = R[2] >> 8;
	out[6] = R[3] & 0xFF; out[7] = R[3] >> 8;
}

static void rc2_decrypt_block(const uint16_t K[64], const unsigned char in[8], unsigned char out[8])
{
	uint16_t R[4];
	int j = 63, round;
	static const unsigned s[4] = {1,2,3,5};
	R[0] = in[0] | ((uint16_t)in[1] << 8);
	R[1] = in[2] | ((uint16_t)in[3] << 8);
	R[2] = in[4] | ((uint16_t)in[5] << 8);
	R[3] = in[6] | ((uint16_t)in[7] << 8);
	for (round = 15; round >= 0; round--) {
		int i;
		if (round == 10 || round == 4) {
			for (i = 3; i >= 0; i--)
				R[i] = (uint16_t)(R[i] - K[R[(i+3)&3] & 63]);
		}
		for (i = 3; i >= 0; i--) {
			R[i] = ror16(R[i], s[i]);
			R[i] = (uint16_t)(R[i] - K[j] - (R[(i+3)&3] & R[(i+2)&3]) - ((~R[(i+3)&3]) & R[(i+1)&3]));
			j--;
		}
	}
	out[0] = R[0] & 0xFF; out[1] = R[0] >> 8;
	out[2] = R[1] & 0xFF; out[3] = R[1] >> 8;
	out[4] = R[2] & 0xFF; out[5] = R[2] >> 8;
	out[6] = R[3] & 0xFF; out[7] = R[3] >> 8;
}

void *
rc2_setup(const unsigned char *key, size_t key_len)
{
	return rc2_setup_bits(key, key_len, 0);
}

void *
rc2_setup_bits(const unsigned char *key, size_t key_len, unsigned effective_bits)
{
	rc2_ctx *ctx = (rc2_ctx *)malloc(sizeof(*ctx));
	unsigned t1;
	if (ctx == NULL) return NULL;
	if (effective_bits == 0) {
		if (key_len == 5) t1 = 40;
		else t1 = (unsigned)(key_len * 8);
	} else {
		t1 = effective_bits;
	}
	if (t1 > 1024) t1 = 1024;
	if (t1 == 0) t1 = (unsigned)(key_len * 8);
	rc2_expand(key, key_len, t1, ctx->K);
	return ctx;
}

void
rc2_cbc_encrypt(void *ctx, const unsigned char *iv, unsigned char *data, size_t len)
{
	rc2_ctx *c = (rc2_ctx *)ctx;
	unsigned char chain[8];
	if (len % 8 != 0) return;
	memcpy(chain, iv, 8);
	for (; len >= 8; len -= 8, data += 8) {
		size_t k;
		for (k = 0; k < 8; k++) data[k] ^= chain[k];
		rc2_encrypt_block(c->K, data, data);
		memcpy(chain, data, 8);
	}
}

void
rc2_cbc_decrypt(void *ctx, const unsigned char *iv, unsigned char *data, size_t len)
{
	rc2_ctx *c = (rc2_ctx *)ctx;
	unsigned char chain[8], tmp[8];
	if (len % 8 != 0) return;
	memcpy(chain, iv, 8);
	for (; len >= 8; len -= 8, data += 8) {
		memcpy(tmp, data, 8);
		rc2_decrypt_block(c->K, data, data);
		{
			size_t k;
			for (k = 0; k < 8; k++) data[k] ^= chain[k];
		}
		memcpy(chain, tmp, 8);
	}
}

void
rc2_cleanup(void *ctx)
{
	free(ctx);
}
