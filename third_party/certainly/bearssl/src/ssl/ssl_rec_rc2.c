/*
 * ssl_rec_rc2.c — RC2-CBC record layer for SSL 3.0.
 * Block size 8, SSLv3 MAC (MD5) + random padding (last byte = pad len).
 * Mirrors BearSSL's CBC shape but uses the Gateway RC2 primitive and
 * SSLv3's MAC/padding, not TLS's.
 */

#include "ssl3/ssl3.h"
#include "inner.h"
#include <string.h>
#include <stdlib.h>
#ifdef GW_DEBUG_IO
#include <stdio.h>
#endif

static void
in_rc2_init(const br_sslrec_in_rc2_class **ctx,
	const void *key, size_t key_len, unsigned effective_bits,
	const br_hash_class *hash, const void *mac_key, size_t mac_len,
	const void *iv)
{
	br_sslrec_in_rc2_context *cc = (br_sslrec_in_rc2_context *)ctx;
	cc->vtable = &br_sslrec_in_rc2_vtable;
	cc->seq = 0;
	if (cc->rc2) rc2_cleanup(cc->rc2);
	if (effective_bits == 0) effective_bits = (unsigned)(key_len * 8);
	cc->rc2 = rc2_setup_bits(key, key_len, effective_bits);
	cc->hash = hash;
	if (mac_len > sizeof cc->mac_key) mac_len = sizeof cc->mac_key;
	memcpy(cc->mac_key, mac_key, mac_len);
	cc->mac_len = mac_len;
	if (iv) memcpy(cc->iv, iv, 8);
	else memset(cc->iv, 0, 8);
}

static void
out_rc2_init(const br_sslrec_out_rc2_class **ctx,
	const void *key, size_t key_len, unsigned effective_bits,
	const br_hash_class *hash, const void *mac_key, size_t mac_len,
	const void *iv)
{
	br_sslrec_out_rc2_context *cc = (br_sslrec_out_rc2_context *)ctx;
	cc->vtable = &br_sslrec_out_rc2_vtable;
	cc->seq = 0;
	if (cc->rc2) rc2_cleanup(cc->rc2);
	if (effective_bits == 0) effective_bits = (unsigned)(key_len * 8);
	cc->rc2 = rc2_setup_bits(key, key_len, effective_bits);
	cc->hash = hash;
	if (mac_len > sizeof cc->mac_key) mac_len = sizeof cc->mac_key;
	memcpy(cc->mac_key, mac_key, mac_len);
	cc->mac_len = mac_len;
	if (iv) memcpy(cc->iv, iv, 8);
	else memset(cc->iv, 0, 8);
}

static int
rc2_in_check(const br_sslrec_in_class *const *ctx, size_t len)
{
	(void)ctx;
	/* SSL3 ciphertext must be multiple of 8, at least 8 for MAC+pad */
	if (len < 8 || (len & 7) != 0) return 0;
	if (len > 18432) return 0;
	return 1;
}

static unsigned char *
rc2_in_decrypt(const br_sslrec_in_class **ctx,
	int record_type, unsigned version, void *payload, size_t *len)
{
	br_sslrec_in_rc2_context *cc = (br_sslrec_in_rc2_context *)ctx;
	size_t plen = *len;
	unsigned char *data = payload;
	unsigned char mac[32];
	size_t mac_len = cc->mac_len;
	unsigned pad_len;
	size_t plain_len;
	unsigned char len_buf[2];

#ifdef GW_DEBUG_IO
	{
		FILE *kf = fopen("C:\\Gateway\\keylog.txt", "a");
		if (kf != NULL) {
			fprintf(kf, "RC2 DEC enter seq=%llu type=%d plen=%u mac=%u\n",
				(unsigned long long)cc->seq, record_type,
				(unsigned)plen, (unsigned)mac_len);
			fflush(kf); fclose(kf);
		}
	}
#endif
	if (plen < 8 || (plen & 7) != 0) {
#ifdef GW_DEBUG_IO
		FILE *kf = fopen("C:\\Gateway\\keylog.txt", "a");
		if (kf != NULL) { fprintf(kf, "RC2 early1 plen=%u\n", (unsigned)plen); fclose(kf); }
#endif
		return NULL;
	}
	{
		unsigned char ctail[8];
		memcpy(ctail, data + plen - 8, 8);
		rc2_cbc_decrypt(cc->rc2, cc->iv, data, plen);
		memcpy(cc->iv, ctail, 8);
	}

	if (plen < 1) {
#ifdef GW_DEBUG_IO
		FILE *kf = fopen("C:\\Gateway\\keylog.txt", "a");
		if (kf != NULL) { fprintf(kf, "RC2 early2 plen=%u\n", (unsigned)plen); fclose(kf); }
#endif
		return NULL;
	}
	pad_len = data[plen - 1];
	if (pad_len >= 8 || pad_len + 1 + mac_len > plen) {
#ifdef GW_DEBUG_IO
		FILE *kf = fopen("C:\\Gateway\\keylog.txt", "a");
		if (kf != NULL) {
			fprintf(kf, "RC2 early3 pad=%u plain? plen=%u mac=%u\n",
				(unsigned)pad_len, (unsigned)plen, (unsigned)mac_len);
			fprintf(kf, "RC2 tail %02x%02x%02x%02x%02x%02x%02x%02x\n",
				data[plen-8], data[plen-7], data[plen-6], data[plen-5],
				data[plen-4], data[plen-3], data[plen-2], data[plen-1]);
			fclose(kf);
		}
#endif
		return NULL;
	}
	plain_len = plen - mac_len - pad_len - 1;
	len_buf[0] = (plain_len >> 8) & 0xFF;
	len_buf[1] = plain_len & 0xFF;
	ssl3_mac(cc->hash, cc->mac_key, mac_len, cc->seq,
		(unsigned char)record_type, len_buf, data, plain_len, mac);
	{
		unsigned diff = 0;
		const unsigned char *recv_mac = data + plain_len;
		size_t k;
		for (k = 0; k < mac_len; k++) diff |= recv_mac[k] ^ mac[k];
#ifdef GW_DEBUG_IO
		{
			FILE *kf = fopen("C:\\Gateway\\keylog.txt", "a");
			if (kf != NULL) {
				fprintf(kf, "RC2 DEC seq=%llu type=%d plen=%u plain=%u pad=%u %s\n",
					(unsigned long long)cc->seq, record_type,
					(unsigned)plen, (unsigned)plain_len, (unsigned)pad_len,
					diff ? "MISMATCH" : "match");
				fprintf(kf, "RC2 pt %02x%02x%02x%02x calc %02x%02x%02x%02x recv %02x%02x%02x%02x\n",
					data[0], data[1], data[2], data[3],
					mac[0], mac[1], mac[2], mac[3],
					recv_mac[0], recv_mac[1], recv_mac[2], recv_mac[3]);
				fclose(kf);
			}
		}
#endif
		if (diff) return NULL;
	}
	cc->seq++;
	*len = plain_len;
	return data;
}

