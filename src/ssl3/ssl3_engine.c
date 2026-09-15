/*
 * ssl3_engine.c ? SSL 3.0 record layer and handshake processing.
 */

#include "ssl3.h"
#include "inner.h"

#include <string.h>

#define SSL3_HDR_LEN    5
#define SSL3_MAC_LEN    36
#define SSL3_VERSION    0x0300

#define SSL3_CK_RSA_NULL_MD5               0x0001
#define SSL3_CK_RSA_EXPORT_RC4_40_MD5      0x0003
#define SSL3_CK_RSA_RC4_128_MD5            0x0004
#define SSL3_CK_RSA_RC4_128_SHA            0x0005
#define SSL3_CK_RSA_EXPORT_RC2_CBC_40_MD5  0x0006
#define SSL3_CK_RSA_EXPORT_DES40_CBC_SHA   0x0008
#define SSL3_CK_RSA_DES_64_CBC_SHA         0x0009
#define SSL3_CK_RSA_3DES_EDE_CBC_SHA       0x000A

static const uint16_t ssl3_suites_with_export[] = {
	SSL3_CK_RSA_EXPORT_RC4_40_MD5,
	SSL3_CK_RSA_EXPORT_RC2_CBC_40_MD5,
	SSL3_CK_RSA_EXPORT_DES40_CBC_SHA,
	SSL3_CK_RSA_RC4_128_MD5,
	SSL3_CK_RSA_RC4_128_SHA,
	SSL3_CK_RSA_DES_64_CBC_SHA,
	SSL3_CK_RSA_3DES_EDE_CBC_SHA
};

static void
ssl3_compute_hash(const br_ssl_engine_context *cc,
	unsigned char *md5_out, unsigned char *sha1_out)
{
	br_md5_context md5_ctx;
	br_sha1_context sha1_ctx;

	br_md5_init(&md5_ctx);
	br_sha1_init(&sha1_ctx);
	br_md5_update(&md5_ctx, cc->hbuf_in, cc->hlen_in);
	br_sha1_update(&sha1_ctx, cc->hbuf_in, cc->hlen_in);
	br_md5_out(&md5_ctx, md5_out);
	br_sha1_out(&sha1_ctx, sha1_out);
}

int
ssl3_finished(const br_ssl_engine_context *cc,
	int is_client, unsigned char *verify_data, size_t *verify_len)
{
	unsigned char md5_hash[16];
	unsigned char sha1_hash[20];
	unsigned char prf_out[36];
	const char *label;
	br_tls_prf_seed_chunk chunks[2];

	ssl3_compute_hash(cc, md5_hash, sha1_hash);

	if (is_client) {
		label = "client finished";
	} else {
		label = "server finished";
	}
	chunks[0].data = md5_hash;
	chunks[0].len = 16;
	chunks[1].data = sha1_hash;
	chunks[1].len = 20;

	ssl3_prf(prf_out, 12, cc->session.master_secret, 48,
		label, 2, chunks);

	memcpy(verify_data, prf_out, 12);
	*verify_len = 12;

	return 0;
}

void
ssl3_server_init(br_ssl_server_context *sc)
{
	br_ssl_engine_context *cc;

	cc = &sc->eng;
	if (cc == NULL)
		return;

	br_ssl_engine_set_versions(cc, SSL3_VERSION, SSL3_VERSION);
	/*
	 * prf10 serves SSL 3.0 only on this engine: versions are pinned
	 * above, so TLS 1.0/1.1 (which need the split-secret P_MD5-XOR-
	 * P_SHA1 PRF, not the SSL 3.0 A/BB/CCC construction) can never
	 * negotiate here. Do not widen the version range without
	 * revisiting ssl3_prf()'s label dispatch.
	 */
	br_ssl_engine_set_prf10(cc, (br_tls_prf_impl)ssl3_prf);
	br_ssl_engine_set_default_des_cbc(cc);
	br_ssl_engine_set_suites(cc, ssl3_suites_with_export,
		sizeof ssl3_suites_with_export / sizeof ssl3_suites_with_export[0]);
}

static void
compute_key_block_ssl3(br_ssl_engine_context *cc,
	unsigned char *kb, size_t kb_len)
{
	br_tls_prf_seed_chunk seed[2] = {
		{ cc->server_random, sizeof cc->server_random },
		{ cc->client_random, sizeof cc->client_random }
	};
	ssl3_prf(kb, kb_len, cc->session.master_secret,
		sizeof cc->session.master_secret, "key expansion", 2, seed);
}

void
br_ssl_engine_switch_rc4_in(br_ssl_engine_context *cc,
	int is_client, int prf_id, int mac_id,
	size_t rc4_key_len, size_t mac_key_len)
{
	unsigned char kb[96];
	size_t kb_len = (mac_key_len + rc4_key_len) * 2;
	if (kb_len > sizeof kb) kb_len = sizeof kb;
	const br_hash_class *hash;
	unsigned char *mac_key;
	unsigned char *rc4_key;
	const br_hash_class *rc4_hash;

	compute_key_block_ssl3(cc, kb, kb_len);

	if (mac_id == br_md5_ID) {
		hash = &br_md5_vtable;
		rc4_hash = &br_md5_vtable;
	} else {
		hash = &br_sha1_vtable;
		rc4_hash = &br_sha1_vtable;
	}

	if (is_client) {
		mac_key = kb + mac_key_len;
		rc4_key = kb + 2 * mac_key_len + rc4_key_len;
	} else {
		mac_key = kb;
		rc4_key = kb + 2 * mac_key_len;
	}

	cc->in.rc4.vtable = &br_sslrec_in_rc4_vtable;
	br_sslrec_in_rc4_vtable.init((const br_sslrec_in_rc4_class **)&cc->in.rc4.vtable,
		rc4_key, rc4_key_len, rc4_hash,
		mac_key, mac_key_len);
	cc->in.rc4.hash = hash;
	cc->in.rc4.mac_len = mac_id == br_md5_ID ? 16 : 20;
	cc->incrypt = 1;
}

void
br_ssl_engine_switch_rc4_out(br_ssl_engine_context *cc,
	int is_client, int prf_id, int mac_id,
	size_t rc4_key_len, size_t mac_key_len)
{
	unsigned char kb[96];
	size_t kb_len = (mac_key_len + rc4_key_len) * 2;
	if (kb_len > sizeof kb) kb_len = sizeof kb;
	const br_hash_class *hash;
	unsigned char *mac_key;
	unsigned char *rc4_key;
	const br_hash_class *rc4_hash;

	compute_key_block_ssl3(cc, kb, kb_len);

	if (mac_id == br_md5_ID) {
		hash = &br_md5_vtable;
		rc4_hash = &br_md5_vtable;
	} else {
		hash = &br_sha1_vtable;
		rc4_hash = &br_sha1_vtable;
	}

	if (is_client) {
		mac_key = kb;
		rc4_key = kb + 2 * mac_key_len;
	} else {
		mac_key = kb + mac_key_len;
		rc4_key = kb + 2 * mac_key_len + rc4_key_len;
	}

	cc->out.rc4.vtable = &br_sslrec_out_rc4_vtable;
	br_sslrec_out_rc4_vtable.init((const br_sslrec_out_rc4_class **)&cc->out.rc4.vtable,
		rc4_key, rc4_key_len, rc4_hash,
		mac_key, mac_key_len);
	cc->out.rc4.hash = hash;
	cc->out.rc4.mac_len = mac_id == br_md5_ID ? 16 : 20;
}

void
ssl3_register_ciphers(br_ssl_engine_context *cc)
{
	(void)cc;
}