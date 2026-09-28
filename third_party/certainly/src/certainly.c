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

/* ------------------------------------------------------------------ */
/* Known-answer test                                                   */
/* ------------------------------------------------------------------ */

/*
 * Encrypt RFC 8439 section 2.8.2 and compare against the published answer.
 *
 * This exists because a record that the peer rejects with bad_record_mac
 * accuses two things at once -- the cipher, and the way this library drives it
 * -- and nothing in the failure separates them. The vector's plaintext is 114
 * bytes, which spans several ChaCha20 blocks, so it exercises the same path a
 * real request takes rather than the single block a Finished message fits in.
 *
 * Returns 0 on success, 1 if the ciphertext is wrong, 2 if only the tag is.
 */
int MacTLS_SelfTest(void)
{
    static const unsigned char key[32] = {
        0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
        0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
        0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
        0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f
    };
    static const unsigned char nonce[12] = {
        0x07, 0x00, 0x00, 0x00, 0x40, 0x41,
        0x42, 0x43, 0x44, 0x45, 0x46, 0x47
    };
    static const unsigned char aad[12] = {
        0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1,
        0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7
    };
    static const char plain[] =
        "Ladies and Gentlemen of the class of \'99: If I could offer you "
        "only one tip for the future, sunscreen would be it.";
    static const unsigned char want_ct[114] = {
        0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb, 0x7b, 0x86, 0xaf, 0xbc,
        0x53, 0xef, 0x7e, 0xc2, 0xa4, 0xad, 0xed, 0x51, 0x29, 0x6e, 0x08, 0xfe,
        0xa9, 0xe2, 0xb5, 0xa7, 0x36, 0xee, 0x62, 0xd6, 0x3d, 0xbe, 0xa4, 0x5e,
        0x8c, 0xa9, 0x67, 0x12, 0x82, 0xfa, 0xfb, 0x69, 0xda, 0x92, 0x72, 0x8b,
        0x1a, 0x71, 0xde, 0x0a, 0x9e, 0x06, 0x0b, 0x29, 0x05, 0xd6, 0xa5, 0xb6,
        0x7e, 0xcd, 0x3b, 0x36, 0x92, 0xdd, 0xbd, 0x7f, 0x2d, 0x77, 0x8b, 0x8c,
        0x98, 0x03, 0xae, 0xe3, 0x28, 0x09, 0x1b, 0x58, 0xfa, 0xb3, 0x24, 0xe4,
        0xfa, 0xd6, 0x75, 0x94, 0x55, 0x85, 0x80, 0x8b, 0x48, 0x31, 0xd7, 0xbc,
        0x3f, 0xf4, 0xde, 0xf0, 0x8e, 0x4b, 0x7a, 0x9d, 0xe5, 0x76, 0xd2, 0x65,
        0x86, 0xce, 0xc6, 0x4b, 0x61, 0x16,
    };
    static const unsigned char want_tag[16] = {
        0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a, 0x7e, 0x90, 0x2e, 0xcb,
        0xd0, 0x60, 0x06, 0x91,
    };

    unsigned char buf[114];
    unsigned char tag[16];

    memcpy(buf, plain, sizeof(buf));
    br_poly1305_ctmul_run(key, nonce, buf, sizeof(buf),
                          aad, sizeof(aad), tag,
                          br_chacha20_ct_run, 1 /* encrypt */);

    if (memcmp(buf, want_ct, sizeof(want_ct)) != 0) return 1;
    if (memcmp(tag, want_tag, sizeof(want_tag)) != 0) return 2;

    /*
     * And back again. Decryption is a different call with a different flag,
     * and it is the direction that matters once a peer stops rejecting what we
     * send: a record we cannot open looks, from above, exactly like a socket
     * that failed.
     */
    memcpy(buf, want_ct, sizeof(buf));
    br_poly1305_ctmul_run(key, nonce, buf, sizeof(buf),
                          aad, sizeof(aad), tag,
                          br_chacha20_ct_run, 0 /* decrypt */);

    if (memcmp(buf, plain, sizeof(buf)) != 0) return 3;
    if (memcmp(tag, want_tag, sizeof(want_tag)) != 0) return 4;
    return 0;
}

