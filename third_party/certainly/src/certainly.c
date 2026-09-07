/*
 * certainly.c — Core implementation
 *
 * This file is the glue between the public API, BearSSL, and Open Transport.
 * Its main job is the pump loop — shuttling bytes between OT and BearSSL's
 * state machine buffers.
 *
 * TLS version negotiation:
 *   The library always starts by sending a TLS 1.3 ClientHello (which also
 *   offers TLS 1.2 cipher suites for fallback). If the server responds with
 *   TLS 1.3, the handshake is driven entirely by our tls13_handshake code.
 *   If the server responds with TLS 1.2, we reconnect and let BearSSL's
 *   T0 engine handle the entire handshake.
 */

#include "certainly_internal.h"

#include <string.h>

/* ── Library lifecycle ── */

MacTLS_Error MacTLS_Init(void)
{
    entropy_init();
    return kMacTLS_OK;
}

void MacTLS_Shutdown(void)
{
    /* Currently nothing to clean up at library level */
}

/* ── BearSSL setup helpers ── */

/*
 * Initialize BearSSL's client context for a TLS 1.2 connection.
 *
 * This configures:
 *   1. Which cipher suites to offer (we pick a practical subset)
 *   2. The X.509 certificate validator with our trust anchors
 *   3. The I/O buffer
 *   4. The server name for SNI
 *
 * CIPHER SUITES — what the client and server negotiate:
 *   A cipher suite specifies four algorithms:
 *     - Key exchange: how client+server agree on a shared secret
 *       (ECDHE = Elliptic Curve Diffie-Hellman Ephemeral — forward secrecy)
 *     - Authentication: how the server proves identity (RSA or ECDSA)
 *     - Encryption: how data is encrypted (AES-128-GCM, ChaCha20-Poly1305)
 *     - MAC: message integrity (included in GCM/Poly1305 — AEAD ciphers)
 *
 *   We offer modern AEAD suites only. The server picks the best one it supports.
 */
static void setup_bearssl(MacTLS_Context *ctx)
{
    /*
     * Cipher suites, in preference order.
     *
     * ChaCha20-Poly1305 is listed first because PPC G3/G4 lacks AES hardware
     * acceleration, making ChaCha20-Poly1305 significantly faster than AES-GCM
     * on this platform. ECDSA is interleaved before RSA because most modern
     * servers use ECDSA certificates.
     */
    static const uint16_t suites[] = {
        BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    };

    const br_x509_trust_anchor *tas;
    size_t tas_count;

    /* Use custom trust anchors if configured, otherwise defaults */
    if (ctx->config && ctx->config->custom_tas) {
        tas       = ctx->config->custom_tas;
        tas_count = ctx->config->custom_tas_count;
    } else {
        tas       = TAs;
        tas_count = TAs_NUM;
    }

    /*
     * br_ssl_client_init_full sets up the client context with
     * sensible defaults: all hash functions, all standard cipher
     * suites, the minimal X.509 validator. We then override the
     * cipher suite list to only offer AEAD suites.
     */
    br_ssl_client_init_full(&ctx->sc, &ctx->xc, tas, tas_count);

    /* Override with our preferred cipher suites */
    br_ssl_engine_set_suites(&ctx->sc.eng, suites,
                             sizeof(suites) / sizeof(suites[0]));

    /* Pin to TLS 1.2 only — prevent downgrade to TLS 1.0/1.1 */
    br_ssl_engine_set_versions(&ctx->sc.eng, BR_TLS12, BR_TLS12);

    /*
     * Set the I/O buffer. BearSSL uses this to assemble and
     * disassemble TLS records. In mono (half-duplex) mode, a single
     * buffer handles both directions — BearSSL alternates between
     * reading into it and writing from it.
     */
    br_ssl_engine_set_buffer(&ctx->sc.eng, ctx->iobuf,
                             sizeof(ctx->iobuf), 0);

    /* Seed the PRNG with our entropy pool */
    entropy_seed_engine(&ctx->sc.eng);

    /*
     * Initialize the TLS 1.3 handshake context.
     *
     * This prepares the parallel TLS 1.3 state machine. The TLS 1.3
     * handshake will be attempted first; if the server doesn't support
     * 1.3, we fall back to BearSSL's T0 engine for TLS 1.2.
     *
     * The X.509 validator is shared between both paths — BearSSL's
     * br_ssl_client_init_full already set up ctx->xc, and we point
     * the TLS 1.3 context at it.
     */
    tls13_handshake_init(&ctx->hs13);
    ctx->hs13.x509_ctx = (const br_x509_class **)&ctx->xc.vtable;
    /* The TLS 1.3 handshake borrows BearSSL's engine only for its PRNG. */
    ctx->hs13.eng = &ctx->sc.eng;

    /*
     * NOTE: We deliberately do NOT call br_ssl_client_reset() here.
     * The TLS 1.3 handshake is driven by our own state machine, which
     * reads records directly from ctx->tls13_recv_buf rather than from
     * BearSSL's engine. Starting BearSSL's T0 handshake at this point
     * would put the engine in a state where it competes with us for
     * record I/O buffers. br_ssl_client_reset() is only called on the
     * TLS 1.2 fallback path (see tls13_pump_handshake / kTLS13_Fallback12).
     */
}

/* ── Connection lifecycle ── */

MacTLS_Context *MacTLS_Create(const char *host, uint16_t port)
{
    return MacTLS_CreateWithConfig(host, port, NULL);
}

