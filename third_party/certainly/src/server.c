/*
 * server.c - Certainly on the other side of the handshake.
 *
 * Gateway patch, for MITM of a CONNECT tunnel. Internet Explorer 4 asked for
 * https://host, and a proxy that answers "200 Connection Established" and then
 * relays bytes leaves IE talking to a 2026 server with a 1997 TLS stack, which
 * fails. So Gateway terminates that TLS itself, with a certificate it minted
 * for the host, and does its own TLS 1.3 to the origin.
 *
 * Much smaller than the client side, because none of what makes that hard
 * applies. There is no TLS 1.3 to negotiate -- the peer is a vintage browser,
 * and br_ssl_server_init_full_rsa already pins BR_TLS10 to BR_TLS12. There is
 * no certificate chain to validate, since we are the one presenting it. There
 * is no name to resolve and no connection to open: the socket arrives already
 * accepted. What is left is the record pump, which is the same shape as the
 * TLS 1.2 half of MacTLS_Pump().
 *
 * The cipher that matters is TLS_RSA_WITH_3DES_EDE_CBC_SHA. BearSSL has no RC4
 * and no single DES, and IE 4 and Netscape 4 have no AES, so 3DES is the whole
 * of the common ground. It is in BearSSL's full RSA server profile already.
 * A browser built for export-grade crypto has no 3DES either — SSL 3.0
 * export suites (RC4_40, RC2_40, DES40) via src/ssl3/ cover Netscape 3.04 Gold.
 */

#include "certainly.h"
#include "certainly_internal.h"

#include <string.h>
#include <stdio.h>

#include <bearssl.h>
/* Gateway: src is on this target's include path (CMakeLists.txt and
 * Makefile.win32 both put it there), so these are found without reaching
 * back out of third_party by relative path. */
#include <ssl3/ssl3.h>
#include <gw_log.h>

struct MacTLS_Server {
    MacTLS_State           state;
    MacTLS_Error           error;

    CTransport            *transport;
    br_ssl_server_context  sc;

    /*
     * Leaf then authority. Sending the authority as well costs a few hundred
     * bytes and means a browser that has installed it does not have to have
     * indexed it by subject to build the chain.
     */
    br_x509_certificate    chain[2];
    size_t                 chain_len;

    unsigned char          iobuf[CERTAINLY_IOBUF_SIZE];
    /* One-shot/limited hexdump throttle for GW_DEBUG_IO builds. Kept
     * unconditionally so the struct layout never depends on the flag. */
    int                    logged_raw;
    /* Set once the handshake completes, so a close before that can be told
     * apart from an ordinary end-of-session close. */
    int                    handshook;
    /*
     * How much the browser sent us, and how much of it arrived *after* our
     * first flight went out. When a browser abandons the handshake, rxAfterFF
     * is the whole diagnosis: 0 means it walked away on our ServerHello /
     * Certificate / ServerHelloDone without answering, anything else means it
     * sent a ClientKeyExchange (and possibly more) and then left. Paired with
     * the engine's incrypt flag (set only once its ChangeCipherSpec is in),
     * this says where in the exchange the browser gave up -- which is what
     * tells an SSLv2-hello abandonment that reaches the certificate apart from
     * one that dies at the client's own second flight.
     */
    size_t                 rxTotal;
    size_t                 rxAtFirstFlight;
    int                    firstFlightSeen;
    /* Set once the opening bytes of the client's hello have been logged. A
     * handshake that never completes, never fails through BearSSL and never
     * times out -- IE 3 on Windows 95 does exactly this -- otherwise leaves no
     * trace of what the client sent. The first bytes name the protocol: 16 03
     * is a TLS/SSL 3 record, 80.. an SSLv2-framed hello (version at byte 3-4),
     * and an 80.. hello whose version is 80 01 is Microsoft PCT, which is not
     * SSL and is not served. */
    int                    helloLogged;
};

