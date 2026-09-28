/*
 * gw_stream.c - one interface over "raw TCP" and "TLS", on any platform.
 *
 * This was the tail of gw_net.c until the transport interface was split out.
 * Nothing in it names an operating system: it drives a GWConn through the
 * accessors in gw_transport.h and a TLS connection through Certainly, so both
 * the Open Transport and the Winsock builds share it rather than each keeping
 * a copy of 280 lines of identical state machine.
 */

#include "gw_transport.h"

#include <certainly.h>

#include <stdio.h>
#include <string.h>


void GWStream_Init(GWStream *s)
{
    memset(s, 0, sizeof(*s));
    s->state = kGWStreamIdle;
}

int GWStream_ConnectPlain(GWStream *s, const char *host, UInt16 port)
{
    GWStream_Init(s);
    s->startTicks = GWNet_Ticks();
    s->plain = GWConn_Connect(host, port);
    if (s->plain == NULL) {
        s->state = kGWStreamError;
        return 0;
    }
    s->state = kGWStreamConnecting;
    return 1;
}

int GWStream_ConnectTLS(GWStream *s, const char *host, UInt16 port)
{
    GWStream_Init(s);
    s->tls = true;
    s->startTicks = GWNet_Ticks();
    s->sec = MacTLS_Create(host, port);
    if (s->sec == NULL || MacTLS_GetState(s->sec) == kMacTLS_Error) {
        s->state = kGWStreamError;
        return 0;
    }
    s->state = kGWStreamConnecting;
    return 1;
}

void GWStream_Adopt(GWStream *s, GWConn *c)
{
    GWStream_Init(s);
    s->plain = c;
    s->state = (c != NULL && GWConn_GetState(c) == kGWConnReady)
                   ? kGWStreamReady : kGWStreamError;
}

int GWStream_UpgradeToTLS(GWStream *s, const char *host)
{
    CTSocket sock;

    if (s == NULL || s->tls || s->plain == NULL) return 0;
    if (GWConn_GetState(s->plain) != kGWConnReady ||
        GWConn_PeerClosed(s->plain)) return 0;

    sock = GWConn_DetachSocket(s->plain);
    GWConn_Destroy(s->plain);           /* closes the DNS provider, not the ep */
    s->plain = NULL;

    if (sock == CT_SOCKET_NONE) {
        s->state = kGWStreamError;
        return 0;
    }

    /* Certainly owns the connection from here, including on failure. */
    s->sec = MacTLS_CreateOnEndpoint(host, sock);
    if (s->sec == NULL || MacTLS_GetState(s->sec) == kMacTLS_Error) {
        s->state = kGWStreamError;
        return 0;
    }

    s->tls = true;
    s->eof = false;
    s->startTicks = GWNet_Ticks();
    s->state = kGWStreamConnecting;
    return 1;
}

/*
 * The same detach, but for a far end with no TLS 1.3: BearSSL's 1.2 engine
 * drives from the first pump and no fallback reconnect is needed, which is
 * what makes 1.2-only servers reachable through a proxy tunnel or STARTTLS.
 */
int GWStream_UpgradeToTLS12(GWStream *s, const char *host)
{
    CTSocket sock;

    if (s == NULL || s->tls || s->plain == NULL) return 0;
    if (GWConn_GetState(s->plain) != kGWConnReady ||
        GWConn_PeerClosed(s->plain)) return 0;

    sock = GWConn_DetachSocket(s->plain);
    GWConn_Destroy(s->plain);           /* closes the DNS provider, not the ep */
    s->plain = NULL;

    if (sock == CT_SOCKET_NONE) {
        s->state = kGWStreamError;
        return 0;
    }

    /* Certainly owns the connection from here, including on failure. */
    s->sec = MacTLS_CreateOnEndpointTLS12(host, sock);
    if (s->sec == NULL || MacTLS_GetState(s->sec) == kMacTLS_Error) {
        s->state = kGWStreamError;
        return 0;
    }

    s->tls = true;
    s->eof = false;
    s->startTicks = GWNet_Ticks();
    s->state = kGWStreamConnecting;
    return 1;
}

