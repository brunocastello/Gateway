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

GWStreamState GWStream_Pump(GWStream *s)
{
    if (s == NULL) return kGWStreamError;

    if (s->tls) {
        MacTLS_State st;

        if (s->sec == NULL) return s->state = kGWStreamError;
        st = MacTLS_Pump(s->sec);
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
        if (s->sec == NULL) return -1;
        n = MacTLS_Write(s->sec, buf, len);
        return (n < 0) ? -1 : (long)n;
    }
    return GWConn_Send(s->plain, buf, len);
}

long GWStream_Read(GWStream *s, void *buf, size_t len)
{
    if (s == NULL) return -1;

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
    case 51: return "bad certificate signature";
    case 34: return "server sent no certificate";
    case 52: return "certificate dates unknown";
    case 57: return "intermediate is not a CA";
    case 59: return "public key too weak";
    default: return NULL;
    }
}

const char *GWStream_Describe(const GWStream *s, char *out, size_t cap)
{
    const char   *phase = "idle";
    OSStatus      otErr = noErr;
    UInt32        addr = 0;
    int           tlsErr = 0;
    unsigned long sent = 0, got = 0;

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
        MacTLS_GetCounters(s->sec, &sent, &got);
    } else if (s != NULL && s->plain != NULL) {
        otErr = GWConn_LastError(s->plain);
        addr  = GWConn_PeerIPv4(s->plain);
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

    /*
     * The byte counters are the point of this line now. "Connected, no error,
     * nothing wrong" describes both a request that never left and a peer that
     * ignored one, and those are looked for in completely different places.
     */
    if (addr != 0) {
        snprintf(out, cap,
                 "%s [%s, OT %d, TLS %d, %lu.%lu.%lu.%lu, wire %lu out %lu in]",
                 GWStream_ErrorText(s), phase, (int)otErr, tlsErr,
                 (unsigned long)((addr >> 24) & 0xFF),
                 (unsigned long)((addr >> 16) & 0xFF),
                 (unsigned long)((addr >> 8) & 0xFF),
                 (unsigned long)(addr & 0xFF), sent, got);
    } else {
        snprintf(out, cap, "%s [%s, OT %d, TLS %d, name unresolved]",
                 GWStream_ErrorText(s), phase, (int)otErr, tlsErr);
    }
    return out;
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