MacTLS_Server *MacTLS_ServerCreate(CTSocket sock,
                                   const unsigned char *leaf, size_t leaf_len,
                                   const unsigned char *ca, size_t ca_len,
                                   const void *key)
{
    MacTLS_Server *s;
    const br_rsa_private_key *sk = (const br_rsa_private_key *)key;

    if (sock == CT_SOCKET_NONE) return NULL;

    if (leaf == NULL || leaf_len == 0 || sk == NULL) {
        ct_socket_close(sock);
        return NULL;
    }

    s = (MacTLS_Server *)NewPtrClear(sizeof(MacTLS_Server));
    if (s == NULL) {
        /* adopt() takes ownership unconditionally, so an early failure here
         * still has to close the socket itself. */
        ct_socket_close(sock);
        return NULL;
    }

    s->transport = ct_transport_adopt(sock);
    if (s->transport == NULL) {
        DisposePtr((Ptr)s);
        return NULL;
    }

    /*
     * The DER is not copied. Every certificate here comes from gw_ca.c, which
     * holds the authority for the life of the process and each leaf in a cache
     * entry -- and a cache entry can be recycled, so a session must not
     * outlive the handshake that read it. The handshake reads the chain while
     * sending the Certificate message, which is the first thing it does.
     */
    s->chain[0].data     = (unsigned char *)leaf;
    s->chain[0].data_len = leaf_len;
    s->chain_len = 1;
    /*
     * The authority certificate is deliberately never sent: Netscape
     * 3.04 Gold (Export) aborts the handshake on any chain carrying
     * it (verified by revert test), while every client in range
     * completes with the leaf alone. A peer that trusts the authority
     * already holds it locally, which is the only case where the chain
     * could validate anyway (RFC 5246 section 7.4.2: a self-signed root
     * may be omitted). (ca, ca_len) are kept for signature
     * compatibility and ignored.
     */
    (void)ca;
    (void)ca_len;

    br_ssl_server_init_full_rsa(&s->sc, s->chain, s->chain_len, sk);
    ssl3_server_init(&s->sc);
    br_ssl_engine_set_buffer(&s->sc.eng, s->iobuf, sizeof(s->iobuf), 0);

    /*
     * Our own pool on top of whatever BearSSL has, exactly as the client path
     * does. On Windows 95 RTM and NT 3.51 there is no system generator behind
     * it, so this is the only seeding the engine gets.
     */
    entropy_seed_engine(&s->sc.eng);

    if (!br_ssl_server_reset(&s->sc)) {
        s->state = kMacTLS_Error;
        s->error = kMacTLS_ErrHandshake;
        return s;
    }

    s->state = kMacTLS_Handshaking;
    return s;
}

/*
 * What the connection settled on, for the log. Which framing the hello
 * arrived in is worth a word: a browser that speaks the SSLv2-compatible
 * form is a browser that predates the one Gateway would otherwise assume.
 */
static void describe_hello(MacTLS_Server *s, char *out, size_t cap)
{
    size_t      rx_after = s->rxTotal - s->rxAtFirstFlight;
    const char *reached;

    /*
     * How far the browser got, stated as a plain fact rather than a verdict:
     * this describes the "done" line as well as the "abandoned" and "failed"
     * ones, so the surrounding log verb supplies the outcome. incrypt is the
     * receive-side crypto flag, set only after the client's ChangeCipherSpec,
     * so it marks a client that finished its own second flight. Short of that,
     * rx_after separates a client that answered our certificate with a
     * ClientKeyExchange from one that never replied at all -- the distinction
     * that says where an SSL 3.0 client (IE 3/4, Netscape) stops if it does.
     */
    if (s->sc.eng.incrypt)
        reached = "client completed its second flight";
    else if (rx_after > 0)
        reached = "client replied, no ChangeCipherSpec";
    else
        reached = "no client reply after our certificate";

    snprintf(out, cap,
             "%s hello, version %04x, suite %04x "
             "[%s; rx %lu, rx-after-flight %lu, in-rectype %u, incrypt %u]",
             s->sc.eng.ssl2_hello ? "SSLv2" : "native",
             s->sc.eng.session.version, s->sc.eng.session.cipher_suite,
             reached, (unsigned long)s->rxTotal, (unsigned long)rx_after,
             s->sc.eng.record_type_in, s->sc.eng.incrypt);
}