/*
 * The same detach, with Gateway answering the handshake instead of starting
 * it. The browser has just been told "200 Connection Established" and is about
 * to send a ClientHello; from here the socket belongs to Certainly's server
 * side, and every GWStream operation on it goes through MacTLS_Server.
 */
int GWStream_UpgradeToTLSServer(GWStream *s,
                                const unsigned char *leaf, size_t leaf_len,
                                const unsigned char *ca, size_t ca_len,
                                const void *key)
{
    CTSocket sock;

    if (s == NULL || s->tls || s->plain == NULL) return 0;
    if (GWConn_GetState(s->plain) != kGWConnReady ||
        GWConn_PeerClosed(s->plain)) return 0;

    sock = GWConn_DetachSocket(s->plain);
    GWConn_Destroy(s->plain);
    s->plain = NULL;

    if (sock == CT_SOCKET_NONE) {
        s->state = kGWStreamError;
        return 0;
    }

    /* Certainly owns the connection from here, including on failure. */
    s->srv = MacTLS_ServerCreate(sock, leaf, leaf_len, ca, ca_len, key);
    if (s->srv == NULL || MacTLS_ServerState(s->srv) == kMacTLS_Error) {
        s->state = kGWStreamError;
        return 0;
    }

    s->tls = true;
    s->eof = false;
    s->startTicks = GWNet_Ticks();
    s->state = kGWStreamConnecting;
    return 1;
}

GWStreamState GWStream_Pump(GWStream *s)
{
    if (s == NULL) return kGWStreamError;

    if (s->tls) {
        MacTLS_State st;

        if (s->srv != NULL) {
            st = MacTLS_ServerPump(s->srv);
        } else if (s->sec != NULL) {
            st = MacTLS_Pump(s->sec);
        } else {
            return s->state = kGWStreamError;
        }
        switch (st) {
        case kMacTLS_Connected: s->state = kGWStreamReady;   break;
        case kMacTLS_Closed:    s->state = kGWStreamClosed;  break;
        case kMacTLS_Error:     s->state = kGWStreamError;   break;
        default:
            if (s->state != kGWStreamReady) s->state = kGWStreamConnecting;
            break;
        }
        return s->state;
    }

    if (s->plain == NULL) return s->state = kGWStreamError;

    switch (GWConn_Pump(s->plain)) {
    case kGWConnReady:   s->state = kGWStreamReady;   break;
    case kGWConnClosed:  s->state = kGWStreamClosed;  break;
    case kGWConnError:   s->state = kGWStreamError;   break;
    case kGWConnClosing: /* half-closed but still writable */              break;
    default:             s->state = kGWStreamConnecting;                   break;
    }
    return s->state;
}

long GWStream_Write(GWStream *s, const void *buf, size_t len)
{
    if (s == NULL) return -1;
    if (s->tls) {
        int n;
        if (s->srv != NULL) {
            n = MacTLS_ServerWrite(s->srv, buf, len);
        } else if (s->sec != NULL) {
            n = MacTLS_Write(s->sec, buf, len);
        } else {
            return -1;
        }
        return (n < 0) ? -1 : (long)n;
    }
    return GWConn_Send(s->plain, buf, len);
}

long GWStream_Read(GWStream *s, void *buf, size_t len)
{
    if (s == NULL) return -1;

    if (s->tls && s->srv != NULL) {
        int n = MacTLS_ServerRead(s->srv, buf, len);

        if (n > 0) return n;
        if (n < 0) return -1;
        if (MacTLS_ServerState(s->srv) == kMacTLS_Closed) {
            s->eof = true;
            return -2;
        }
        return 0;
    }

    if (s->tls) {
        int n;
        if (s->sec == NULL) return -1;
        n = MacTLS_Read(s->sec, buf, len);
        if (n > 0) return n;
        if (n < 0) return -1;
        /* Nothing buffered. A closed session with an empty buffer is EOF. */
        if (MacTLS_GetState(s->sec) == kMacTLS_Closed ||
            MacTLS_GetState(s->sec) == kMacTLS_Closing) {
            s->eof = true;
            return -2;
        }
        return 0;
    }

    {
        long n = GWConn_Recv(s->plain, buf, len);
        if (n == -2) s->eof = true;
        return n;
    }
}