MacTLS_Error MacTLS_Init(void)
{
    entropy_init();
    return kMacTLS_OK;
}

void MacTLS_Shutdown(void)
{
    /* Currently nothing to clean up at library level */
}

/* ── X.509 validation-identity guard (PATCHES.md §34) ── */

/*
 * Both TLS engines' validation is pointed at this vtable rather than at the
 * real validator (`xc`) directly (installed in setup_bearssl()), so that
 * whatever name a handshake's SNI actually carried -- the real host, an
 * override, or none at all (MacTLS_SetSNI) -- start_chain() always
 * receives the connection's real host. tunnel_sni changes only what goes
 * out on the wire; this is what keeps the certificate check from ever
 * moving with it.
 *
 * Every other call forwards straight through to the real validator,
 * unchanged, so ordinary chain validation -- trust anchors, signatures,
 * dates, the name check itself -- is untouched.
 */
static void sniguard_start_chain(const br_x509_class **ctx,
                                 const char *server_name)
{
    MacTLS_SniGuardCtx *gc = (MacTLS_SniGuardCtx *)(void *)ctx;
    const br_x509_class **real =
        (const br_x509_class **)(void *)&gc->owner->xc.vtable;

    (void)server_name;   /* ignored on purpose -- see the comment above */
    (*real)->start_chain(real, gc->owner->host);
}

static void sniguard_start_cert(const br_x509_class **ctx, uint32_t length)
{
    MacTLS_SniGuardCtx *gc = (MacTLS_SniGuardCtx *)(void *)ctx;
    const br_x509_class **real =
        (const br_x509_class **)(void *)&gc->owner->xc.vtable;

    (*real)->start_cert(real, length);
}

static void sniguard_append(const br_x509_class **ctx,
                            const unsigned char *buf, size_t len)
{
    MacTLS_SniGuardCtx *gc = (MacTLS_SniGuardCtx *)(void *)ctx;
    const br_x509_class **real =
        (const br_x509_class **)(void *)&gc->owner->xc.vtable;

    (*real)->append(real, buf, len);
}

static void sniguard_end_cert(const br_x509_class **ctx)
{
    MacTLS_SniGuardCtx *gc = (MacTLS_SniGuardCtx *)(void *)ctx;
    const br_x509_class **real =
        (const br_x509_class **)(void *)&gc->owner->xc.vtable;

    (*real)->end_cert(real);
}

static unsigned sniguard_end_chain(const br_x509_class **ctx)
{
    MacTLS_SniGuardCtx *gc = (MacTLS_SniGuardCtx *)(void *)ctx;
    const br_x509_class **real =
        (const br_x509_class **)(void *)&gc->owner->xc.vtable;

    return (*real)->end_chain(real);
}

static const br_x509_pkey *sniguard_get_pkey(
    const br_x509_class *const *ctx, unsigned *usages)
{
    const MacTLS_SniGuardCtx *gc =
        (const MacTLS_SniGuardCtx *)(const void *)ctx;
    const br_x509_class **real =
        (const br_x509_class **)(void *)&gc->owner->xc.vtable;

    return (*real)->get_pkey(real, usages);
}

static const br_x509_class sni_guard_vtable = {
    sizeof(MacTLS_SniGuardCtx),
    sniguard_start_chain,
    sniguard_start_cert,
    sniguard_append,
    sniguard_end_cert,
    sniguard_end_chain,
    sniguard_get_pkey
};

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

    /*
     * Point the engine's validation at the guard rather than at `xc`
     * directly (PATCHES.md §34), so start_chain() always receives the real
     * host regardless of what name MacTLS_SetSNI later puts on the wire.
     * MacTLS_SetInsecure() overrides this again, deliberately bypassing the
     * guard along with validation itself.
     */
    ctx->sni_guard.vtable = &sni_guard_vtable;
    ctx->sni_guard.owner  = ctx;
    br_ssl_engine_set_x509(&ctx->sc.eng, &ctx->sni_guard.vtable);

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
     * br_ssl_client_init_full already set up ctx->xc, and we point the TLS
     * 1.3 context at the same guard the 1.2 engine goes through above, so
     * both share the §34 host substitution rather than one of them reaching
     * `xc` directly.
     */
    tls13_handshake_init(&ctx->hs13);
    ctx->hs13.x509_ctx = &ctx->sni_guard.vtable;
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

