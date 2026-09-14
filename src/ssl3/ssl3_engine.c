/*
 * ssl3_engine.c — SSL 3.0 record layer and handshake processing.
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

static void
ssl3_compute_record_mac(const br_ssl_engine_context *cc,
	unsigned char *buf, size_t *buf_len,
	int is_outgoing)
{
	unsigned char mac[SSL3_MAC_LEN];
	unsigned char len_buf[2];
	uint64_t seq = 0;
	unsigned char type;
	size_t data_len;
	const unsigned char *data;

	(void)cc;
	(void)is_outgoing;

	type = buf[0];
	len_buf[0] = buf[3];
	len_buf[1] = buf[4];
	data_len = ((size_t)buf[3] << 8) | (size_t)buf[4];
	data = buf + SSL3_HDR_LEN;

	ssl3_mac(NULL, 0, seq, type, len_buf, data, data_len, mac);

	memcpy(buf + SSL3_HDR_LEN + data_len, mac, SSL3_MAC_LEN);
	*buf_len = SSL3_HDR_LEN + data_len + SSL3_MAC_LEN;
}

void
ssl3_server_init(br_ssl_server_context *sc)
{
	br_ssl_engine_context *cc;

	cc = &sc->eng;
	if (cc == NULL)
		return;

	br_ssl_engine_set_versions(cc, SSL3_VERSION, SSL3_VERSION);
	br_ssl_engine_set_prf10(cc, (br_tls_prf_impl)ssl3_prf);
	br_ssl_engine_set_suites(cc, ssl3_suites_with_export,
		sizeof ssl3_suites_with_export / sizeof ssl3_suites_with_export[0]);
}

void
ssl3_register_ciphers(br_ssl_engine_context *cc)
{
	br_ssl_engine_set_prf10(cc, (br_tls_prf_impl)ssl3_prf);
	br_ssl_engine_set_versions(cc, SSL3_VERSION, SSL3_VERSION);
	br_ssl_engine_set_suites(cc, ssl3_suites_with_export,
		sizeof ssl3_suites_with_export / sizeof ssl3_suites_with_export[0]);
}