MacTLS_Context *MacTLS_CreateWithConfig(const char *host, uint16_t port,
                                        MacTLS_Config *cfg)
{
    MacTLS_Context *ctx;

    ctx = (MacTLS_Context *)NewPtrClear(sizeof(MacTLS_Context));
    if (ctx == NULL) return NULL;

    ctx->state  = kMacTLS_Connecting;
    ctx->config = cfg;

    /*
     * Reject invalid hostnames. DNS names are limited to 253 chars (RFC 1035).
     * Also reject anything that would overflow our 256-byte buffer.
     */
    if (strlen(host) > 253 || strlen(host) >= sizeof(ctx->host)) {
        ctx->state = kMacTLS_Error;
        ctx->error = kMacTLS_ErrDNS;  /* closest error code */
        return ctx;
    }

    /* Save hostname for SNI */
    strncpy(ctx->host, host, sizeof(ctx->host) - 1);
    ctx->host[sizeof(ctx->host) - 1] = '\0';

    /* Start TCP connection */
    ctx->transport = ct_transport_create(host, port);
    if (ctx->transport == NULL) {
        ctx->state = kMacTLS_Error;
        ctx->error = kMacTLS_ErrMemory;
        return ctx;
    }

    /* Set up BearSSL (doesn't do any I/O yet — just configures) */
    setup_bearssl(ctx);

    return ctx;
}

MacTLS_Context *MacTLS_CreateOnEndpoint(const char *host, CTSocket sock)
{
    MacTLS_Context *ctx;

    if (sock == CT_SOCKET_NONE) return NULL;

    ctx = (MacTLS_Context *)NewPtrClear(sizeof(MacTLS_Context));
    if (ctx == NULL) {
        /* Ownership transferred unconditionally, so it is ours to close. */
        ct_socket_close(sock);
        return NULL;
    }

    ctx->state  = kMacTLS_Connecting;
    ctx->config = NULL;

    if (strlen(host) > 253 || strlen(host) >= sizeof(ctx->host)) {
        ctx->state = kMacTLS_Error;
        ctx->error = kMacTLS_ErrDNS;
        ct_socket_close(sock);
        return ctx;
    }

    strncpy(ctx->host, host, sizeof(ctx->host) - 1);
    ctx->host[sizeof(ctx->host) - 1] = '\0';

    ctx->transport = ct_transport_adopt(sock);
    if (ctx->transport == NULL) {
        ctx->state = kMacTLS_Error;
        ctx->error = kMacTLS_ErrMemory;
        ct_socket_close(sock);
        return ctx;
    }

    setup_bearssl(ctx);

    return ctx;
}

/* ── TLS 1.3 Pump Helpers ── */

/*
 * Drive the TLS 1.3 handshake forward.
 *
 * This function handles the TLS 1.3 handshake by:
 *   1. Sending outgoing data from hs13.msg_buf via OT transport
 *   2. Receiving incoming data into ctx->tls13_recv_buf (the same buffer
 *      used for application-data records post-handshake)
 *   3. Calling tls13_handshake_step() to advance the state machine
 *
 * Returns the resulting MacTLS_State.
 *
 * The TLS 1.3 handshake code never touches BearSSL's engine for record
 * I/O — it reads directly from the recv_buf we pass in and consumes
 * complete records by sliding them off the front of the buffer.
 */