MacTLS_State MacTLS_ServerPump(MacTLS_Server *s)
{
    unsigned char *buf;
    size_t         len;
    unsigned       st;
    int            n;

    if (s == NULL) return kMacTLS_Error;
    if (s->state == kMacTLS_Error || s->state == kMacTLS_Closed)
        return s->state;

    /* Let the transport move whatever it has queued in either direction. */
    ct_transport_pump(s->transport);

    st = br_ssl_engine_current_state(&s->sc.eng);

#ifdef GW_DEBUG_IO
    if (!s->logged_raw) {
        size_t avail = 0;
        const unsigned char *pbuf = NULL;
        char raw[513];
        size_t dump, k, p = 0;
        if (s->sc.eng.hbuf_in && s->sc.eng.hlen_in >= 4) {
            pbuf = s->sc.eng.hbuf_in;
            avail = s->sc.eng.hlen_in;
        } else if (s->sc.eng.ibuf && s->sc.eng.ixa != s->sc.eng.ixb) {
            pbuf = s->sc.eng.ibuf + s->sc.eng.ixa;
            avail = s->sc.eng.ixb - s->sc.eng.ixa;
            if (avail > 5 && pbuf[0] == 0x16) { pbuf += 5; avail -= 5; }
        }
        if (pbuf && avail >= 4) {
            s->logged_raw = 1;
            dump = avail > 96 ? 96 : avail;
            for (k = 0; k < dump && p + 3 < sizeof(raw); k++) {
                int n = snprintf(raw + p, sizeof(raw) - p, "%s%02x", k ? " " : "", pbuf[k]);
                if (n < 0) break;
                p += (size_t)n;
            }
            gw_log("  ClientHello pre-filter avail %u: %s", (unsigned)avail, raw);
        }
    }
#endif

    if (st == BR_SSL_CLOSED) {
        int err = br_ssl_engine_last_error(&s->sc.eng);

        if (err == BR_ERR_OK) {
            /*
             * A clean close. Ordinary at the end of a session -- but a
             * browser that walks away mid-handshake lands here too, with no
             * error to report and, until now, nothing in the log. That is
             * the shape of a client that opens a connection and abandons it,
             * and the only way to tell which version it had been offered is
             * to say so here.
             */
            if (!s->handshook) {
                char hello[384];
                describe_hello(s, hello, sizeof(hello));
                gw_log("MITM handshake abandoned by the browser: %s", hello);
            }
            s->state = kMacTLS_Closed;
        } else {
            s->state = kMacTLS_Error;
            s->error = kMacTLS_ErrHandshake;
            {
                char hello[384];
                describe_hello(s, hello, sizeof(hello));
                if (err >= BR_ERR_SEND_FATAL_ALERT) {
                    gw_log("MITM handshake failed: BearSSL %d (alert %d), %s",
                           err, err - BR_ERR_SEND_FATAL_ALERT, hello);
                } else {
                    gw_log("MITM handshake failed: BearSSL %d, %s", err, hello);
                }
#ifdef GW_DEBUG_IO
                if (s->sc.client_suites_num == 0 && s->sc.eng.hbuf_in && s->sc.eng.hlen_in >= 6) {
                    char raw[193];
                    size_t dump = s->sc.eng.hlen_in > 64 ? 64 : s->sc.eng.hlen_in;
                    size_t k, p = 0;
                    for (k = 0; k < dump && p + 3 < sizeof(raw); k++) {
                        int n = snprintf(raw + p, sizeof(raw) - p, "%s%02x", k ? " " : "", s->sc.eng.hbuf_in[k]);
                        if (n < 0) break;
                        p += (size_t)n;
                    }
                    gw_log("  ClientHello raw %u bytes: %s", (unsigned)s->sc.eng.hlen_in, raw);
                }
#endif
            }
        }
        return s->state;
    }

    if (st & BR_SSL_SENDREC) {
        buf = br_ssl_engine_sendrec_buf(&s->sc.eng, &len);
        if (len > 0) {
            /*
             * The first bytes we ever send are our first flight, and the whole
             * ClientHello has been received and parsed by the time the engine
             * offers them. Freeze the received-byte count here so anything the
             * browser sends afterwards can be told apart from its hello.
             */
            if (!s->firstFlightSeen) {
                s->rxAtFirstFlight = s->rxTotal;
                s->firstFlightSeen = 1;
            }
#ifdef GW_DEBUG_IO
            if (len >= 5) {
                gw_log("  SEND rec %02x %02x%02x len %u",
                       buf[0], buf[1], buf[2],
                       (unsigned)(((unsigned)buf[3] << 8) | buf[4]));
                if (buf[0] == 22 && len > 5) {
                    size_t dump = len - 5;
                    size_t off = 0;
                    while (off < dump) {
                        char raw[193];
                        size_t chunk = dump - off > 32 ? 32 : dump - off;
                        size_t k, p = 0;
                        for (k = 0; k < chunk && p + 3 < sizeof(raw); k++) {
                            int nn = snprintf(raw + p, sizeof(raw) - p, "%s%02x",
                                              k ? " " : "", buf[5 + off + k]);
                            if (nn < 0) break;
                            p += (size_t)nn;
                        }
                        gw_log("  SEND payload [%u/%u]: %s", (unsigned)off, (unsigned)dump, raw);
                        off += chunk;
                    }
                    if (dump >= 4 && buf[5] == 0x02) {
                        size_t hs_len = ((size_t)buf[6] << 16) | ((size_t)buf[7] << 8) | buf[8];
                        size_t sh_off = 5 + 4;
                        size_t suite_off = sh_off + 2 + 32 + 1;
                        unsigned suite = 0xFFFF;
                        if (hs_len >= 2+32+1+2 && len >= suite_off + 2) {
                            unsigned sid_len = buf[sh_off + 2 + 32];
                            suite_off += 1 + sid_len;
                            if (len >= suite_off + 2)
                                suite = ((unsigned)buf[suite_off] << 8) | buf[suite_off+1];
                        }
                        gw_log("  ServerHello suite %04x", suite);
                    }
                }
            } else {
                gw_log("  SEND rec %u bytes (short)", (unsigned)len);
            }
#endif
            n = ct_transport_send(s->transport, buf, len);
            if (n > 0) {
                br_ssl_engine_sendrec_ack(&s->sc.eng, (size_t)n);
            } else if (n < 0) {
                /* The browser going away mid-handshake is ordinary: it is what
                 * happens when the user presses Stop, or refuses the
                 * certificate. Not an error worth a log line. */
                if (ct_transport_peer_closed(s->transport)) {
                    br_ssl_engine_close(&s->sc.eng);
                    s->state = kMacTLS_Closed;
                    return s->state;
                }
                s->state = kMacTLS_Error;
                s->error = kMacTLS_ErrWrite;
                return s->state;
            }
        }
    }

    if (st & BR_SSL_RECVREC) {
        buf = br_ssl_engine_recvrec_buf(&s->sc.eng, &len);
        if (len > 0) {
            n = ct_transport_recv(s->transport, buf, len);
            if (n > 0) {
                s->rxTotal += (size_t)n;
                /*
                 * Always-on, once: the opening bytes name the protocol the
                 * client is really speaking, which is the whole diagnosis when
                 * a handshake then goes nowhere. Capped at 24 bytes so it is
                 * one line, and only on the very first read so it is the hello.
                 */
                if (!s->helloLogged) {
                    char hex[3 * 24 + 1];
                    size_t k, dump, p = 0;
                    s->helloLogged = 1;
                    dump = (size_t)n > 24 ? 24 : (size_t)n;
                    for (k = 0; k < dump; k++) {
                        int hn = snprintf(hex + p, sizeof(hex) - p, "%s%02x",
                                          k ? " " : "", buf[k]);
                        if (hn < 0) break;
                        p += (size_t)hn;
                    }
                    gw_log("MITM client hello: %d bytes, first: %s",
                           n, hex);
                }
#ifdef GW_DEBUG_IO
                if (s->logged_raw < 10 && n >= 2) {
                    size_t dump = (size_t)n > 96 ? 96 : (size_t)n;
                    size_t off = 0;
                    s->logged_raw++;
                    while (off < dump) {
                        char raw[193];
                        size_t chunk = dump - off > 32 ? 32 : dump - off;
                        size_t k, p = 0;
                        for (k = 0; k < chunk && p + 3 < sizeof(raw); k++) {
                            int nn = snprintf(raw + p, sizeof(raw) - p, "%s%02x", k ? " " : "", buf[off + k]);
                            if (nn < 0) break;
                            p += (size_t)nn;
                        }
                        gw_log("  RECV raw %d bytes [%u/%u]: %s", n, (unsigned)off, (unsigned)dump, raw);
                        off += chunk;
                    }
                }
#endif
                br_ssl_engine_recvrec_ack(&s->sc.eng, (size_t)n);
            } else if (n < 0) {
                if (ct_transport_peer_closed(s->transport)) {
                    /*
                     * The peer hung up. Ordinary at the end of a session, but
                     * a browser that does it mid-handshake lands here too --
                     * and this is the path it actually takes, ahead of the
                     * BR_SSL_CLOSED branch below, so the description has to
                     * be here as well to be of any use. What it says is how
                     * far the handshake had got: a chosen suite of 0000/0000
                     * means it left before we answered its hello, anything
                     * else means it left after seeing our certificate.
                     */
                    if (!s->handshook) {
                        char hello[384];
                        describe_hello(s, hello, sizeof(hello));
                        gw_log("MITM handshake abandoned by the browser: %s", hello);
                    }
                    br_ssl_engine_close(&s->sc.eng);
                    s->state = kMacTLS_Closed;
                    return s->state;
                }
                s->state = kMacTLS_Error;
                s->error = kMacTLS_ErrRead;
                return s->state;
            }
        }
    }

    st = br_ssl_engine_current_state(&s->sc.eng);
#ifdef GW_DEBUG_IO
    if (!s->logged_raw) {
        if (s->sc.eng.hbuf_in && s->sc.eng.hlen_in >= 4) {
            char raw[385];
            size_t dump = s->sc.eng.hlen_in > 96 ? 96 : s->sc.eng.hlen_in;
            size_t k, p = 0;
            s->logged_raw = 1;
            for (k = 0; k < dump && p + 3 < sizeof(raw); k++) {
                int n = snprintf(raw + p, sizeof(raw) - p, "%s%02x", k ? " " : "", s->sc.eng.hbuf_in[k]);
                if (n < 0) break;
                p += (size_t)n;
            }
            gw_log("  ClientHello hbuf %u bytes: %s", (unsigned)s->sc.eng.hlen_in, raw);
        } else if (s->sc.eng.ibuf && s->sc.eng.ixa != s->sc.eng.ixb) {
            char raw[385];
            size_t avail = s->sc.eng.ixb - s->sc.eng.ixa;
            const unsigned char *pbuf = s->sc.eng.ibuf + s->sc.eng.ixa;
            size_t dump = avail > 96 ? 96 : avail;
            size_t k, p = 0;
            s->logged_raw = 1;
            if (avail > 5 && pbuf[0] == 0x16) { pbuf += 5; dump = dump > 5 ? dump -5 : 0; }
            for (k = 0; k < dump && p + 3 < sizeof(raw); k++) {
                int n = snprintf(raw + p, sizeof(raw) - p, "%s%02x", k ? " " : "", pbuf[k]);
                if (n < 0) break;
                p += (size_t)n;
            }
            gw_log("  ClientHello ibuf %u bytes: %s", (unsigned)avail, raw);
        } else if (s->sc.client_max_version != 0 || s->sc.client_suites_num != 0) {
            s->logged_raw = 1;
            gw_log("  ClientHello no buf: version %04x suites %u hlen %u ixa %u ixb %u st %x",
                s->sc.client_max_version, s->sc.client_suites_num, (unsigned)s->sc.eng.hlen_in, (unsigned)s->sc.eng.ixa, (unsigned)s->sc.eng.ixb, st);
        }
    }
#endif
    if (st & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) {
        if (!s->handshook) {
            char hello[384];
            s->handshook = 1;
            describe_hello(s, hello, sizeof(hello));
            gw_log("MITM handshake done: %s", hello);
        }
        s->state = kMacTLS_Connected;
    }
    else if (st & (BR_SSL_SENDREC | BR_SSL_RECVREC)) {
        /*
         * Record-level I/O alone means "still handshaking" only before the
         * handshake has finished -- see PATCHES.md §26, which is the same
         * fault on the client side. One buffer serves both directions, so a
         * connected session pushing out a record offers neither SENDAPP nor
         * RECVAPP, and that is the ordinary state of one that has just been
         * written to.
         */
        if (s->state != kMacTLS_Connected)
            s->state = kMacTLS_Handshaking;
    }

    return s->state;
}