/*
 * First-record compatibility (PATCHES.md §30): reset the client engine with
 * the ClientHello record's own version dropped to TLS 1.0, not just the
 * handshake's.
 *
 * br_ssl_client_reset() stamps version_min into version_out
 * (ssl_client.c:48) and then, before it returns, runs jump_handshake()
 * (ssl_engine.c:1476) -- whose processor never leaves an unfinished
 * outgoing record. So the ClientHello is assembled and flush-record()
 * bakes its 5-byte header through sendpld_flush() using version_out at
 * that instant (ssl_engine.c:1101): a stamp applied after the reset
 * returns never reaches the wire, because the header bytes are already
 * fixed in the output buffer by then.
 *
 * version_min is the only lever the header reads while the reset runs, so
 * it is dropped to 0x0301 across the call and restored to setup_bearssl()'s
 * 0x0303 pin straight after. That pin's only other consumer is the
 * read-ServerHello range check (ssl_hs_client.t0:656), which runs long
 * after this returns, so restoring it leaves that check exactly as strict
 * as before. version_max is left untouched throughout: it supplies the
 * ClientHello's legacy_version (ssl_hs_client.t0:462) and the ServerHello
 * upper bound, both staying 0x0303.
 *
 * Every other stack puts 0x0301 in that header, and a version-intolerant
 * middlebox forwards only 03 01 first records, so 03 03 there is both
 * unusual and fragile. read-ServerHello overwrites version_out with the
 * negotiated version, so only the pre-negotiation flight is affected.
 */
static int client_first_record_compat(MacTLS_Context *ctx, const char *name)
{
    int ok;
    uint16_t saved_min = ctx->sc.eng.version_min;

    ctx->sc.eng.version_min = BR_TLS10;
    ok = br_ssl_client_reset(&ctx->sc, name, 0);
    ctx->sc.eng.version_min = saved_min;
    return ok;
}

/*
 * The name a handshake offers as SNI (PATCHES.md §34, MacTLS_SetSNI): the
 * dial hostname by default, an override, or none. Used only to shape the
 * ClientHello -- every X.509 start_chain() call goes through sni_guard
 * instead, which always substitutes ctx->host, so this has no bearing on
 * what gets validated.
 */