static MacTLS_State tls13_pump_handshake(MacTLS_Context *ctx)
{
    tls13_hs_result r;
    int n;

    /*
     * Step 1: If there's outgoing data in msg_buf, send it.
     * The handshake code fills msg_buf and returns WantWrite.
     * CRITICAL: we must NOT advance the handshake state until msg_buf
     * is fully flushed, otherwise the next handshake step would
     * overwrite msg_buf with new data before the previous message
     * is fully sent.
     */
    if (ctx->hs13.msg_offset < ctx->hs13.msg_len) {
        size_t remaining = ctx->hs13.msg_len - ctx->hs13.msg_offset;
        n = ct_transport_send(ctx->transport,
                              ctx->hs13.msg_buf + ctx->hs13.msg_offset,
                              remaining);
        if (n > 0) {
            ctx->hs13.msg_offset += n;
        } else if (n < 0) {
            ctx->state = kMacTLS_Error;
            ctx->error = kMacTLS_ErrWrite;
            return ctx->state;
        }
        /*
         * Always return after a send attempt — do NOT fall through to
         * call handshake_step, which could overwrite msg_buf. We'll
         * come back next pump to either continue sending or advance
         * the state machine.
         */
        ctx->state = kMacTLS_Handshaking;
        return ctx->state;
    }

    /*
     * Step 2: Read incoming data into our dedicated tls13_recv_buf.
     * The TLS 1.3 handshake code reads records from here directly.
     */
    if (ctx->tls13_recv_len < sizeof(ctx->tls13_recv_buf)) {
        size_t space = sizeof(ctx->tls13_recv_buf) - ctx->tls13_recv_len;
        n = ct_transport_recv(ctx->transport,
                              ctx->tls13_recv_buf + ctx->tls13_recv_len,
                              space);
        if (n > 0) {
            ctx->tls13_recv_len += (size_t)n;
        } else if (n < 0) {
            /*
             * OT reports no more data. If the peer has closed,
             * we may still have pending data in tls13_recv_buf
             * that needs to be processed. Don't return Closed
             * immediately — fall through to handshake_step to
             * drain the buffer. If there's no pending data AND
             * no handshake work to do, the handshake code will
             * report WantRead and we'll come back here next time
             * with still no data — at which point we mark Closed.
             */
            if (ct_transport_peer_closed(ctx->transport) &&
                ctx->tls13_recv_len == 0 &&
                ctx->hs13.plain_offset >= ctx->hs13.plain_len) {
                ctx->state = kMacTLS_Closed;
                return ctx->state;
            }
            if (!ct_transport_peer_closed(ctx->transport)) {
                ctx->state = kMacTLS_Error;
                ctx->error = kMacTLS_ErrRead;
                return ctx->state;
            }
            /* Connection closing but we have data — fall through */
        }
    }

    /*
     * Step 3: Advance the TLS 1.3 handshake state machine.
     * The handshake consumes complete records from the front of
     * tls13_recv_buf and updates tls13_recv_len accordingly.
     */
    r = tls13_handshake_step(&ctx->hs13,
                             ctx->tls13_recv_buf,
                             &ctx->tls13_recv_len,
                             ctx->host);

    switch (r) {
    case kTLS13_OK:
        /* State advanced — pump again on next call */
        ctx->state = kMacTLS_Handshaking;
        break;

    case kTLS13_WantRead:
        /* Need more data from network */
        ctx->state = kMacTLS_Handshaking;
        break;

    case kTLS13_WantWrite:
        /* Have data to send — will be flushed on next pump */
        ctx->state = kMacTLS_Handshaking;
        break;

    case kTLS13_Fallback12:
        /*
         * Server chose TLS 1.2 — fall back.
         *
         * The simplest fallback approach: close the current TCP connection
         * and reconnect, letting BearSSL's T0 engine handle the entire
         * TLS 1.2 handshake from scratch.
         *
         * This is simpler than trying to replay the ServerHello through
         * BearSSL's engine (which would require feeding raw bytes into
         * a very specific T0 state). The cost of a second TCP handshake
         * is negligible compared to the TLS handshake itself.
         *
         * Flow:
         * 1. Close current OT transport
         * 2. Create a new OT transport to the same host:port
         * 3. Re-init BearSSL (it will do its own ClientHello)
         * 4. Set tls13_active = false so the pump loop uses BearSSL
         */
        {
            uint16_t port = ct_transport_port(ctx->transport);

            /* Close and destroy the current transport */
            ct_transport_close(ctx->transport);
            ct_transport_destroy(ctx->transport);

            /* Create a new transport */
            ctx->transport = ct_transport_create(ctx->host, port);
            if (ctx->transport == NULL) {
                ctx->state = kMacTLS_Error;
                ctx->error = kMacTLS_ErrMemory;
                return ctx->state;
            }

            /* Re-init BearSSL for TLS 1.2 (the T0 engine will drive) */
            br_ssl_client_reset(&ctx->sc, ctx->host, 0);

            /* Mark as NOT TLS 1.3 — future pumps use BearSSL path */
            ctx->tls13_active = false;
            ctx->tls13_started = false;
            ctx->hs13.is_tls13 = false;
            /* Clear any diagnostic alert from the failed 1.3 attempt so
             * a successful 1.2 retry doesn't surface stale error state. */
            ctx->hs13.error = 0;
            ctx->state = kMacTLS_Connecting;
        }
        break;

    case kTLS13_Error:
        ctx->state = kMacTLS_Error;
        ctx->error = kMacTLS_ErrHandshake;
        break;

    default:
        break;
    }

    /*
     * Check if the handshake just completed.
     */
    if (ctx->hs13.state == kTLS13_Complete &&
        ctx->hs13.msg_offset >= ctx->hs13.msg_len) {
        /*
         * Handshake complete and all outgoing data flushed.
         * Switch to connected state. From here, MacTLS_Read/Write
         * will use the TLS 1.3 record layer directly.
         */
        ctx->state = kMacTLS_Connected;
        ctx->tls13_active = true;
    }

    return ctx->state;
}

/*
 * Process incoming TLS 1.3 records during the application data phase.
 *
 * Reads raw bytes from OT transport into tls13_recv_buf, then parses
 * and decrypts complete TLS records. Application data is placed into
 * tls13_app_buf for MacTLS_Read() to consume. Post-handshake messages
 * (NewSessionTicket) are handled transparently.
 *
 * This function is non-blocking: it processes whatever data is available
 * and returns.
 */
static int tls13_flush_out(MacTLS_Context *ctx);