void GWStream_Close(GWStream *s)
{
    if (s == NULL) return;
    if (s->tls) {
        if (s->srv != NULL) {
            MacTLS_ServerClose(s->srv);
            s->srv = NULL;
        }
        if (s->sec != NULL) {
            MacTLS_Close(s->sec);
            s->sec = NULL;
        }
    } else {
        GWConn_Close(s->plain);
    }
    s->state = kGWStreamClosed;
}

void GWStream_Destroy(GWStream *s)
{
    if (s == NULL) return;
    if (s->srv != NULL) {
        MacTLS_ServerClose(s->srv);
        s->srv = NULL;
    }
    if (s->sec != NULL) {
        MacTLS_Close(s->sec);
        s->sec = NULL;
    }
    if (s->plain != NULL) {
        GWConn_Destroy(s->plain);
        s->plain = NULL;
    }
    s->state = kGWStreamIdle;
}

/*
 * BearSSL's error number from a failed server-side handshake, or 0. The proxy
 * logs it because with a 1997 client the number is the diagnosis: 4 means the
 * browser offered a protocol version older than BearSSL will speak, which for
 * Internet Explorer 4 means SSL 3.0, and 16 means it offered no cipher suite
 * BearSSL has.
 */
int GWStream_ServerError(const GWStream *s)
{
    if (s == NULL || s->srv == NULL) return 0;
    return MacTLS_ServerLastError(s->srv);
}

int GWStream_PeerGone(const GWStream *s)
{
    if (s == NULL) return 1;
    if (s->state == kGWStreamError || s->state == kGWStreamClosed) return 1;
    if (s->eof) return 1;
    if (!s->tls && s->plain != NULL &&
        (GWConn_PeerClosed(s->plain) ||
         GWConn_GetState(s->plain) == kGWConnError))
        return 1;
    return 0;
}

int GWStream_TlsVersion(const GWStream *s)
{
    if (s == NULL || !s->tls || s->sec == NULL) return 0;
    switch (MacTLS_GetVersion(s->sec)) {
    case kMacTLS_Version12: return 12;
    case kMacTLS_Version13: return 13;
    default:                return 0;
    }
}

unsigned int GWStream_ClientHelloVersion(const GWStream *s)
{
    if (s == NULL || !s->tls || s->srv == NULL) return 0;
    return MacTLS_ServerClientVersion(s->srv);
}

int GWStream_ServerStage(const GWStream *s)
{
    if (s == NULL || !s->tls || s->srv == NULL) return kGWStageNothing;
    switch (MacTLS_ServerGetStage(s->srv)) {
    case kMacTLS_StageHello:       return kGWStageHello;
    case kMacTLS_StageCertificate: return kGWStageCertificate;
    case kMacTLS_StageReplied:     return kGWStageReplied;
    case kMacTLS_StageFinished:    return kGWStageFinished;
    case kMacTLS_StageDone:        return kGWStageDone;
    default:                       return kGWStageNothing;
    }
}

void GWStream_ServerDescribe(const GWStream *s, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return;
    out[0] = '\0';
    if (s != NULL && s->tls && s->srv != NULL)
        MacTLS_ServerDescribe(s->srv, out, cap);
}

void GWStream_ServerHelloHex(const GWStream *s, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return;
    out[0] = '\0';
    if (s != NULL && s->tls && s->srv != NULL)
        MacTLS_ServerHelloHex(s->srv, out, cap);
}

int GWStream_ServerResumeTried(const GWStream *s)
{
    if (s == NULL || !s->tls || s->srv == NULL) return 0;
    return MacTLS_ServerResumeTried(s->srv);
}

int GWStream_ServerResumed(const GWStream *s)
{
    if (s == NULL || !s->tls || s->srv == NULL) return 0;
    return MacTLS_ServerResumed(s->srv);
}

unsigned int GWStream_ServerVersion(const GWStream *s)
{
    if (s == NULL || !s->tls || s->srv == NULL) return 0;
    return MacTLS_ServerSessionVersion(s->srv);
}

