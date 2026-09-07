/*
 * tls13_handshake.h — TLS 1.3 handshake state machine types
 */

#ifndef CERTAINLY_TLS13_HANDSHAKE_H
#define CERTAINLY_TLS13_HANDSHAKE_H

#include <bearssl.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "tls13_keysched.h"
#include "tls13_record.h"

/* Handshake message types (RFC 8446 Section 4) */
#define TLS13_HT_CLIENT_HELLO          1
#define TLS13_HT_SERVER_HELLO          2
#define TLS13_HT_NEW_SESSION_TICKET    4
#define TLS13_HT_ENCRYPTED_EXTENSIONS  8
#define TLS13_HT_CERTIFICATE          11
#define TLS13_HT_CERTIFICATE_REQUEST  13
#define TLS13_HT_CERTIFICATE_VERIFY   15
#define TLS13_HT_FINISHED             20
#define TLS13_HT_KEY_UPDATE           24
#define TLS13_HT_MESSAGE_HASH        254

/* TLS versions */
#define TLS13_VERSION  0x0304
#define TLS12_VERSION  0x0303

/* TLS 1.3 cipher suite IDs */
#define TLS13_AES_128_GCM_SHA256       0x1301
#define TLS13_AES_256_GCM_SHA384       0x1302
#define TLS13_CHACHA20_POLY1305_SHA256 0x1303

/* Extension types */
#define TLS13_EXT_SERVER_NAME           0
#define TLS13_EXT_SUPPORTED_GROUPS     10
#define TLS13_EXT_SIGNATURE_ALGORITHMS 13
#define TLS13_EXT_SUPPORTED_VERSIONS   43
#define TLS13_EXT_COOKIE               44
#define TLS13_EXT_KEY_SHARE            51

/* Signature schemes */
#define TLS13_SIG_RSA_PSS_RSAE_SHA256      0x0804
#define TLS13_SIG_RSA_PSS_RSAE_SHA384      0x0805
#define TLS13_SIG_ECDSA_SECP256R1_SHA256   0x0403
#define TLS13_SIG_ECDSA_SECP384R1_SHA384   0x0503
#define TLS13_SIG_RSA_PKCS1_SHA256         0x0401
#define TLS13_SIG_RSA_PKCS1_SHA384         0x0501

/* Named groups */
#define TLS13_GROUP_X25519  0x001D
/*
 * secp256r1 / NIST P-256. Not optional in practice: Microsoft's endpoints
 * (login.microsoftonline.com, outlook.office365.com, smtp-mail.outlook.com)
 * do not support X25519 at all, and reset the connection rather than sending
 * an alert (Gateway patch - see PATCHES.md).
 */
#define TLS13_GROUP_SECP256R1  0x0017

/* Uncompressed P-256 point: 0x04 || X(32) || Y(32) */
#define TLS13_P256_POINT_LEN   65

/* Handshake state machine states */
typedef enum {
    kTLS13_SendClientHello,
    kTLS13_SendCCS,
    kTLS13_RecvServerHello,
    kTLS13_RecvEncryptedExtensions,
    kTLS13_RecvCertRequestOrCert,
    kTLS13_RecvCertificate,
    kTLS13_RecvCertificateVerify,
    kTLS13_RecvFinished,
    kTLS13_SendFinished,
    kTLS13_Complete
} tls13_hs_state;

/* Return values from handshake state handlers */
typedef enum {
    kTLS13_OK,          /* state completed, advance to next */
    kTLS13_WantRead,    /* need more data from network */
    kTLS13_WantWrite,   /* have data to send */
    kTLS13_Fallback12,  /* server chose TLS 1.2, fall back */
    kTLS13_Error        /* handshake failed */
} tls13_hs_result;

/* Transcript hash context */
typedef struct {
    br_sha256_context sha256;
    br_sha384_context sha384;
    const br_hash_class *hash;
    size_t hash_len;
} tls13_transcript;

void tls13_transcript_init(tls13_transcript *t, const br_hash_class *hash);
void tls13_transcript_update(tls13_transcript *t,
                             const void *data, size_t len);
void tls13_transcript_snapshot(const tls13_transcript *t,
                               void *out_hash);
/* Reset for HRR: replace transcript with Hash(message_hash construct) */
void tls13_transcript_reset_for_hrr(tls13_transcript *t);