static void tls13_recv_records(MacTLS_Context *ctx)
{
    int n;

    /*
     * Step 1: Read raw bytes from OT transport into recv_buf.
     * If the peer has closed but we have buffered data, we still
     * need to process it below — don't return early.
     */
    if (ctx->tls13_recv_len < sizeof(ctx->tls13_recv_buf)) {
        size_t space = sizeof(ctx->tls13_recv_buf) - ctx->tls13_recv_len;
        n = ct_transport_recv(ctx->transport,
                              ctx->tls13_recv_buf + ctx->tls13_recv_len,
                              space);
        if (n > 0) {
            ctx->tls13_recv_len += n;
        } else if (n < 0) {
            /*
             * Peer closed or error. Mark for closure AFTER processing
             * any pending records below. Real errors (not peer close)
             * still bail immediately.
             */
            if (!ct_transport_peer_closed(ctx->transport)) {
                ctx->state = kMacTLS_Error;
                ctx->error = kMacTLS_ErrRead;
                return;
            }
            /* Fall through to process any buffered records */
        }
    }

    /*
     * Step 2: Parse complete TLS records from recv_buf.
     * Keep processing as long as we have complete records.
     */
    while (ctx->tls13_recv_len >= 5) {
        uint16_t record_len;
        uint8_t record_type;
        size_t total_record;
        unsigned char *decrypted = ctx->tls13_dec_buf;
        size_t dec_len;
        uint8_t inner_ct;
        int ret;

        record_type = ctx->tls13_recv_buf[0];
        record_len = (uint16_t)((ctx->tls13_recv_buf[3] << 8) |
                                 ctx->tls13_recv_buf[4]);

        total_record = 5 + record_len;

        /*
         * Gateway patch (see PATCHES.md): recv_buf holds exactly one maximum
         * record, so a longer one can never arrive in full. Without this the
         * loop would break every pass waiting for bytes that cannot fit, and
         * the connection would hang instead of failing.
         */
        if (record_len > TLS13_MAX_CIPHERTEXT) {
            ctx->state = kMacTLS_Error;
            ctx->error = kMacTLS_ErrRead;
            return;
        }

        if (ctx->tls13_recv_len < total_record) {
            /* Incomplete record — wait for more data */
            break;
        }

        /*
         * Skip CCS records (type 20) — middlebox compatibility artifacts
         * that can arrive at any time.
         */
        if (record_type == TLS13_CT_CHANGE_CIPHER_SPEC) {
            /* Consume the record and continue */
            memmove(ctx->tls13_recv_buf,
                    ctx->tls13_recv_buf + total_record,
                    ctx->tls13_recv_len - total_record);
            ctx->tls13_recv_len -= total_record;
            continue;
        }

        /*
         * Alert records (type 21) — server is signaling something.
         * For simplicity, treat any alert as connection close.
         */
        if (record_type == TLS13_CT_ALERT) {
            ctx->state = kMacTLS_Closed;
            return;
        }

        /*
         * In TLS 1.3, all encrypted records have outer type 0x17
         * (application_data). The real content type is inside.
         */
        if (record_type != TLS13_CT_APPLICATION_DATA) {
            ctx->state = kMacTLS_Error;
            ctx->error = kMacTLS_ErrRead;
            return;
        }

        /* Decrypt the record */
        ret = tls13_record_decrypt(&ctx->hs13.read_ctx,
                                   ctx->tls13_recv_buf + 5, record_len,
                                   decrypted, sizeof(ctx->tls13_dec_buf),
                                   &dec_len, &inner_ct);
        if (ret != 0) {
            ctx->state = kMacTLS_Error;
            ctx->error = kMacTLS_ErrRead;
            return;
        }

        /* Consume the record from recv_buf */
        memmove(ctx->tls13_recv_buf,
                ctx->tls13_recv_buf + total_record,
                ctx->tls13_recv_len - total_record);
        ctx->tls13_recv_len -= total_record;

        /*
         * Dispatch based on the inner (real) content type.
         */
        if (inner_ct == TLS13_CT_APPLICATION_DATA) {
            /*
             * Application data — append to app_buf for MacTLS_Read().
             *
             * If app_buf still has unconsumed data, compact it first
             * (move remaining data to the front).
             */
            size_t app_remaining = ctx->tls13_app_len - ctx->tls13_app_offset;
            if (ctx->tls13_app_offset > 0 && app_remaining > 0) {
                memmove(ctx->tls13_app_buf,
                        ctx->tls13_app_buf + ctx->tls13_app_offset,
                        app_remaining);
                ctx->tls13_app_len = app_remaining;
                ctx->tls13_app_offset = 0;
            } else if (app_remaining == 0) {
                ctx->tls13_app_len = 0;
                ctx->tls13_app_offset = 0;
            }

            /* Append decrypted data (truncate if buffer full) */
            {
                size_t space = sizeof(ctx->tls13_app_buf) - ctx->tls13_app_len;
                size_t copy = (dec_len < space) ? dec_len : space;
                memcpy(ctx->tls13_app_buf + ctx->tls13_app_len,
                       decrypted, copy);
                ctx->tls13_app_len += copy;
            }

        } else if (inner_ct == TLS13_CT_HANDSHAKE) {
            /*
             * Post-handshake message (NewSessionTicket, KeyUpdate, etc.)
             * Dispatch to the post-handshake handler.
             */
            tls13_hs_result phr = tls13_handle_post_handshake(
                &ctx->hs13, decrypted, dec_len);
            if (phr == kTLS13_Error) {
                ctx->state = kMacTLS_Error;
                ctx->error = kMacTLS_ErrHandshake;
                return;
            }
            /* kTLS13_OK means the message was handled (e.g., discarded) */

        } else if (inner_ct == TLS13_CT_ALERT) {
            /*
             * Decrypted alert. Keep the two bytes before treating it as a
             * close: level and description are the peer's entire account of
             * what it objected to, and discarding them turned every rejected
             * record into an unexplained disconnection.
             */
            if (dec_len >= 2)
                ctx->tls13_alert = ((unsigned int)decrypted[0] << 8) |
                                   decrypted[1];
            ctx->state = kMacTLS_Closed;
            return;

        } else {
            /* Unexpected inner content type */
            ctx->state = kMacTLS_Error;
            ctx->error = kMacTLS_ErrRead;
            return;
        }
    }

    /*
     * After processing all buffered records: if the peer closed and
     * there's no more data to read, mark as Closed. The app can still
     * drain tls13_app_buf via MacTLS_Read in the Closed state.
     */
    if (ct_transport_peer_closed(ctx->transport) &&
        ctx->tls13_recv_len == 0) {
        ctx->state = kMacTLS_Closed;
    }
}

/*
 * The pump loop — the heart of Certainly.
 *
 * This function moves bytes between three layers:
 *   App <-> BearSSL/TLS1.3 <-> OT
 *
 * Each call does a bounded amount of work and returns. It NEVER
 * blocks. Call it from your event loop alongside WaitNextEvent.
 *
 * Version negotiation flow:
 *   1. First pump after TCP connect: send TLS 1.3 ClientHello + CCS
 *   2. Receive ServerHello:
 *      a. TLS 1.3: continue with our handshake code
 *      b. TLS 1.2: reconnect and use BearSSL's T0 engine
 *   3. After handshake completes: use TLS 1.3 or 1.2 data path
 */