int GWStream_FallbackNoRoute(const GWStream *s)
{
    if (s == NULL || !s->tls || s->sec == NULL) return 0;
    return MacTLS_FallbackNoRoute(s->sec);
}

/*
 * Testing only (tunnel_insecure): stop validating the far end's certificate.
 * Client side only -- the MITM server side presents certificates rather
 * than checking them. Must run before the handshake, i.e. right after an
 * Upgrade call returns and before the first Pump.
 */
void GWStream_SetInsecure(GWStream *s)
{
    if (s == NULL || !s->tls || s->sec == NULL) return;
    MacTLS_SetInsecure(s->sec);
}

/*
 * Testing/diagnosis (tunnel_sni): replace the SNI name the handshake sends,
 * or omit SNI when sni is NULL. Client side only, before the first Pump --
 * i.e. right after an Upgrade call returns. Certificate validation is
 * unaffected: it always checks the host the stream was upgraded with
 * (PATCHES.md §34).
 */
void GWStream_SetSNI(GWStream *s, const char *sni)
{
    if (s == NULL || !s->tls || s->sec == NULL) return;
    MacTLS_SetSNI(s->sec, sni);
}

/*
 * Turn BearSSL's error number into something readable. Only the codes that
 * actually come up in the field are named; the rest fall through to the raw
 * number, which is still enough to look up in bearssl_ssl.h.
 */
static const char *gw_tls_error_text(int err)
{
    switch (err) {
    case 0:  return NULL;                       /* nothing to report */
    case 62: return "certificate not trusted";  /* BR_ERR_X509_NOT_TRUSTED */
    case 54: return "certificate expired";      /* BR_ERR_X509_EXPIRED */
    case 56: return "certificate is for another host";
    /*
     * BR_ERR_X509_DN_MISMATCH. Seen on macintoshgarden.org from IE 5. The
     * minimal validator walks the chain in the order the server sent it and
     * wants each certificate's issuer DN to equal the next one's subject DN;
     * a browser builds the chain itself and does not care about the order. So
     * this is usually a server sending an out-of-order or redundant chain
     * rather than anything wrong with the certificates in it -- hence naming
     * the chain in the text, so it does not read as "bad certificate".
     */
    case 55: return "certificate chain out of order";
    /* bearssl_x509.h's BR_ERR_X509_* numbers: 52 BAD_SIGNATURE, 35
     * EMPTY_CHAIN, 53 TIME_UNKNOWN, 58 NOT_CA, 60 WEAK_PUBLIC_KEY. */
    case 52: return "bad certificate signature";
    case 35: return "server sent no certificate";
    case 53: return "certificate dates unknown";
    case 58: return "intermediate is not a CA";
    case 60: return "public key too weak";
    default: return NULL;
    }
}

