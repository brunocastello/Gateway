/*
 * ssl_rec_rc4.c ? RC4 record layer for SSL3 (export 40 and 128).
 * Uses rc4_crypt_state + ssl3 MAC (concat MD5/SHA1) with sequence.
 */

#include "ssl3/ssl3.h"
#include "inner.h"
#include <string.h>
static void
rc4_init_state(unsigned char *S, unsigned *i, unsigned *j,
	const void *key, size_t key_len)
{
	unsigned k, jj = 0;
	unsigned char tmp;
	for (k = 0; k < 256; k++) S[k] = (unsigned char)k;
	for (k = 0; k < 256; k++) {
		jj = (jj + S[k] + ((const unsigned char *)key)[k % key_len]) & 0xFF;
		tmp = S[k]; S[k] = S[jj]; S[jj] = tmp;
	}
	*i = 0; *j = 0;
}

static void
rc4_crypt_state(unsigned char *S, unsigned *i, unsigned *j,
	void *data, size_t len)
{
	unsigned char *p = data;
	unsigned ii = *i, jj = *j;
	unsigned char tmp;
	while (len--) {
		ii = (ii + 1) & 0xFF;
		jj = (jj + S[ii]) & 0xFF;
		tmp = S[ii]; S[ii] = S[jj]; S[jj] = tmp;
		*p++ ^= S[(S[ii] + S[jj]) & 0xFF];
	}
	*i = ii; *j = jj;
}

static void
in_rc4_init(const br_sslrec_in_rc4_class **ctx,
	const void *key, size_t key_len,
	const br_hash_class *hash, const void *mac_key, size_t mac_len)
{
	br_sslrec_in_rc4_context *cc = (br_sslrec_in_rc4_context *)ctx;
	cc->vtable = &br_sslrec_in_rc4_vtable;
	cc->seq = 0;
	rc4_init_state(cc->S, &cc->i, &cc->j, key, key_len);
	cc->hash = hash;
	if (mac_len > sizeof cc->mac_key) mac_len = sizeof cc->mac_key;
	memcpy(cc->mac_key, mac_key, mac_len);
	cc->mac_len = mac_len;
}

static void
out_rc4_init(const br_sslrec_out_rc4_class **ctx,
	const void *key, size_t key_len,
	const br_hash_class *hash, const void *mac_key, size_t mac_len)
{
	br_sslrec_out_rc4_context *cc = (br_sslrec_out_rc4_context *)ctx;
	cc->vtable = &br_sslrec_out_rc4_vtable;
	cc->seq = 0;
	rc4_init_state(cc->S, &cc->i, &cc->j, key, key_len);
	cc->hash = hash;
	if (mac_len > sizeof cc->mac_key) mac_len = sizeof cc->mac_key;
	memcpy(cc->mac_key, mac_key, mac_len);
	cc->mac_len = mac_len;
}

static int
rc4_in_check(const br_sslrec_in_class *const *ctx, size_t len)
{
	(void)ctx;
	return len <= 18432;
}

static unsigned char *
rc4_in_decrypt(const br_sslrec_in_class **ctx,
	int record_type, unsigned version, void *payload, size_t *len)
{
	br_sslrec_in_rc4_context *cc = (br_sslrec_in_rc4_context *)ctx;
	size_t plen = *len;
	unsigned char *data = payload;
	unsigned char mac[48]; /* SSL3 MAC is up to 36 bytes */
	size_t mac_len = cc->mac_len;

	rc4_crypt_state(cc->S, &cc->i, &cc->j, data, plen);
	if (plen < mac_len) return NULL;
	size_t plain_len = plen - mac_len;

	/* Compute SSL3 MAC using ssl3_mac */
	unsigned char len_buf[2];
	len_buf[0] = (plain_len >> 8) & 0xFF;
	len_buf[1] = plain_len & 0xFF;
	ssl3_mac(cc->hash, cc->mac_key, mac_len, cc->seq,
		(unsigned char)record_type, len_buf,
		data, plain_len, mac);

	/* Compare the relevant portion of the MAC */
	unsigned diff = 0;
	const unsigned char *recv_mac = data + plain_len;
	size_t k;
	for (k = 0; k < mac_len; k++) diff |= recv_mac[k] ^ mac[k];
	if (diff) return NULL;
	cc->seq++;
	*len = plain_len;
	return data;
}

static void
rc4_out_max(const br_sslrec_out_class *const *ctx, size_t *start, size_t *end)
{
	const br_sslrec_out_rc4_context *cc = (const br_sslrec_out_rc4_context *)ctx;
	(void)cc;
	*start += 5;
	*end -= 48;
	if (*end < *start) *end = *start;
}

static unsigned char *
rc4_out_encrypt(const br_sslrec_out_class **ctx,
	int record_type, unsigned version, void *data, size_t *len)
{
	br_sslrec_out_rc4_context *cc = (br_sslrec_out_rc4_context *)ctx;
	unsigned char *plain = data;
	size_t plain_len = *len;
	unsigned char mac[48];
	size_t mac_len = cc->mac_len;
	unsigned char len_buf[2];

	len_buf[0] = (plain_len >> 8) & 0xFF;
	len_buf[1] = plain_len & 0xFF;
	ssl3_mac(cc->hash, cc->mac_key, mac_len, cc->seq,
		(unsigned char)record_type, len_buf,
		plain, plain_len, mac);

	/* Append MAC */
	memcpy(plain + plain_len, mac, mac_len);
	size_t out_len = plain_len + mac_len;
	rc4_crypt_state(cc->S, &cc->i, &cc->j, plain, out_len);
	unsigned char *out = plain - 5;
	out[0] = (unsigned char)record_type;
	out[1] = (version >> 8) & 0xFF;
	out[2] = version & 0xFF;
	out[3] = (out_len >> 8) & 0xFF;
	out[4] = out_len & 0xFF;
	cc->seq++;
	*len = out_len + 5;
	return out;
}

const br_sslrec_in_rc4_class br_sslrec_in_rc4_vtable = {
	{
		sizeof(br_sslrec_in_rc4_context),
		(int (*)(const br_sslrec_in_class *const *, size_t)) &rc4_in_check,
		(unsigned char *(*)(const br_sslrec_in_class **,
			int, unsigned, void *, size_t *)) &rc4_in_decrypt
	},
	in_rc4_init
};
const br_sslrec_out_rc4_class br_sslrec_out_rc4_vtable = {
	{
		sizeof(br_sslrec_out_rc4_context),
		(void (*)(const br_sslrec_out_class *const *ctx,
			size_t *start, size_t *end)) &rc4_out_max,
		(unsigned char *(*)(const br_sslrec_out_class **,
			int, unsigned, void *, size_t *)) &rc4_out_encrypt
	},
	out_rc4_init
};