int MacTLS_ServerRead(MacTLS_Server *s, void *out, size_t cap)
{
    unsigned char *buf;
    size_t         len;

    if (s == NULL || out == NULL || cap == 0) return -1;
    if (s->state == kMacTLS_Error) return -1;

    if (!(br_ssl_engine_current_state(&s->sc.eng) & BR_SSL_RECVAPP))
        return (s->state == kMacTLS_Closed) ? -1 : 0;

    buf = br_ssl_engine_recvapp_buf(&s->sc.eng, &len);
    if (len == 0) return 0;
    if (len > cap) len = cap;

    memcpy(out, buf, len);
    br_ssl_engine_recvapp_ack(&s->sc.eng, len);
    return (int)len;
}

int MacTLS_ServerWrite(MacTLS_Server *s, const void *data, size_t len)
{
    unsigned char *buf;
    size_t         room;

    if (s == NULL || data == NULL) return -1;
    if (s->state == kMacTLS_Error) return -1;
    if (len == 0) return 0;

    if (!(br_ssl_engine_current_state(&s->sc.eng) & BR_SSL_SENDAPP))
        return (s->state == kMacTLS_Closed) ? -1 : 0;

    buf = br_ssl_engine_sendapp_buf(&s->sc.eng, &room);
    if (room == 0) return 0;
    if (len > room) len = room;

    memcpy(buf, data, len);
    br_ssl_engine_sendapp_ack(&s->sc.eng, len);
    /*
     * Flushed rather than left to fill. A proxy writes a response head and
     * then waits on the origin; without this the head would sit in the buffer
     * until enough body arrived to fill a record, and a browser waiting for
     * headers it has not been sent looks exactly like a hung connection.
     */
    br_ssl_engine_flush(&s->sc.eng, 0);
    return (int)len;
}