MacTLS_State MacTLS_Pump(MacTLS_Context *ctx)
{
    unsigned int    st;
    unsigned char  *buf;
    size_t          len;
    int             n;

    if (ctx->state == kMacTLS_Error ||
        ctx->state == kMacTLS_Closed) {
        return ctx->state;
    }

    /* Step 1: Pump the OT transport layer */
    {
        CTransportState tstate = ct_transport_pump(ctx->transport);

        if (tstate == kCTransport_Error) {
            /*
             * Transport has entered error state (e.g. server disconnected).
             * If we already completed the TLS handshake and have received
             * data in the TLS 1.3 buffers, treat it as a graceful close so
             * the app can still read the response data.
             */
            if (ctx->tls13_active && ctx->state == kMacTLS_Connected) {
                /* Let tls13_recv_records process any remaining data. */
                tls13_recv_records(ctx);
                /* If there's still undrained app data, stay Connected. */
                if (ctx->tls13_app_offset < ctx->tls13_app_len) {
                    return ctx->state;
                }
                ctx->state = kMacTLS_Closed;
                return ctx->state;
            }
            ctx->state = kMacTLS_Error;
            ctx->error = kMacTLS_ErrConnect;
            return ctx->state;
        }

        /* Still waiting for TCP? Nothing for BearSSL to do yet. */
        if (tstate == kCTransport_ResolvingDNS ||
            tstate == kCTransport_Connecting) {
            ctx->state = kMacTLS_Connecting;
            return ctx->state;
        }

        /*
         * TCP just connected — start the handshake timer.
         * Without this timeout, a malicious server could complete the TCP
         * handshake but never send a TLS ServerHello, stalling us forever
         * in kMacTLS_Handshaking. An attacker in a MITM position can do
         * this trivially: accept the SYN, complete the three-way handshake,
         * then go silent. Our app would spin in the pump loop indefinitely.
         */
        if (ctx->state == kMacTLS_Connecting && tstate == kCTransport_Connected) {
            ctx->handshake_start_ticks = (uint32_t)TickCount();
        }
    }

    /*
     * Handshake timeout check — 30 seconds.
     * This protects against a MITM that completes TCP but stalls TLS.
     */
    if (ctx->state == kMacTLS_Handshaking) {
        if ((uint32_t)TickCount() - ctx->handshake_start_ticks > 30 * 60) {
            ctx->state = kMacTLS_Error;
            ctx->error = kMacTLS_ErrHandshake;
            return ctx->state;
        }
    }

    /*
     * TLS 1.3 handshake path.
     *
     * If we haven't fallen back to TLS 1.2, drive the TLS 1.3
     * handshake state machine. This bypasses BearSSL's T0 engine
     * entirely during the handshake phase.
     */
    if (!ctx->tls13_started && ctx->hs13.is_tls13 == false &&
        ctx->hs13.state == kTLS13_SendClientHello) {
        /*
         * First time through after TCP connect — start the TLS 1.3
         * handshake. The tls13_started flag prevents re-entry after
         * a TLS 1.2 fallback (where we've cleared the flag and want
         * BearSSL to drive).
         */
        ctx->tls13_started = true;
        ctx->state = kMacTLS_Handshaking;
        return tls13_pump_handshake(ctx);
    }

    if (ctx->tls13_started && ctx->state != kMacTLS_Connected) {
        /*
         * TLS 1.3 handshake in progress.
         * Keep pumping the TLS 1.3 state machine until it completes,
         * errors, or falls back to TLS 1.2.
         */
        return tls13_pump_handshake(ctx);
    }

    /*
     * TLS 1.3 connected state — finish sending, then process incoming records.
     */
    if (ctx->tls13_active && ctx->state == kMacTLS_Connected) {
        /*
         * A record MacTLS_Write staged but could not finish must keep going
         * out from here. The usual shape of an exchange is a caller that
         * writes a whole request and then does nothing but read, so without
         * this the tail of the last record would sit in the buffer forever
         * and the peer would wait for a message it had only half received.
         */
        if (ctx->tls13_out_sent < ctx->tls13_out_len &&
            tls13_flush_out(ctx) < 0) {
            ctx->state = kMacTLS_Error;
            ctx->error = kMacTLS_ErrWrite;
            return ctx->state;
        }

        tls13_recv_records(ctx);
        return ctx->state;
    }

    /*
     * TLS 1.2 path (BearSSL's T0 engine).
     *
     * If we fell back to TLS 1.2 (tls13_started == false), or if we're
     * in the closing state, use BearSSL's engine to drive everything.
     */

    st = br_ssl_engine_current_state(&ctx->sc.eng);

    if (st == BR_SSL_CLOSED) {
        int err = br_ssl_engine_last_error(&ctx->sc.eng);
        if (err == BR_ERR_OK) {
            ctx->state = kMacTLS_Closed;
        } else if (err == BR_ERR_X509_NOT_TRUSTED ||
                   err == BR_ERR_X509_BAD_SERVER_NAME) {
            ctx->state = kMacTLS_Error;
            ctx->error = kMacTLS_ErrCertificate;
        } else {
            ctx->state = kMacTLS_Error;
            ctx->error = kMacTLS_ErrHandshake;
        }
        return ctx->state;
    }

    /* Send: BearSSL -> OT */
    if (st & BR_SSL_SENDREC) {
        buf = br_ssl_engine_sendrec_buf(&ctx->sc.eng, &len);
        if (len > 0) {
            n = ct_transport_send(ctx->transport, buf, len);
            if (n > 0) {
                br_ssl_engine_sendrec_ack(&ctx->sc.eng, n);
            } else if (n < 0) {
                /*
                 * Send failed. If the peer already closed the connection
                 * (orderly release or disconnect), this is expected —
                 * e.g., server sent "Connection: close" and shut down.
                 * Treat as closed, not as an error.
                 */
                if (ct_transport_peer_closed(ctx->transport)) {
                    br_ssl_engine_close(&ctx->sc.eng);
                    ctx->state = kMacTLS_Closed;
                    return ctx->state;
                }
                ctx->state = kMacTLS_Error;
                ctx->error = kMacTLS_ErrWrite;
                return ctx->state;
            }
        }
    }

    /* Receive: OT -> BearSSL */
    if (st & BR_SSL_RECVREC) {
        buf = br_ssl_engine_recvrec_buf(&ctx->sc.eng, &len);
        if (len > 0) {
            n = ct_transport_recv(ctx->transport, buf, len);
            if (n > 0) {
                br_ssl_engine_recvrec_ack(&ctx->sc.eng, n);
            } else if (n < 0) {
                /* Recv failed — if peer closed, treat as normal close */
                if (ct_transport_peer_closed(ctx->transport)) {
                    br_ssl_engine_close(&ctx->sc.eng);
                    ctx->state = kMacTLS_Closed;
                    return ctx->state;
                }
                ctx->state = kMacTLS_Error;
                ctx->error = kMacTLS_ErrRead;
                return ctx->state;
            }
        }
    }

    /* Update state based on what BearSSL is doing */
    st = br_ssl_engine_current_state(&ctx->sc.eng);

    if (st & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) {
        /* Application data channels are open — handshake is done */
        ctx->state = kMacTLS_Connected;
    } else if (st & (BR_SSL_SENDREC | BR_SSL_RECVREC)) {
        /* Only record-level I/O — still handshaking */
        ctx->state = kMacTLS_Handshaking;
    }

    return ctx->state;
}