static const char *eff_sni(const MacTLS_Context *ctx)
{
    if (ctx->sni_mode == 2) return NULL;
    if (ctx->sni_mode == 1) return ctx->sni_override;
    return ctx->host;
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

/*
 * Adopted, but for a far end with no TLS 1.3: BearSSL's 1.2 engine drives
 * from the first pump, and the 1.3 ClientHello is never sent -- so there is
 * no ServerHello to fall back from and no reconnect to perform. The engine
 * is reset here (hostname for SNI), exactly as the fallback path does after
 * re-arming it; setup_bearssl() deliberately leaves that to its callers.
 */
MacTLS_Context *MacTLS_CreateOnEndpointTLS12(const char *host, CTSocket sock)
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

    /*
     * Nothing to re-arm: the engine has never run, so set_buffer() in
     * setup_bearssl() is its first arming. The reset only fails on a name
     * too long for the engine or an RNG that will not seed. It goes through
     * client_first_record_compat() like the fallback path, so the hello's
     * record header is 03 01 here too (PATCHES.md §30).
     */
    if (!client_first_record_compat(ctx, eff_sni(ctx))) {
        ctx->state = kMacTLS_Error;
        ctx->error = kMacTLS_ErrHandshake;
        return ctx;
    }

    ctx->force_tls12 = true;
    ctx->tls13_active = false;
    ctx->tls13_started = false;

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
    /* The peer has closed and this pass read nothing: no more bytes, ever. */
    int closed_now = 0;

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
        } else {
            closed_now = ct_transport_peer_closed(ctx->transport);
            /*
             * The transports report an orderly close differently: Win32
             * returns n == 0 with ct_transport_peer_closed() set
             * (transport_win32.c), OT returns n < 0 once ordRel has arrived
             * and its queue is drained (transport_ot.c). A close check that
             * lived only in the n < 0 arm never fired on Win32, the
             * handshake read WantRead forever, and it died on the 30-second
             * timeout instead of on the peer's FIN (PATCHES.md §31). Test
             * peer_closed on both arms.
             *
             * If the peer has closed we may still have pending data in
             * tls13_recv_buf that needs to be processed. Don't return
             * Closed immediately — fall through to handshake_step to
             * drain the buffer. If there's no pending data AND no
             * handshake work to do, the handshake code will report
             * WantRead and we'll come back here next time with still no
             * data — at which point we mark Closed.
             */
            if (ct_transport_peer_closed(ctx->transport) &&
                ctx->tls13_recv_len == 0 &&
                ctx->hs13.plain_offset >= ctx->hs13.plain_len) {
                ctx->state = kMacTLS_Closed;
                return ctx->state;
            }
            if (n < 0 && !ct_transport_peer_closed(ctx->transport)) {
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
                             eff_sni(ctx));

    switch (r) {
    case kTLS13_OK:
        /* State advanced — pump again on next call */
        ctx->state = kMacTLS_Handshaking;
        break;

    case kTLS13_WantRead:
        /*
         * Need more data from network -- unless the peer has already
         * closed and this pass brought nothing, in which case the
         * handshake is stuck on a record that can never complete, and
         * waiting would only reach the same failure 30 seconds later.
         */
        if (closed_now) {
            ctx->state = kMacTLS_Error;
            ctx->error = kMacTLS_ErrRead;
            break;
        }
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

            /*
             * An adopted transport -- SMTP STARTTLS (gw_mail.c), which hands
             * Certainly a socket already mid-conversation via
             * ct_transport_adopt() -- has no route to redial: adopt stores
             * no host or port, so port reads 0 here, and even with them a
             * fresh dial would bypass the plaintext STARTTLS prologue the
             * server is waiting on. What this used to do was dial port 0
             * directly: the adopted socket was torn down first, so the far
             * end saw a plain disconnect while Gateway reported an
             * empty-handed handshake failure that read like a proxy
             * problem, when the actual event is a version problem -- the
             * server chose TLS 1.2 (PATCHES.md §29). Fail with the original
             * connection intact and let fell_back_no_route name the remedy.
             */
            if (port == 0) {
                ctx->fell_back_no_route = true;
                ctx->state = kMacTLS_Error;
                ctx->error = kMacTLS_ErrHandshake;
                return ctx->state;
            }

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

            /*
             * Re-arm the engine before resetting the client, or the
             * fallback is dead on arrival (PATCHES.md §23).
             *
             * br_ssl_engine_fail() sets iomode = BR_IO_FAILED *and* err,
             * and br_ssl_engine_hs_reset() -- which is all
             * br_ssl_client_reset() calls -- clears neither. So a client
             * context that failed once stays failed through every reset,
             * reporting the same stale code for the rest of its life.
             * br_ssl_engine_set_buffer() is the only entry point that puts
             * iomode back to BR_IO_INOUT and err back to BR_ERR_OK, so it
             * has to run first. Suites, versions, trust anchors and the
             * seeded RNG all survive it; only the record state is reset.
             */
            br_ssl_engine_set_buffer(&ctx->sc.eng, ctx->iobuf,
                                     sizeof(ctx->iobuf), 0);

            /*
             * Re-init BearSSL for TLS 1.2 (the T0 engine will drive).
             * client_first_record_compat() is the reset itself, not a step
             * after it -- see PATCHES.md §30 for why the stamp has to ride
             * inside the call.
             */
            if (!client_first_record_compat(ctx, eff_sni(ctx))) {
                /*
                 * Nothing is retryable past this point: the reset only
                 * fails on a name too long for the engine or an RNG that
                 * will not seed, and neither improves by trying again.
                 * Report it rather than looping on a context that can no
                 * longer handshake, which is what hid this in the first
                 * place.
                 */
                ctx->state = kMacTLS_Error;
                ctx->error = kMacTLS_ErrHandshake;
                return ctx->state;
            }

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
     * Set when the loop below stops on a complete record it cannot decrypt
     * yet for want of app_buf room -- as opposed to an incomplete one it is
     * still waiting on bytes for. See the peer_closed check after the loop
     * (PATCHES.md §31).
     */
    int waiting_for_room = 0;
    /*
     * Set when this call tried to read and got nothing. A close is honoured
     * only then: bytes that arrived on this very pass, or a pass that
     * skipped the read because recv_buf was full, may be followed by more
     * still queued, whatever peer_closed already says.
     */
    int read_dry = 0;

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
        } else {
            read_dry = 1;
            /*
             * The two transports disagree on how a close is reported: OT
             * returns n < 0 once ordRel has arrived and its queue is drained
             * (transport_ot.c), while Win32 returns n == 0 with
             * ct_transport_peer_closed() set instead (transport_win32.c) --
             * so a test only in the n < 0 arm misses Win32's close and a
             * test only on n == 0 misses OT's. Reading peer_closed on both
             * arms handles either convention (PATCHES.md §31).
             *
             * Real errors (not a close) still bail immediately; a close
             * falls through so any already-buffered records get processed,
             * and the peer_closed decision after the loop below tells a
             * truncated trailing record apart from one merely waiting on
             * app_buf room.
             */
            if (n < 0 && !ct_transport_peer_closed(ctx->transport)) {
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
         * Make room before decrypting, and stop if there is not enough.
         *
         * The plaintext of this record has to go somewhere, and the only
         * somewhere is tls13_app_buf, which holds exactly one maximum record.
         * When the application reads more slowly than the peer sends -- which
         * on a 1999 Macintosh behind a browser fetching a dozen resources is
         * the normal case, not the exception -- unread bytes are still in
         * there when the next full-sized record lands.
         *
         * This used to decrypt anyway and copy what fit, discarding the rest,
         * with a comment that said "truncate if buffer full". That is a hole
         * punched in the middle of the byte stream: a chunked body loses sync
         * with its own framing a few kilobytes later and is reported as
         * malformed, and a Content-Length body simply arrives short.
         *
         * Leaving the record in recv_buf costs nothing -- it is already there,
         * and the next pump will find it again once the reader has drained.
         */
        {
            size_t held = ctx->tls13_app_len - ctx->tls13_app_offset;

            if (ctx->tls13_app_offset > 0) {
                if (held > 0)
                    memmove(ctx->tls13_app_buf,
                            ctx->tls13_app_buf + ctx->tls13_app_offset, held);
                ctx->tls13_app_len = held;
                ctx->tls13_app_offset = 0;
            }

            /*
             * Compare against the plaintext, not the record.
             *
             * record_len counts the ciphertext, which carries a content type
             * byte and a 16 byte tag on top of the plaintext. A maximum sized
             * record is 16384 of plaintext and so 16401 on the wire, while
             * this buffer holds exactly 16384 -- so testing record_len against
             * it refused every full sized record even when the buffer was
             * completely empty, and the connection stalled until its idle
             * timeout. Any response large enough to fill one record hit it.
             */
            if (record_type == TLS13_CT_APPLICATION_DATA) {
                size_t overhead = 1 + TLS13_TAG_SIZE;
                size_t plain = (record_len > overhead)
                                   ? (size_t)record_len - overhead : 0;
                size_t room = sizeof(ctx->tls13_app_buf) - ctx->tls13_app_len;

                /*
                 * An empty buffer always has room for a legal record, so
                 * needing more than all of it means the peer has exceeded
                 * what TLS 1.3 permits. Saying so beats waiting for space
                 * that cannot arrive.
                 */
                if (plain > sizeof(ctx->tls13_app_buf)) {
                    ctx->state = kMacTLS_Error;
                    ctx->error = kMacTLS_ErrRead;
                    return;
                }
                if (room < plain) {
                    waiting_for_room = 1;
                    break;              /* wait for the reader to drain */
                }
            }
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
            /*
             * Room was checked and the buffer compacted before this record was
             * decrypted, so the whole plaintext fits. Nothing is dropped here;
             * a short copy would be a hole in the stream.
             */
            if (ctx->tls13_app_len + dec_len > sizeof(ctx->tls13_app_buf)) {
                ctx->state = kMacTLS_Error;
                ctx->error = kMacTLS_ErrRead;
                return;
            }
            memcpy(ctx->tls13_app_buf + ctx->tls13_app_len, decrypted, dec_len);
            ctx->tls13_app_len += dec_len;

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
     * After processing all buffered records: the peer is gone, so no more
     * bytes are ever coming. Requiring recv_len == 0 here left a trailing
     * partial record -- one truncated by the close, which can never
     * complete -- stuck forever: the next pump finds the same unfinished
     * bytes, ct_transport_recv() reports the close again, and the loop
     * above breaks on the same incomplete record with nothing to time it
     * out (PATCHES.md §31, the same close-detection gap as
     * tls13_pump_handshake and MacTLS_Pump's BearSSL read, missed here
     * originally because this site's n < 0 arm already read as harmless).
     *
     * waiting_for_room is the one case where leftover bytes are not lost
     * data: a complete record is sitting there, blocked only on app_buf
     * space, and MacTLS_Read() draining app_buf will let a later pump
     * finish it -- closing now would discard it instead. The app can still
     * drain tls13_app_buf via MacTLS_Read() from the Closed state either
     * way, so nothing already delivered is lost by closing.
     *
     * Leftover bytes that are not a room-blocked record are a record cut
     * off by the close, and that is an error, not an end of stream. Called
     * Closed, a FIN injected mid-record would pass a truncated body off as
     * complete whenever the response has no length of its own to check it
     * against. A close at a record boundary stays Closed, as before.
     */
    if (read_dry && ct_transport_peer_closed(ctx->transport) &&
        !waiting_for_room) {
        if (ctx->tls13_recv_len > 0) {
            ctx->state = kMacTLS_Error;
            ctx->error = kMacTLS_ErrRead;
        } else {
            ctx->state = kMacTLS_Closed;
        }
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
    if (!ctx->force_tls12 &&
        !ctx->tls13_started && ctx->hs13.is_tls13 == false &&
        ctx->hs13.state == kTLS13_SendClientHello) {
        /*
         * First time through after TCP connect — start the TLS 1.3
         * handshake. The tls13_started flag prevents re-entry after
         * a TLS 1.2 fallback (where we've cleared the flag and want
         * BearSSL to drive). force_tls12 (PATCHES.md §32) skips this
         * branch entirely, so the BearSSL 1.2 engine drives from the
         * very first pump instead.
         */
        ctx->tls13_started = true;
        ctx->state = kMacTLS_Handshaking;
        return tls13_pump_handshake(ctx);
    }

    if (!ctx->force_tls12 &&
        ctx->tls13_started && ctx->state != kMacTLS_Connected) {
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
            } else if (ct_transport_peer_closed(ctx->transport)) {
                /*
                 * The peer is gone. OT reports that as n < 0 once ordRel has
                 * drained (transport_ot.c); Win32 reports it as n == 0
                 * instead (transport_win32.c) -- so this test has to read
                 * peer_closed directly rather than sit inside the n < 0 arm,
                 * or Win32's close is never seen and the handshake rides out
                 * its 30-second timeout instead of closing on the peer's
                 * close (PATCHES.md §31).
                 */
                br_ssl_engine_close(&ctx->sc.eng);
                ctx->state = kMacTLS_Closed;
                return ctx->state;
            } else if (n < 0) {
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
        /*
         * Only record-level I/O. That means "still handshaking" exactly
         * once -- before the handshake has ever finished (PATCHES.md §26).
         *
         * The engine runs on one buffer for both directions
         * (br_ssl_engine_set_buffer with bidi = 0), so it works one
         * direction at a time: while an outgoing record is being pushed
         * out, neither SENDAPP nor RECVAPP is offered and current_state()
         * is BR_SSL_SENDREC alone. That is the normal condition of a
         * connected session that has just been written to -- which is every
         * session, immediately after the request goes out. Reading it as a
         * return to handshaking sent the context backwards, and MacTLS_Read
         * answers -1 in any state but Connected, Closing or Closed, so the
         * caller was told the read failed while the connection was in
         * perfect health. BR_SSL_CLOSED above is the only way out of
         * Connected.
         */
        if (ctx->state != kMacTLS_Connected) {
            ctx->state = kMacTLS_Handshaking;
        }
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

unsigned int MacTLS_GetAlert(const MacTLS_Context *ctx)
{
    return (ctx == NULL) ? 0 : ctx->tls13_alert;
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

int MacTLS_GetTls13Error(const MacTLS_Context *ctx)
{
    if (ctx == NULL) return 0;
    return ctx->hs13.error;
}

int MacTLS_FallbackNoRoute(const MacTLS_Context *ctx)
{
    if (ctx == NULL) return 0;
    return ctx->fell_back_no_route ? 1 : 0;
}

/* ── Trust-any validator (testing only, PATCHES.md §33) ── */

/*
 * Accept any chain and return the end-entity public key, decoded from the
 * first certificate. Signatures, names, dates and trust anchors are all
 * ignored by design -- this exists to test against a far end whose
 * certificate cannot validate (wrong name, private CA), and must never be
 * enabled for anything carrying credentials. Modelled on BearSSL's own
 * knownkey engine (x509_knownkey.c), except the key comes out of the peer's
 * certificate via the decoder instead of being configured in advance.
 *
 * Only the first certificate is fed to the decoder; the rest of the chain
 * is skipped. An undecodable end-entity certificate fails the chain with
 * the decoder's own error, so the engine reports a handshake failure
 * rather than dereferencing a NULL key.
 *
 * MacTLS_InsecureCtx is defined once, in certainly_internal.h, rather than
 * kept in sync with a duplicate here.
 */
static void insecure_start_chain(const br_x509_class **ctx,
                                 const char *server_name)
{
    MacTLS_InsecureCtx *cc = (MacTLS_InsecureCtx *)(void *)ctx;

    (void)server_name;
    cc->cert_index = 0;
}

static void insecure_start_cert(const br_x509_class **ctx, uint32_t length)
{
    MacTLS_InsecureCtx *cc = (MacTLS_InsecureCtx *)(void *)ctx;

    (void)length;
    if (cc->cert_index == 0)
        br_x509_decoder_init(&cc->dc, 0, 0);
}

static void insecure_append(const br_x509_class **ctx,
                            const unsigned char *buf, size_t len)
{
    MacTLS_InsecureCtx *cc = (MacTLS_InsecureCtx *)(void *)ctx;

    if (cc->cert_index == 0)
        br_x509_decoder_push(&cc->dc, buf, len);
}

static void insecure_end_cert(const br_x509_class **ctx)
{
    MacTLS_InsecureCtx *cc = (MacTLS_InsecureCtx *)(void *)ctx;

    cc->cert_index++;
}

static unsigned insecure_end_chain(const br_x509_class **ctx)
{
    MacTLS_InsecureCtx *cc = (MacTLS_InsecureCtx *)(void *)ctx;

    if (cc->cert_index == 0)
        return BR_ERR_X509_EMPTY_CHAIN;
    return (unsigned)br_x509_decoder_last_error(&cc->dc);
}

static const br_x509_pkey *insecure_get_pkey(
    const br_x509_class *const *ctx, unsigned *usages)
{
    MacTLS_InsecureCtx *cc = (MacTLS_InsecureCtx *)(void *)ctx;

    /* Both uses permitted, as in BearSSL's own test tool (twrch.c). */
    if (usages != NULL)
        *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    return br_x509_decoder_get_pkey(&cc->dc);
}

static const br_x509_class insecure_vtable = {
    sizeof(MacTLS_InsecureCtx),
    insecure_start_chain,
    insecure_start_cert,
    insecure_append,
    insecure_end_cert,
    insecure_end_chain,
    insecure_get_pkey
};

/*
 * Testing only: stop validating the far end's certificate. Swaps both
 * validation paths -- BearSSL's 1.2 engine and the 1.3 state machine, which
 * shares its validator through hs13.x509_ctx -- onto the trust-any engine
 * above, bypassing the §34 guard entirely rather than routing through it:
 * there is no host to defend once nothing is being checked. Must be called
 * before the first Pump; the handshake has not run yet at that point, so
 * neither engine has touched a validator.
 */
void MacTLS_SetInsecure(MacTLS_Context *ctx)
{
    if (ctx == NULL) return;

    ctx->insecure.vtable = &insecure_vtable;
    br_ssl_engine_set_x509(&ctx->sc.eng, &ctx->insecure.vtable);
    ctx->hs13.x509_ctx = &ctx->insecure.vtable;
}

/* ── SNI override (testing/diagnosis, PATCHES.md §34) ── */

/*
 * Replace the server name the handshake sends as SNI. sni == NULL omits SNI
 * entirely; otherwise that name is sent instead of the connection's real
 * host. Must run before the first Pump: both engines are idle then, so
 * re-resetting BearSSL here is the same call the force-1.2 entry point and
 * the fallback make themselves. The 1.3 state machine picks the name up
 * through eff_sni() in MacTLS_Pump().
 *
 * This never touches validation. sni_guard (installed in setup_bearssl())
 * always hands start_chain() ctx->host, whatever name ends up here -- so an
 * omitted or overridden SNI changes only what is sent, never what is
 * checked, unless MacTLS_SetInsecure() has turned checking off entirely.
 */
void MacTLS_SetSNI(MacTLS_Context *ctx, const char *sni)
{
    if (ctx == NULL) return;

    if (sni == NULL) {
        ctx->sni_mode = 2;
    } else {
        if (strlen(sni) >= sizeof(ctx->sni_override))
            return;             /* overlong: keep the previous name */
        strcpy(ctx->sni_override, sni);
        ctx->sni_mode = 1;
    }
    if (ctx->state != kMacTLS_Connecting)
        return;             /* handshake running or failed: the name is fixed */
    /*
     * BearSSL reads its name only at reset. A fresh engine takes a reset as
     * ordinary initialisation; on the fallback path below the same call
     * re-arms it, so this stays consistent however the handshake proceeds.
     * A reset that fails here fails the context the same way the other
     * call sites do.
     */
    br_ssl_engine_set_buffer(&ctx->sc.eng, ctx->iobuf,
                             sizeof(ctx->iobuf), 0);
    if (!client_first_record_compat(ctx, eff_sni(ctx))) {
        ctx->state = kMacTLS_Error;
        ctx->error = kMacTLS_ErrHandshake;
    }
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
    if (ctx == NULL) return kMacTLS_VersionUnknown;
    if (ctx->state == kMacTLS_Connected)
        return ctx->tls13_active ? kMacTLS_Version13 : kMacTLS_Version12;
    /*
     * A peer that sends its ServerHello and then hangs up leaves state at
     * something other than Connected, so the test above used to answer
     * Unknown -- read by GWStream_Describe() as "without answering the
     * ClientHello", when it plainly did (PATCHES.md §31). BearSSL writes
     * the negotiated version into session.version the moment the
     * ServerHello arrives, so read it from there while the engine is still
     * around; 0 means no ServerHello was ever received, which is the case
     * this is meant to tell apart from the one above.
     */
    if (!ctx->tls13_active &&
        br_ssl_engine_get_version(&ctx->sc.eng) == BR_TLS12) {
        return kMacTLS_Version12;
    }
    return kMacTLS_VersionUnknown;
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

const char *MacTLS_EntropySource(void)
{
    return entropy_system_source();
}