const char *GWStream_Describe(const GWStream *s, char *out, size_t cap)
{
    const char   *phase = "idle";
    OSStatus      otErr = noErr;
    UInt32        addr = 0;
    int           tlsErr = 0;
    unsigned int  alert = 0;
    /*
     * Which of the two handshakes produced the number. They share BR_ERR_*
     * numbering, so "TLS 1" alone could be the 1.3 state machine rejecting a
     * ServerHello field or BearSSL's 1.2 engine refusing to run, and those
     * want opposite fixes. Naming it saves the guess.
     */
    const char   *leg = "";
    /*
     * The encoded diagnostics -- 0x1LLDD for an alert, 0x2000|type for a
     * record that is not one, 0x3000|line for a rejected ServerHello field --
     * are printed in hex so their parts can be read straight off. A plain
     * BR_ERR_* number stays decimal, which is how everyone quotes them.
     */
    char          code[16];

    if (cap == 0) return out;

    if (s != NULL && s->tls && s->sec != NULL) {
        switch (MacTLS_GetPhase(s->sec)) {
        case kMacTLS_PhaseResolving:  phase = "resolving DNS"; break;
        case kMacTLS_PhaseConnecting: phase = "connecting TCP"; break;
        case kMacTLS_PhaseConnected:  phase = "connected";      break;
        case kMacTLS_PhaseClosing:    phase = "closing";        break;
        case kMacTLS_PhaseClosed:     phase = "closed";         break;
        case kMacTLS_PhaseFailed:     phase = "failed";         break;
        default:                      phase = "idle";           break;
        }
        otErr = MacTLS_GetTransportError(s->sec);
        addr  = (UInt32)MacTLS_GetResolvedAddress(s->sec);
        tlsErr = MacTLS_GetBearSSLError(s->sec);
        alert = MacTLS_GetAlert(s->sec);
        if (tlsErr != 0)
            leg = MacTLS_GetTls13Error(s->sec) != 0 ? " 1.3" : " 1.2";
    } else if (s != NULL && s->plain != NULL) {
        otErr = GWConn_LastError(s->plain);
        addr  = GWConn_PeerIPv4(s->plain);
    }

    /*
     * Which version was negotiated, when the handshake got far enough to
     * settle one. A connection that reached TLS 1.2 came through the
     * fallback -- Certainly always opens with 1.3 -- so this says which of
     * the two paths the session was actually on when it failed.
     */
    {
        int ver = GWStream_TlsVersion(s);
        if (ver == 12) leg = " 1.2";
        else if (ver == 13) leg = " 1.3";
    }

    if (tlsErr >= 0x1000) {
        snprintf(code, sizeof(code), "0x%X", (unsigned)tlsErr);
    } else {
        snprintf(code, sizeof(code), "%d", tlsErr);
    }

    {
        const char *tlsText = gw_tls_error_text(tlsErr);
        if (tlsText != NULL) {
            /* A named certificate problem is the whole story; say it plainly
             * rather than making someone look the number up. */
            snprintf(out, cap, "%s: %s [TLS %d]",
                     GWStream_ErrorText(s), tlsText, tlsErr);
            return out;
        }
    }

    if (addr != 0) {
        char why[24];

        /*
         * The peer's own alert, when it sent one. Level and description are
         * two bytes that say what it objected to -- 2/20 is bad_record_mac,
         * 2/51 decrypt_error -- and the library used to discard them, which
         * turned every rejected record into an unexplained disconnection.
         */
        why[0] = '\0';
        if (alert != 0)
            snprintf(why, sizeof(why), ", alert %u/%u",
                     (alert >> 8) & 0xFF, alert & 0xFF);

        snprintf(out, cap, "%s [%s, OT %d, TLS %s%s, %lu.%lu.%lu.%lu%s]",
                 GWStream_ErrorText(s), phase, (int)otErr, code, leg,
                 (unsigned long)((addr >> 24) & 0xFF),
                 (unsigned long)((addr >> 16) & 0xFF),
                 (unsigned long)((addr >> 8) & 0xFF),
                 (unsigned long)(addr & 0xFF), why);
    } else {
        snprintf(out, cap, "%s [%s, OT %d, TLS %s%s, name unresolved]",
                 GWStream_ErrorText(s), phase, (int)otErr, code, leg);
    }
    return out;
}

/*
 * A certificate BearSSL refused, in words. The numbers are the same ones
 * gw_tls_error_text() names for the debug line (BR_ERR_X509_* in
 * bearssl_x509.h); here each gets a sentence of its own and a code, because
 * which one it was is the remedy.
 */
static const char *explain_certificate(int err, const char *host,
                                       char *out, size_t cap)
{
    switch (err) {
    case 62:                                    /* NOT_TRUSTED */
        snprintf(out, cap, "the certificate for %s is not from an authority "
                 "Gateway trusts", host);
        return "T10";
    case 54:                                    /* EXPIRED */
        snprintf(out, cap, "the certificate for %s has expired", host);
        return "T11";
    case 56:                                    /* BAD_SERVER_NAME */
        snprintf(out, cap, "the certificate %s sent is for another name", host);
        return "T12";
    case 55:                                    /* DN_MISMATCH */
        snprintf(out, cap, "%s sent its certificate chain out of order", host);
        return "T13";
    case 52:                                    /* BAD_SIGNATURE */
        snprintf(out, cap, "the certificate for %s has a bad signature", host);
        return "T14";
    case 35:                                    /* EMPTY_CHAIN */
        snprintf(out, cap, "%s sent no certificate", host);
        return "T15";
    case 53:                                    /* TIME_UNKNOWN */
        snprintf(out, cap, "Gateway could not check the dates on the "
                 "certificate for %s: is the clock set?", host);
        return "T16";
    case 58:                                    /* NOT_CA */
        snprintf(out, cap, "the certificate chain for %s has an intermediate "
                 "that is not an authority", host);
        return "T17";
    case 60:                                    /* WEAK_PUBLIC_KEY */
        snprintf(out, cap, "the certificate for %s has a key too weak to "
                 "trust", host);
        return "T18";
    default:
        return NULL;
    }
}