void MacTLS_Close(MacTLS_Context *ctx)
{
    if (ctx == NULL) return;

    /*
     * A close_notify can only be sent once there are keys to encrypt it with
     * (Gateway patch - see PATCHES.md).
     *
     * This used to run for kMacTLS_Handshaking too. tls13_active is set the
     * moment ServerHello selects TLS 1.3, which is well before the key
     * schedule has produced the write keys, so tearing a connection down
     * mid-handshake encrypted an alert with a zeroed record context: key_len
     * 0 and cipher_suite 0. That takes the AES-GCM branch and hands
     * br_aes_ct_ctr_init a zero-length key, which derives a nonsense round
     * count and runs off the end of the key schedule.
     *
     * Closing a browser, or navigating away, tears down connections whose
     * handshakes are still in flight -- so this was reachable simply by
     * leaving a page while it was still loading.
     */
    if (ctx->state == kMacTLS_Connected) {

        if (ctx->tls13_active && ctx->hs13.write_ctx.key_len != 0) {
            /*
             * TLS 1.3 close: send a close_notify alert.
             *
             * In TLS 1.3, close_notify is an encrypted alert (type 21,
             * value 0 = close_notify). We encrypt it with our write_ctx
             * and send it directly.
             */
            unsigned char alert[2];
            unsigned char ciphertext[2 + 1 + TLS13_TAG_SIZE];
            size_t ct_len;
            unsigned char record[5 + sizeof(ciphertext)];

            alert[0] = 1;  /* warning level */
            alert[1] = 0;  /* close_notify */

            if (tls13_record_encrypt(&ctx->hs13.write_ctx,
                                     alert, 2,
                                     TLS13_CT_ALERT,
                                     ciphertext, &ct_len) == 0) {
                record[0] = TLS13_CT_APPLICATION_DATA;
                record[1] = 0x03;
                record[2] = 0x03;
                record[3] = (unsigned char)(ct_len >> 8);
                record[4] = (unsigned char)(ct_len);
                memcpy(record + 5, ciphertext, ct_len);

                /* Best-effort send — don't retry on failure */
                ct_transport_send(ctx->transport, record, 5 + ct_len);
            }
        } else {
            /*
             * TLS 1.2 close: use BearSSL's close mechanism.
             */
            br_ssl_engine_close(&ctx->sc.eng);
            ctx->state = kMacTLS_Closing;

            /* Pump up to 10 times to flush the close_notify */
            {
                int flush_attempts;
                for (flush_attempts = 0; flush_attempts < 10; flush_attempts++) {
                    MacTLS_State pump_state = MacTLS_Pump(ctx);
                    if (br_ssl_engine_current_state(&ctx->sc.eng) == BR_SSL_CLOSED)
                        break;
                    if (pump_state == kMacTLS_Error)
                        break;
                }
            }
        }
    }

    /* Close the TCP connection */
    if (ctx->transport) {
        ct_transport_close(ctx->transport);
        ct_transport_destroy(ctx->transport);
        ctx->transport = NULL;
    }

    ctx->state = kMacTLS_Closed;

    /* Zero sensitive data before freeing — Mac OS 9 has no memory protection */
    {
        volatile unsigned char *p = (volatile unsigned char *)ctx;
        size_t i;
        for (i = 0; i < sizeof(MacTLS_Context); i++) p[i] = 0;
    }

    DisposePtr((Ptr)ctx);
}

/* ── Data transfer ── */

/*
 * Push whatever is left of the staged record at the transport.
 *
 * Returns -1 on a transport error, 0 otherwise; the caller checks
 * tls13_out_len against tls13_out_sent to see whether anything is still owed.
 */