/* Full TLS 1.3 handshake context */
typedef struct {
    tls13_hs_state      state;
    tls13_keysched      ks;
    tls13_transcript    transcript;
    tls13_record_ctx    read_ctx;    /* decrypt incoming records */
    tls13_record_ctx    write_ctx;   /* encrypt outgoing records */

    /* Ephemeral X25519 key pair */
    unsigned char       ecdhe_secret[32];
    unsigned char       ecdhe_public[32];

    /*
     * A second key share, on P-256. Both are offered in the ClientHello so
     * the server can pick either without costing a HelloRetryRequest round
     * trip; negotiated_group records what it chose.
     */
    unsigned char       ecdhe_p256_priv[32];
    size_t              ecdhe_p256_priv_len;
    unsigned char       ecdhe_p256_pub[TLS13_P256_POINT_LEN];
    uint16_t            negotiated_group;

    /* Traffic secrets (kept for Finished key derivation) */
    /*
     * A fingerprint of the client application key material, taken where it is
     * stashed and again where it is installed.
     *
     * The server's application keys are installed the moment they are derived
     * and demonstrably work; the client's are parked in client_hs_secret and
     * picked up later, and only they produce bad_record_mac. These two numbers
     * decide whether what is picked up is what was put down.
     */
    uint32_t            app_write_fp_stash;
    uint32_t            app_write_fp_install;

    unsigned char       client_hs_secret[64];
    unsigned char       server_hs_secret[64];

    /*
     * Handshake message buffer, used for outgoing messages and for the one
     * incoming message currently being examined.
     *
     * Gateway patch (see PATCHES.md): this was 4096 bytes, while the incoming
     * path copies whole handshake messages into it. A TLS 1.3 Certificate
     * message carrying a leaf plus an intermediate is routinely larger than
     * that, so any site behind a mainstream CDN overran the buffer and
     * corrupted the rest of this struct -- and then the heap. Sized to hold
     * the largest message that can arrive, which is bounded by plain_buf
     * because messages spanning several records are rejected outright.
     */
    unsigned char       msg_buf[TLS13_MAX_PLAINTEXT];
    size_t              msg_len;
    size_t              msg_offset;

    /*
     * Length of a pre-read handshake message sitting in msg_buf
     * (for the RecvCertRequestOrCert handler to hand off to
     * RecvCertificate). SEPARATE from msg_len to avoid triggering
     * the outgoing-send pump path.
     */
    size_t              pending_recv_msg_len;

    /*
     * Plaintext buffer for decrypted incoming handshake records.
     * TLS 1.3 servers commonly pack multiple handshake messages into
     * a single encrypted record (EncryptedExtensions + Certificate +
     * CertificateVerify + Finished). We decrypt the full record into
     * this buffer, then consume handshake messages one at a time from
     * here, advancing plain_offset. When plain_offset == plain_len,
     * we decrypt the next record.
     */
    /*
     * Sized against the ciphertext limit, not the plaintext one, because
     * records are decrypted in place and the tag and content-type byte land
     * here too -- and then again for a full message, because a handshake
     * message split across records is reassembled in this buffer: it has to
     * hold the partial message plus the whole of the record that completes it.
     */
    unsigned char       plain_buf[TLS13_MAX_PLAINTEXT + TLS13_MAX_CIPHERTEXT];
    size_t              plain_len;
    size_t              plain_offset;

    /* HRR cookie */
    unsigned char       cookie[256];
    size_t              cookie_len;

    /* Negotiated cipher suite */
    uint16_t            cipher_suite;
    bool                is_tls13;
    bool                hrr_received;

    bool                cert_request_received;

    /* X.509 validation context (pointer to MacTLS_Context's xc) */
    const br_x509_class **x509_ctx;

    /*
     * BearSSL engine pointer — used only for PRNG seeding during
     * ClientHello construction. The handshake does NOT drive record
     * I/O through this engine; that is done directly against a
     * caller-supplied recv buffer. This field is allowed to be NULL
     * if the caller arranges RNG seeding some other way.
     */
    br_ssl_engine_context *eng;

    /* Server's public key (from certificate, for CertificateVerify) */
    br_x509_pkey        server_pkey;
    /* Backing storage for server_pkey's pointer fields */
    unsigned char       server_pkey_data[520]; /* BR_X509_BUFSIZE_KEY */

    /* Error code from BearSSL (if handshake fails) */
    int                 error;
} tls13_hs_ctx;

/*
 * Main handshake driver — call repeatedly from the pump loop.
 *
 * recv_buf / recv_len is a caller-owned buffer holding raw bytes received
 * from the network. The handshake consumes complete TLS records from the
 * front of the buffer and slides the remaining bytes down, updating
 * *recv_len to reflect what's still unread. Callers should append newly
 * received bytes to recv_buf at offset *recv_len before calling this.
 */
tls13_hs_result tls13_handshake_step(tls13_hs_ctx *hs,
                                     unsigned char *recv_buf,
                                     size_t *recv_len,
                                     const char *hostname);

/* Initialize handshake context */
void tls13_handshake_init(tls13_hs_ctx *hs);

/* Handle post-handshake messages (NewSessionTicket, KeyUpdate).
 * Called when an encrypted record with inner content type handshake (22)
 * is received during the application data phase. */
tls13_hs_result tls13_handle_post_handshake(tls13_hs_ctx *hs,
                                            const unsigned char *data,
                                            size_t data_len);

#endif /* CERTAINLY_TLS13_HANDSHAKE_H */