int MacTLS_ServerPendingOut(const MacTLS_Server *s)
{
    unsigned st;

    if (s == NULL) return 0;
    if (s->state != kMacTLS_Connected && s->state != kMacTLS_Handshaking)
        return 0;
    st = br_ssl_engine_current_state((br_ssl_engine_context *)&s->sc.eng);
    return (st & BR_SSL_SENDREC) != 0;
}

MacTLS_State MacTLS_ServerState(const MacTLS_Server *s)
{
    return (s == NULL) ? kMacTLS_Error : s->state;
}

int MacTLS_ServerLastError(const MacTLS_Server *s)
{
    if (s == NULL) return -1;
    /*
     * const off: br_ssl_engine_last_error takes a mutable pointer although it
     * reads one field. Casting here keeps the const on this function, which is
     * the honest signature -- asking what went wrong changes nothing.
     */
    return br_ssl_engine_last_error((br_ssl_engine_context *)&s->sc.eng);
}

unsigned int MacTLS_ServerClientVersion(const MacTLS_Server *s)
{
    if (s == NULL) return 0;
    return s->sc.client_max_version;
}

void MacTLS_ServerClose(MacTLS_Server *s)
{
    if (s == NULL) return;

    if (s->state == kMacTLS_Connected || s->state == kMacTLS_Handshaking)
        br_ssl_engine_close(&s->sc.eng);

    if (s->transport != NULL) {
        ct_transport_close(s->transport);
        ct_transport_destroy(s->transport);
        s->transport = NULL;
    }
    DisposePtr((Ptr)s);
}