static void
rc2_out_max(const br_sslrec_out_class *const *ctx, size_t *start, size_t *end)
{
	const br_sslrec_out_rc2_context *cc = (const br_sslrec_out_rc2_context *)ctx;
	(void)cc;
	*start += 5;
	/* Reserve MAC (16) + up to 8 padding bytes; CBC will pad to 8. */
	*end -= 24;
	if (*end < *start) *end = *start;
}

static unsigned char *
rc2_out_encrypt(const br_sslrec_out_class **ctx,
	int record_type, unsigned version, void *data, size_t *len)
{
	br_sslrec_out_rc2_context *cc = (br_sslrec_out_rc2_context *)ctx;
	unsigned char *plain = data;
	size_t plain_len = *len;
	unsigned char mac[32];
	size_t mac_len = cc->mac_len;
	unsigned char len_buf[2];
	size_t pad_len, total, i;
	unsigned char *out;

	len_buf[0] = (plain_len >> 8) & 0xFF;
	len_buf[1] = plain_len & 0xFF;
	ssl3_mac(cc->hash, cc->mac_key, mac_len, cc->seq,
		(unsigned char)record_type, len_buf, plain, plain_len, mac);
	memcpy(plain + plain_len, mac, mac_len);
	total = plain_len + mac_len + 1;
	pad_len = (8 - (total & 7)) & 7;
	/* Fill random padding (except last byte). For determinism use 0. */
	for (i = 0; i < pad_len; i++) plain[plain_len + mac_len + i] = 0;
	plain[plain_len + mac_len + pad_len] = (unsigned char)pad_len;
	total = plain_len + mac_len + pad_len + 1;
	rc2_cbc_encrypt(cc->rc2, cc->iv, plain, total);
	memcpy(cc->iv, plain + total - 8, 8);
	out = plain - 5;
	out[0] = (unsigned char)record_type;
	out[1] = (version >> 8) & 0xFF;
	out[2] = version & 0xFF;
	out[3] = (total >> 8) & 0xFF;
	out[4] = total & 0xFF;
	cc->seq++;
	*len = total + 5;
	return out;
}

const br_sslrec_in_rc2_class br_sslrec_in_rc2_vtable = {
	{
		sizeof(br_sslrec_in_rc2_context),
		(int (*)(const br_sslrec_in_class *const *, size_t)) &rc2_in_check,
		(unsigned char *(*)(const br_sslrec_in_class **, int, unsigned, void *, size_t *)) &rc2_in_decrypt
	},
	in_rc2_init
};
const br_sslrec_out_rc2_class br_sslrec_out_rc2_vtable = {
	{
		sizeof(br_sslrec_out_rc2_context),
		(void (*)(const br_sslrec_out_class *const *ctx, size_t *start, size_t *end)) &rc2_out_max,
		(unsigned char *(*)(const br_sslrec_out_class **, int, unsigned, void *, size_t *)) &rc2_out_encrypt
	},
	out_rc2_init
};
