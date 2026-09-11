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
 * A browser built for export-grade crypto has no 3DES either and cannot be
 * served at all.
 */

#include "certainly.h"
#include "certainly_internal.h"

#include <string.h>

#include <bearssl.h>

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
    if (ca != NULL && ca_len > 0) {
        s->chain[1].data     = (unsigned char *)ca;
        s->chain[1].data_len = ca_len;
        s->chain_len = 2;
    }

    br_ssl_server_init_full_rsa(&s->sc, s->chain, s->chain_len, sk);
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

    if (st == BR_SSL_CLOSED) {
        int err = br_ssl_engine_last_error(&s->sc.eng);

        if (err == BR_ERR_OK) {
            s->state = kMacTLS_Closed;
        } else {
            s->state = kMacTLS_Error;
            s->error = kMacTLS_ErrHandshake;
        }
        return s->state;
    }

    if (st & BR_SSL_SENDREC) {
        buf = br_ssl_engine_sendrec_buf(&s->sc.eng, &len);
        if (len > 0) {
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
                br_ssl_engine_recvrec_ack(&s->sc.eng, (size_t)n);
            } else if (n < 0) {
                if (ct_transport_peer_closed(s->transport)) {
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
    if (st & (BR_SSL_SENDAPP | BR_SSL_RECVAPP))
        s->state = kMacTLS_Connected;
    else if (st & (BR_SSL_SENDREC | BR_SSL_RECVREC))
        s->state = kMacTLS_Handshaking;

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