static int tls13_flush_out(MacTLS_Context *ctx)
{
    /*
     * The staged record is the five header bytes followed by the ciphertext,
     * held in two buffers but sent as one stream, so out_sent counts across
     * both. Keeping them separate leaves the ciphertext at the alignment
     * BearSSL's cipher code was handed everywhere else.
     */
    while (ctx->tls13_out_sent < ctx->tls13_out_len) {
        const unsigned char *from;
        size_t               avail;
        int                  n;

        if (ctx->tls13_out_sent < 5) {
            from  = ctx->tls13_out_hdr + ctx->tls13_out_sent;
            avail = 5 - ctx->tls13_out_sent;
        } else {
            from  = ctx->tls13_enc_buf + (ctx->tls13_out_sent - 5);
            avail = ctx->tls13_out_len - ctx->tls13_out_sent;
        }

        n = ct_transport_send(ctx->transport, from, avail);
        if (n < 0) return -1;
        if (n == 0) break;                  /* flow controlled; try later */
        ctx->tls13_out_sent += (size_t)n;
    }
    return 0;
}

int MacTLS_Write(MacTLS_Context *ctx, const void *data, size_t len)
{
    if (ctx->state != kMacTLS_Connected) return -1;

    if (ctx->tls13_active) {
        /*
         * TLS 1.3 write path.
         *
         * Encrypt the plaintext into a TLS 1.3 record and send it directly,
         * bypassing BearSSL's sendapp buffer. Max plaintext per record is
         * 16384 (2^14), so cap at that.
         *
         * The record is staged whole -- header and ciphertext in one buffer --
         * and this function will not encrypt another until the last one has
         * left entirely. It used to send the header, then loop over the
         * ciphertext and break out if the transport went flow controlled, and
         * then report the whole plaintext as written. The remainder of that
         * record was never sent, so the peer received a truncated one and
         * closed the connection without answering.
         *
         * On Mac OS 9 that was invisible: OTSnd on a non-blocking endpoint
         * either takes everything or returns kOTFlowErr having sent nothing,
         * so a partial send effectively never happened. Winsock's send()
         * returns a partial count as a matter of course once the socket buffer
         * fills, which a several-kilobyte OAuth request does easily -- and the
         * first thing the Windows port did was fail to refresh a token.
         */
        unsigned char *ciphertext = ctx->tls13_enc_buf;
        size_t ct_len;
        int ret;

        /* Finish the previous record before starting another. */
        if (ctx->tls13_out_sent < ctx->tls13_out_len) {
            if (tls13_flush_out(ctx) < 0) return -1;
            if (ctx->tls13_out_sent < ctx->tls13_out_len) return 0;
        }

        if (len > TLS13_MAX_PLAINTEXT) len = TLS13_MAX_PLAINTEXT;

        ret = tls13_record_encrypt(&ctx->hs13.write_ctx,
                                   data, len,
                                   TLS13_CT_APPLICATION_DATA,
                                   ciphertext, &ct_len);
        if (ret != 0) return -1;

        ctx->tls13_out_hdr[0] = TLS13_CT_APPLICATION_DATA;  /* 0x17 */
        ctx->tls13_out_hdr[1] = 0x03;
        ctx->tls13_out_hdr[2] = 0x03;
        ctx->tls13_out_hdr[3] = (unsigned char)(ct_len >> 8);
        ctx->tls13_out_hdr[4] = (unsigned char)(ct_len);

        ctx->tls13_out_len  = 5 + ct_len;
        ctx->tls13_out_sent = 0;

        if (tls13_flush_out(ctx) < 0) return -1;

        /*
         * The plaintext is accounted for even if the record is still going
         * out: it is staged, it belongs to this context, and no further
         * plaintext will be encrypted until it has all left. MacTLS_Pump
         * keeps pushing it, so a caller that stops writing and starts reading
         * -- which is exactly what an HTTP request does -- still drains it.
         */
        return (int)len;
    }

    /*
     * TLS 1.2 write path (BearSSL).
     *
     * Get BearSSL's sendapp buffer — this is where plaintext goes
     * before BearSSL encrypts it into a TLS record.
     */
    {
        unsigned char *bbuf;
        size_t avail;

        bbuf = br_ssl_engine_sendapp_buf(&ctx->sc.eng, &avail);
        if (bbuf == NULL || avail == 0) return 0;

        if (len > avail) len = avail;
        memcpy(bbuf, data, len);

        /*
         * Tell BearSSL we wrote len bytes. BearSSL will encrypt them
         * on the next engine cycle and make them available via sendrec.
         */
        br_ssl_engine_sendapp_ack(&ctx->sc.eng, len);

        /*
         * Flush — tell BearSSL to wrap what we've written into a TLS
         * record now, rather than waiting for more data. For HTTP
         * requests, you want each write to go out promptly.
         */
        br_ssl_engine_flush(&ctx->sc.eng, 0);

        return (int)len;
    }
}

int MacTLS_Read(MacTLS_Context *ctx, void *buf, size_t len)
{
    /*
     * Allow reads in Connected, Closing, and even Closed states —
     * the server may have sent a response followed by a close,
     * and we need to let the app drain any buffered plaintext
     * before the connection is fully torn down.
     */
    if (ctx->state != kMacTLS_Connected &&
        ctx->state != kMacTLS_Closing &&
        ctx->state != kMacTLS_Closed) return -1;

    if (ctx->tls13_active) {
        /*
         * TLS 1.3 read path.
         *
         * Return data from tls13_app_buf (populated by tls13_recv_records
         * during MacTLS_Pump).
         */
        size_t avail = ctx->tls13_app_len - ctx->tls13_app_offset;
        if (avail == 0) return 0;

        if (len > avail) len = avail;
        memcpy(buf, ctx->tls13_app_buf + ctx->tls13_app_offset, len);
        ctx->tls13_app_offset += len;

        return (int)len;
    }

    /*
     * TLS 1.2 read path (BearSSL).
     *
     * Get BearSSL's recvapp buffer — this is where decrypted
     * plaintext lands after BearSSL processes incoming TLS records.
     */
    {
        unsigned char *src;
        size_t avail;

        src = br_ssl_engine_recvapp_buf(&ctx->sc.eng, &avail);
        if (src == NULL || avail == 0) return 0;

        if (len > avail) len = avail;
        memcpy(buf, src, len);

        /* Tell BearSSL we consumed len bytes */
        br_ssl_engine_recvapp_ack(&ctx->sc.eng, len);

        return (int)len;
    }
}

