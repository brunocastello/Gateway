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

void
ssl3_server_init(br_ssl_server_context *sc)
{
	/*
	 * SSL 3.0 export suites for vintage clients, appended to the
	 * full-RSA profile's list (which init_full_rsa installed just
	 * before this runs, modern suites first). 0x000A is already
	 * there, so it is not repeated. Suites without a record layer
	 * (RC2, DES40, single DES) translate to nothing and filter out
	 * cleanly until their engines exist.
	 */
	static const uint16_t ssl3_extra_suites[] = {
		SSL3_CK_RSA_EXPORT_RC4_40_MD5,    /* 0x0003 */
		SSL3_CK_RSA_RC4_128_MD5,          /* 0x0004 */
		SSL3_CK_RSA_RC4_128_SHA,          /* 0x0005 */
		SSL3_CK_RSA_EXPORT_RC2_CBC_40_MD5,/* 0x0006 */
		SSL3_CK_RSA_EXPORT_DES40_CBC_SHA, /* 0x0008 */
		SSL3_CK_RSA_DES_64_CBC_SHA,       /* 0x0009 */
	};
	br_ssl_engine_context *cc;
	size_t n, m;

	cc = &sc->eng;
	if (cc == NULL)
		return;

	br_ssl_engine_set_versions(cc, SSL3_VERSION, BR_TLS12);
	n = cc->suites_num;
	m = sizeof ssl3_extra_suites / sizeof ssl3_extra_suites[0];
	if (n + m <= BR_MAX_CIPHER_SUITES) {
		memcpy(cc->suites_buf + n, ssl3_extra_suites,
			m * sizeof ssl3_extra_suites[0]);
		cc->suites_num = n + m;
	}
}

static void
compute_key_block_ssl3(br_ssl_engine_context *cc,
	unsigned char *kb, size_t kb_len)
{
	ssl3_key_block(kb, kb_len, cc->session.master_secret,
		sizeof cc->session.master_secret,
		cc->client_random, cc->server_random);
}

/*
 * Gateway: SSL 3.0 export key expansion (RFC 6101 section 6.2.2),
 * matching OpenSSL 1.0.1e ssl3_change_cipher_state(): the key block
 * carries raw write keys, and the final bulk key is the whole MD5 of
 * raw key + nonces (16 bytes carrying 40 bits of secret for RC4_40).
 * client_key selects whose write key this is (client keys mix
 * ClientHello.random first, server keys ServerHello.random first).
 * Only suite 0x0003 needs this; other suites use block keys directly.
 */
static void
ssl3_export_expand(br_ssl_engine_context *cc,
	const unsigned char *raw, size_t raw_len, int client_key,
	unsigned char expanded[16])
{
	const unsigned char *er1 =
		client_key ? cc->client_random : cc->server_random;
	const unsigned char *er2 =
		client_key ? cc->server_random : cc->client_random;
	br_md5_context md5;

	br_md5_init(&md5);
	br_md5_update(&md5, raw, raw_len);
	br_md5_update(&md5, er1, sizeof cc->client_random);
	br_md5_update(&md5, er2, sizeof cc->server_random);
	br_md5_out(&md5, expanded);
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
	unsigned char expkey[16];

	/* RC4 suites negotiate below TLS 1.0 only (enforced again in the
	 * server policy); fail closed anywhere else. */
	if (cc->session.version != SSL3_VERSION) {
		br_ssl_engine_fail(cc, BR_ERR_BAD_VERSION);
		return;
	}

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

	if (cc->session.cipher_suite == 0x0003) {
		/* Export suite: expand the raw block key (see above).
		 * Reading peer keys here, so client_key = !is_client. */
		ssl3_export_expand(cc, rc4_key, rc4_key_len,
			!is_client, expkey);
		rc4_key = expkey;
		rc4_key_len = 16;
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
	unsigned char expkey[16];

	if (cc->session.version != SSL3_VERSION) {
		br_ssl_engine_fail(cc, BR_ERR_BAD_VERSION);
		return;
	}

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

	if (cc->session.cipher_suite == 0x0003) {
		/* Export suite: expand the raw block key (see above).
		 * Writing own keys here, so client_key = is_client. */
		ssl3_export_expand(cc, rc4_key, rc4_key_len,
			is_client, expkey);
		rc4_key = expkey;
		rc4_key_len = 16;
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