const char *GWStream_Explain(const GWStream *s, const char *host,
                             char *out, size_t cap)
{
    const char *code;

    if (out == NULL || cap == 0) return NULL;
    out[0] = '\0';
    if (s == NULL) return NULL;
    if (host == NULL || host[0] == '\0') host = "the far end";

    if (s->tls && s->sec != NULL) {
        int err = MacTLS_GetBearSSLError(s->sec);

        switch (MacTLS_GetError(s->sec)) {
        case kMacTLS_ErrMemory:
            snprintf(out, cap, "Gateway ran out of memory connecting to %s",
                     host);
            return "T01";
        case kMacTLS_ErrDNS:
            snprintf(out, cap, "the name %s could not be looked up", host);
            return "T02";
        case kMacTLS_ErrConnect:
            snprintf(out, cap, "%s did not accept a connection", host);
            return "T03";
        case kMacTLS_ErrHandshake:
        case kMacTLS_ErrCertificate:
            code = explain_certificate(err, host, out, cap);
            if (code != NULL)
                return code;
            /* PATCHES.md §29: an adopted socket has no route to redial. */
            if (MacTLS_FallbackNoRoute(s->sec)) {
                snprintf(out, cap, "%s speaks only TLS 1.2, which this "
                         "connection cannot fall back to", host);
                return "T20";
            }
            if (MacTLS_GetAlert(s->sec) != 0) {
                snprintf(out, cap, "%s refused the secure connection", host);
                return "T21";
            }
            snprintf(out, cap, "the secure connection to %s failed", host);
            return "T22";
        case kMacTLS_ErrRead:
            snprintf(out, cap, "the secure connection to %s broke while "
                     "reading", host);
            return "T23";
        case kMacTLS_ErrWrite:
            snprintf(out, cap, "the secure connection to %s broke while "
                     "sending", host);
            return "T24";
        case kMacTLS_ErrOT:
            snprintf(out, cap, "the network failed while talking to %s", host);
            return "T04";
        default:                /* OK, or the far end closed: the caller's */
            return NULL;
        }
    }

    if (s->plain != NULL && GWConn_LastError(s->plain) != 0) {
        /* The address is filled in once DNS has answered, on both stacks. */
        if (GWConn_PeerIPv4(s->plain) == 0) {
            snprintf(out, cap, "the name %s could not be looked up", host);
            return "T02";
        }
        snprintf(out, cap, "%s did not accept a connection", host);
        return "T03";
    }
    return NULL;
}

int GWStream_SendPending(const GWStream *s)
{
    if (s == NULL || !s->tls) return 0;
    if (s->srv != NULL) return MacTLS_ServerPendingOut(s->srv);
    return 0;
}

const char *GWStream_ErrorText(const GWStream *s)
{
    if (s == NULL) return "no stream";
    if (s->tls && s->sec != NULL) {
        switch (MacTLS_GetError(s->sec)) {
        case kMacTLS_OK:             return "ok";
        case kMacTLS_ErrMemory:      return "out of memory";
        case kMacTLS_ErrDNS:         return "DNS lookup failed";
        case kMacTLS_ErrConnect:     return "connect failed";
        case kMacTLS_ErrHandshake:   return "TLS handshake failed";
        case kMacTLS_ErrCertificate: return "certificate rejected";
        case kMacTLS_ErrRead:        return "TLS read failed";
        case kMacTLS_ErrWrite:       return "TLS write failed";
        case kMacTLS_ErrClosed:      return "connection closed";
        default:                     return "Open Transport error";
        }
    }
    if (s->plain != NULL && GWConn_LastError(s->plain) != 0) return "TCP error";
    return "ok";
}