size_t MacTLS_Available(const MacTLS_Context *ctx)
{
    if (ctx->state != kMacTLS_Connected) return 0;

    if (ctx->tls13_active) {
        return ctx->tls13_app_len - ctx->tls13_app_offset;
    }

    {
        unsigned char *buf;
        size_t avail;

        buf = br_ssl_engine_recvapp_buf(
            &((MacTLS_Context *)ctx)->sc.eng, &avail);
        return (buf != NULL) ? avail : 0;
    }
}

/* ── Status ── */

MacTLS_State MacTLS_GetState(const MacTLS_Context *ctx)
{
    return ctx->state;
}

MacTLS_Error MacTLS_GetError(const MacTLS_Context *ctx)
{
    return ctx->error;
}

void MacTLS_GetCounters(const MacTLS_Context *ctx,
                        unsigned long *sent, unsigned long *received)
{
    if (ctx == NULL || ctx->transport == NULL) {
        if (sent != NULL)     *sent = 0;
        if (received != NULL) *received = 0;
        return;
    }
    ct_transport_counters(ctx->transport, sent, received);
}

void MacTLS_GetAppKeyFingerprints(const MacTLS_Context *ctx,
                                  unsigned long *stashed,
                                  unsigned long *installed)
{
    if (stashed != NULL)
        *stashed = (ctx == NULL) ? 0 : (unsigned long)ctx->hs13.app_write_fp_stash;
    if (installed != NULL)
        *installed = (ctx == NULL) ? 0 : (unsigned long)ctx->hs13.app_write_fp_install;
}

uint16_t MacTLS_GetCipherSuite(const MacTLS_Context *ctx)
{
    return (ctx == NULL) ? 0 : ctx->hs13.cipher_suite;
}

unsigned int MacTLS_GetAlert(const MacTLS_Context *ctx)
{
    return (ctx == NULL) ? 0 : ctx->tls13_alert;
}

size_t MacTLS_GetPending(const MacTLS_Context *ctx,
                         unsigned char *head, size_t headcap)
{
    size_t n;

    if (ctx == NULL) return 0;

    n = ctx->tls13_recv_len;
    if (head != NULL && headcap > 0) {
        size_t copy = (n < headcap) ? n : headcap;

        memset(head, 0, headcap);
        if (copy > 0) memcpy(head, ctx->tls13_recv_buf, copy);
    }
    return n;
}

long MacTLS_GetTransportError(const MacTLS_Context *ctx)
{
    if (ctx->transport) {
        return ct_transport_last_error(ctx->transport);
    }
    return 0;
}

MacTLS_Phase MacTLS_GetPhase(const MacTLS_Context *ctx)
{
    if (ctx == NULL || ctx->transport == NULL) return kMacTLS_PhaseIdle;

    switch (ct_transport_state(ctx->transport)) {
    case kCTransport_ResolvingDNS: return kMacTLS_PhaseResolving;
    case kCTransport_Connecting:   return kMacTLS_PhaseConnecting;
    case kCTransport_Connected:    return kMacTLS_PhaseConnected;
    case kCTransport_Closing:      return kMacTLS_PhaseClosing;
    case kCTransport_Closed:       return kMacTLS_PhaseClosed;
    case kCTransport_Error:        return kMacTLS_PhaseFailed;
    default:                        return kMacTLS_PhaseIdle;
    }
}

uint32_t MacTLS_GetResolvedAddress(const MacTLS_Context *ctx)
{
    if (ctx == NULL || ctx->transport == NULL) return 0;
    return ct_transport_peer_ipv4(ctx->transport);
}

int MacTLS_GetBearSSLError(const MacTLS_Context *ctx)
{
    /*
     * TLS 1.3 handshake errors are recorded in hs13.error using the
     * same BR_ERR_* codes BearSSL uses. Surface those first so callers
     * get meaningful diagnostics regardless of which path failed.
     */
    if (ctx->hs13.error != 0) return ctx->hs13.error;
    return br_ssl_engine_last_error(&((MacTLS_Context *)ctx)->sc.eng);
}

MacTLS_Version MacTLS_GetVersion(const MacTLS_Context *ctx)
{
    /* Only meaningful once the handshake has completed. tls13_active is
     * latched true after a successful TLS 1.3 handshake; otherwise the
     * connection ran through BearSSL's T0 engine, which we pin to 1.2. */
    if (ctx->state != kMacTLS_Connected) return kMacTLS_VersionUnknown;
    return ctx->tls13_active ? kMacTLS_Version13 : kMacTLS_Version12;
}

/* ── Configuration ── */

MacTLS_Config *MacTLS_ConfigCreate(void)
{
    return (MacTLS_Config *)NewPtrClear(sizeof(MacTLS_Config));
}

void MacTLS_ConfigFree(MacTLS_Config *cfg)
{
    if (cfg) DisposePtr((Ptr)cfg);
}

MacTLS_Error MacTLS_ConfigAddCA(MacTLS_Config *cfg,
                                const void *der, size_t len)
{
    /*
     * TODO: implement DER certificate decoding and storage.
     * For now, custom CAs require building a br_x509_trust_anchor
     * array manually and setting cfg->custom_tas directly.
     */
    (void)cfg; (void)der; (void)len;
    return kMacTLS_ErrMemory;
}

/* ── Entropy ── */

void MacTLS_AddEntropy(const void *data, size_t len)
{
    entropy_add(data, len);
}
