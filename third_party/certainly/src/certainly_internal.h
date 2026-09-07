/*
 * certainly_internal.h — internal struct definitions
 *
 * These are the actual contents of the opaque MacTLS_Context
 * and MacTLS_Config types. Application code never sees this file.
 */

#ifndef CERTAINLY_INTERNAL_H
#define CERTAINLY_INTERNAL_H

#include "certainly.h"
#include "certainly_compat.h"
#include "certainly_transport.h"
#include "entropy.h"
#include "ca_roots.h"
#include "tls13_handshake.h"

#include <bearssl.h>

/*
 * I/O buffer size for BearSSL.
 *
 * BearSSL needs a buffer to hold TLS records during processing.
 * A TLS record can be up to 16KB + overhead. We use a single
 * buffer (half-duplex mode) — BearSSL alternates between reading
 * and writing in this buffer. Full-duplex would need two buffers
 * but uses more RAM.
 *
 * For a REST client doing request-then-response, half-duplex is
 * fine — you're never sending and receiving simultaneously.
 */
#define CERTAINLY_IOBUF_SIZE  (16384 + 325)  /* max TLS record + overhead */

struct MacTLS_Context {
    /* Connection state */
    MacTLS_State    state;
    MacTLS_Error    error;

    /* Open Transport TCP connection */
    CTransport    *transport;

    /* BearSSL client context — contains the TLS state machine */
    br_ssl_client_context   sc;

    /*
     * BearSSL X.509 minimal validator — walks the certificate chain
     * and checks each signature against our trust anchors.
     * "Minimal" means: it validates the chain but doesn't support
     * CRL/OCSP revocation checking. Fine for our use case.
     */
    br_x509_minimal_context xc;

    /* I/O buffer — BearSSL reads/writes TLS records here */
    unsigned char   iobuf[CERTAINLY_IOBUF_SIZE];

    /* Hostname — kept for SNI and cert validation */
    char            host[256];

    /* Tick count when TLS handshake started, for timeout */
    uint32_t        handshake_start_ticks;

    /* Config reference (NULL = use defaults) */
    const MacTLS_Config *config;

    /* TLS 1.3 handshake context */
    tls13_hs_ctx    hs13;

    /*
     * TLS 1.3 application data buffers.
     *
     * During the TLS 1.3 data phase, we bypass BearSSL's engine entirely
     * and handle record encryption/decryption ourselves. These buffers
     * stage incoming encrypted records and decrypted plaintext.
     *
     * recv_buf: holds raw bytes from OT transport, consumed as complete
     *           TLS records are parsed and decrypted.
     * app_buf:  holds decrypted application data ready for MacTLS_Read().
     */
    unsigned char   tls13_recv_buf[5 + TLS13_MAX_CIPHERTEXT]; /* one max record */
    size_t          tls13_recv_len;               /* bytes currently in recv_buf */
    unsigned char   tls13_app_buf[TLS13_MAX_PLAINTEXT]; /* decrypted app data */
    size_t          tls13_app_len;                /* bytes in app_buf */
    size_t          tls13_app_offset;             /* read cursor in app_buf */

    /*
     * Record scratch, Gateway patch (see PATCHES.md).
     *
     * These used to be locals in tls13_recv_records() and MacTLS_Write(),
     * which put 16 KB on the stack inside a call chain that already runs
     * several frames deep from the application's event loop. On Mac OS 9 that
     * is a poor bet, and the decrypt buffer was 256 bytes short of the largest
     * record a peer may legally send. Both now live with the rest of the
     * connection state, on the heap, sized against the RFC 8446 limit.
     */
    unsigned char   tls13_dec_buf[TLS13_MAX_CIPHERTEXT];
    unsigned char   tls13_enc_buf[TLS13_MAX_PLAINTEXT + 1 + TLS13_TAG_SIZE];

    /* True once TLS 1.3 handshake is confirmed (ServerHello chose 1.3) */
    bool            tls13_active;
    /* True once TLS 1.3 handshake has started (ClientHello sent) */
    bool            tls13_started;
};

struct MacTLS_Config {
    /*
     * Custom trust anchors. If non-NULL, these are used INSTEAD of
     * the compiled-in defaults. We store raw DER cert data and
     * decode it when creating a connection.
     */
    const br_x509_trust_anchor *custom_tas;
    size_t                      custom_tas_count;

    /* TODO: add custom CA DER storage when MacTLS_ConfigAddCA is implemented */
};

#endif /* CERTAINLY_INTERNAL_H */
