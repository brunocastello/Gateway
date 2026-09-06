/*
 * gw_net.c - Open Transport plumbing for Gateway.
 *
 * Everything here is non-blocking. OT notifiers run at interrupt time, so they
 * only ever set flags on the owning struct; the pump functions, which run at
 * application time from the single WaitNextEvent loop, do the real work
 * (CLAUDE.md rule 6).
 */

#include "gw_net.h"

#include <Events.h>
#include <Memory.h>
#include <string.h>
#include <stdio.h>

#include "../portable/gw_log.h"

/* OpenTransport.h spells this kETIMEDOUTErr, but the constant moved around
 * between Universal Interfaces revisions; the numeric value did not. */
#define GW_ETIMEDOUT (-3259)

static Boolean sOTReady = false;

unsigned long GWNet_Ticks(void)
{
    return (unsigned long)TickCount();
}

OSStatus GWNet_Init(void)
{
    OSStatus err;

    if (sOTReady) return noErr;
    err = InitOpenTransport();
    if (err == noErr) sOTReady = true;
    return err;
}

void GWNet_Shutdown(void)
{
    if (!sOTReady) return;
    CloseOpenTransport();
    sOTReady = false;
}

/* ------------------------------------------------------------------ */
/* Connection                                                          */
/* ------------------------------------------------------------------ */

static pascal void gw_conn_notifier(void *context, OTEventCode event,
                                    OTResult result, void *cookie)
{
    GWConn *c = (GWConn *)context;

    (void)cookie;

    switch (event) {
    case T_CONNECT:
        c->fConnect = true;
        OTRcvConnect(c->ep, NULL);
        break;

    case T_PASSCON:
        c->fPassCon = true;
        break;

    case T_DATA:
    case T_EXDATA:
        c->fData = true;
        break;

    case T_ORDREL:
        c->fOrdRel = true;
        break;

    case T_DISCONNECT:
        c->fDisconnect = true;
        c->err = result;
        break;

    case T_DNRSTRINGTOADDRCOMPLETE:
        c->fDns = true;
        c->err = result;
        break;

    default:
        break;
    }
}

/* Open a TCP endpoint, bind it to an ephemeral local port, then switch it to
 * asynchronous non-blocking mode with our notifier attached. */
static OSStatus gw_conn_open_endpoint(GWConn *c)
{
    OTConfigurationRef cfg;
    OSStatus           err;
    TBind              bindReq;
    InetAddress        local;

    cfg = OTCreateConfiguration("tcp");
    if (cfg == NULL) return kOTOutOfMemoryErr;

    c->ep = OTOpenEndpoint(cfg, 0, NULL, &err);
    if (err != noErr) return err;

    OTInitInetAddress(&local, 0, kOTAnyInetAddress);
    OTMemzero(&bindReq, sizeof(bindReq));
    bindReq.addr.maxlen = sizeof(local);
    bindReq.addr.len    = sizeof(local);
    bindReq.addr.buf    = (unsigned char *)&local;
    bindReq.qlen        = 0;

    err = OTBind(c->ep, &bindReq, NULL);      /* still synchronous: completes */
    if (err != noErr) return err;

    err = OTInstallNotifier(c->ep, gw_conn_notifier, c);
    if (err != noErr) return err;

    err = OTSetAsynchronous(c->ep);
    if (err != noErr) return err;

    return OTSetNonBlocking(c->ep);
}

static GWConn *gw_conn_alloc(void)
{
    GWConn *c = (GWConn *)NewPtrClear(sizeof(GWConn));
    if (c == NULL) return NULL;
    c->state = kGWConnIdle;
    c->startTicks = GWNet_Ticks();
    return c;
}

GWConn *GWConn_Connect(const char *host, UInt16 port)
{
    GWConn  *c = gw_conn_alloc();
    OSStatus err;

    if (c == NULL) return NULL;

    c->port = port;
    strncpy(c->host, host, GW_NET_HOST_MAX - 1);

    err = gw_conn_open_endpoint(c);
    if (err != noErr) {
        c->err = err;
        c->state = kGWConnError;
        return c;
    }

    c->svc = OTOpenInternetServices(kDefaultInternetServicesPath, 0, &err);
    if (err != noErr) {
        c->err = err;
        c->state = kGWConnError;
        return c;
    }
    OTInstallNotifier(c->svc, gw_conn_notifier, c);
    OTSetAsynchronous(c->svc);

    err = OTInetStringToAddress(c->svc, c->host, &c->hinfo);
    if (err != noErr) {
        c->err = err;
        c->state = kGWConnError;
        return c;
    }

    c->state = kGWConnResolving;
    return c;
}

/* Open and bind an endpoint that OTAccept() can transfer a connection onto. */
static GWConn *gw_conn_alloc_for_accept(void)
{
    GWConn  *c = gw_conn_alloc();
    OSStatus err;

    if (c == NULL) return NULL;

    err = gw_conn_open_endpoint(c);
    if (err != noErr) {
        GWConn_Destroy(c);
        return NULL;
    }
    c->state = kGWConnAccepting;
    return c;
}

GWConnState GWConn_Pump(GWConn *c)
{
    if (c == NULL) return kGWConnError;

    switch (c->state) {
    case kGWConnResolving:
        if (GWNet_Ticks() - c->startTicks > GW_CONNECT_TIMEOUT) {
            c->err = GW_ETIMEDOUT;
            c->state = kGWConnError;
            break;
        }
        if (c->fDisconnect) { c->state = kGWConnError; break; }
        if (c->fDns) {
            TCall sndCall;

            if (c->err != noErr) { c->state = kGWConnError; break; }

            OTInitInetAddress(&c->remote, c->port, c->hinfo.addrs[0]);
            OTMemzero(&sndCall, sizeof(sndCall));
            sndCall.addr.maxlen = sizeof(c->remote);
            sndCall.addr.len    = sizeof(c->remote);
            sndCall.addr.buf    = (unsigned char *)&c->remote;

            c->err = OTConnect(c->ep, &sndCall, NULL);
            if (c->err != noErr && c->err != kOTNoDataErr) {
                c->state = kGWConnError;
                break;
            }
            c->err = noErr;
            c->state = kGWConnConnecting;
        }
        break;

    case kGWConnConnecting:
        if (GWNet_Ticks() - c->startTicks > GW_CONNECT_TIMEOUT) {
            c->err = GW_ETIMEDOUT;
            c->state = kGWConnError;
            break;
        }
        if (c->fDisconnect) { c->state = kGWConnError; break; }
        if (c->fConnect) c->state = kGWConnReady;
        break;

    case kGWConnAccepting:
        if (GWNet_Ticks() - c->startTicks > GW_CONNECT_TIMEOUT) {
            c->err = GW_ETIMEDOUT;
            c->state = kGWConnError;
            break;
        }
        if (c->fDisconnect) { c->state = kGWConnError; break; }
        if (c->fPassCon) c->state = kGWConnReady;
        break;

    case kGWConnReady:
        if (c->fDisconnect) { c->state = kGWConnError; break; }
        /*
         * A received FIN must be acknowledged with OTRcvOrderlyDisconnect or
         * OT fails every later call with kOTLookErr. Acknowledging it does not
         * close our send direction, so a half-open splice keeps working.
         */
        if (c->fOrdRel && !c->ordRelConsumed) {
            OTRcvOrderlyDisconnect(c->ep);
            c->ordRelConsumed = true;
            c->remoteEOF = true;
        }
        break;

    case kGWConnClosing:
        if (c->fDisconnect || c->fOrdRel) {
            if (c->fOrdRel && !c->ordRelConsumed) {
                OTRcvOrderlyDisconnect(c->ep);
                c->ordRelConsumed = true;
            }
            c->state = kGWConnClosed;
        }
        break;

    default:
        break;
    }
    return c->state;
}

long GWConn_Send(GWConn *c, const void *buf, size_t len)
{
    OTResult r;

    if (c == NULL) return -1;
    if (c->state != kGWConnReady && c->state != kGWConnClosing) return -1;
    if (len == 0) return 0;

    r = OTSnd(c->ep, (void *)buf, (OTByteCount)len, 0);
    if (r == kOTFlowErr) return 0;              /* try again next slice */
    if (r == kOTLookErr) {
        OTResult look = OTLook(c->ep);
        if (look == T_ORDREL) {
            OTRcvOrderlyDisconnect(c->ep);
            c->ordRelConsumed = true;
            c->remoteEOF = true;
            return 0;
        }
        if (look == T_DISCONNECT) {
            OTRcvDisconnect(c->ep, NULL);
            c->fDisconnect = true;
            c->state = kGWConnError;
        }
        return -1;
    }
    if (r < 0) {
        c->err = r;
        return -1;
    }
    return (long)r;
}

long GWConn_Recv(GWConn *c, void *buf, size_t len)
{
    OTResult r;
    OTFlags  flags = 0;

    if (c == NULL) return -1;
    if (c->remoteEOF) return -2;
    if (c->state != kGWConnReady && c->state != kGWConnClosing) return -1;
    if (len == 0) return 0;

    r = OTRcv(c->ep, buf, (OTByteCount)len, &flags);

    if (r == kOTNoDataErr) {
        c->fData = false;
        if (c->fOrdRel) {
            if (!c->ordRelConsumed) {
                OTRcvOrderlyDisconnect(c->ep);
                c->ordRelConsumed = true;
            }
            c->remoteEOF = true;
            return -2;
        }
        return 0;
    }
    if (r == kOTLookErr) {
        OTResult look = OTLook(c->ep);
        if (look == T_ORDREL) {
            OTRcvOrderlyDisconnect(c->ep);
            c->ordRelConsumed = true;
            c->remoteEOF = true;
            return -2;
        }
        if (look == T_DISCONNECT) {
            OTRcvDisconnect(c->ep, NULL);
            c->fDisconnect = true;
            c->state = kGWConnError;
        }
        return -1;
    }
    if (r < 0) {
        c->err = r;
        return -1;
    }
    return (long)r;
}

EndpointRef GWConn_DetachEndpoint(GWConn *c)
{
    EndpointRef ep;

    if (c == NULL || c->ep == NULL) return NULL;

    ep = c->ep;
    OTRemoveNotifier(ep);       /* our flags must stop being written to */
    c->ep = NULL;
    c->state = kGWConnClosed;
    return ep;
}

void GWConn_Close(GWConn *c)
{
    if (c == NULL) return;
    if (c->state == kGWConnReady && !c->sentFIN) {
        OTSndOrderlyDisconnect(c->ep);
        c->sentFIN = true;
        c->state = kGWConnClosing;
    }
}

void GWConn_Destroy(GWConn *c)
{
    if (c == NULL) return;
    if (c->svc != NULL) OTCloseProvider(c->svc);
    if (c->ep != NULL) {
        OTRemoveNotifier(c->ep);
        OTCloseProvider(c->ep);
    }
    DisposePtr((Ptr)c);
}

void GWConn_PeerText(GWConn *c, char *out, size_t cap)
{
    InetHost h;

    if (cap == 0) return;
    out[0] = '\0';
    if (c == NULL) return;

    h = c->remote.fHost;
    snprintf(out, cap, "%lu.%lu.%lu.%lu",
             (unsigned long)((h >> 24) & 0xFF),
             (unsigned long)((h >> 16) & 0xFF),
             (unsigned long)((h >> 8) & 0xFF),
             (unsigned long)(h & 0xFF));
}

/* ------------------------------------------------------------------ */
/* Listener                                                            */
/* ------------------------------------------------------------------ */

static pascal void gw_listener_notifier(void *context, OTEventCode event,
                                        OTResult result, void *cookie)
{
    GWListener *l = (GWListener *)context;

    (void)cookie;

    switch (event) {
    case T_LISTEN:
        l->fListen = true;
        break;
    case T_ACCEPTCOMPLETE:
        l->fAcceptDone = true;
        l->err = result;
        break;
    case T_DISCONNECT:
        l->err = result;
        break;
    default:
        break;
    }
}

GWListener *GWListener_Open(UInt16 port, OTQLen qlen)
{
    GWListener        *l;
    OTConfigurationRef cfg;
    OSStatus           err;
    TBind              bindReq, bindRet;
    InetAddress        local, boundAddr;

    l = (GWListener *)NewPtrClear(sizeof(GWListener));
    if (l == NULL) return NULL;
    l->port = port;

    /*
     * "tilisten" is the OT module that serialises incoming connection
     * indications, so exactly one T_LISTEN is outstanding at a time. Without
     * it a server has to track several pending TCalls by sequence number,
     * which is more state than a cooperative single-threaded proxy wants.
     */
    cfg = OTCreateConfiguration("tilisten,tcp");
    if (cfg == NULL) {
        DisposePtr((Ptr)l);
        return NULL;
    }

    l->ep = OTOpenEndpoint(cfg, 0, NULL, &err);
    if (err != noErr) {
        gw_log("listen %u: OTOpenEndpoint %d", (unsigned)port, (int)err);
        DisposePtr((Ptr)l);
        return NULL;
    }

    /* kOTAnyInetAddress is OT's INADDR_ANY: loopback and the control-panel
     * address both land here (CLAUDE.md rule 7). */
    OTInitInetAddress(&local, port, kOTAnyInetAddress);
    OTMemzero(&bindReq, sizeof(bindReq));
    OTMemzero(&bindRet, sizeof(bindRet));
    bindReq.addr.maxlen = sizeof(local);
    bindReq.addr.len    = sizeof(local);
    bindReq.addr.buf    = (unsigned char *)&local;
    bindReq.qlen        = qlen;
    bindRet.addr.maxlen = sizeof(boundAddr);
    bindRet.addr.buf    = (unsigned char *)&boundAddr;

    err = OTBind(l->ep, &bindReq, &bindRet);
    if (err != noErr) {
        gw_log("listen %u: OTBind %d", (unsigned)port, (int)err);
        OTCloseProvider(l->ep);
        DisposePtr((Ptr)l);
        return NULL;
    }

    err = OTInstallNotifier(l->ep, gw_listener_notifier, l);
    if (err == noErr) err = OTSetAsynchronous(l->ep);
    if (err == noErr) err = OTSetNonBlocking(l->ep);
    if (err != noErr) {
        gw_log("listen %u: async setup %d", (unsigned)port, (int)err);
        OTRemoveNotifier(l->ep);
        OTCloseProvider(l->ep);
        DisposePtr((Ptr)l);
        return NULL;
    }

    l->open = true;
    gw_log("listening on port %u", (unsigned)port);
    return l;
}

GWConn *GWListener_Poll(GWListener *l)
{
    OSStatus err;

    if (l == NULL || !l->open) return NULL;

    /* An accept is already in flight: let it finish before listening again. */
    if (l->pending != NULL) {
        GWConnState st = GWConn_Pump(l->pending);
        if (st == kGWConnReady) {
            GWConn *c = l->pending;
            l->pending = NULL;
            l->fAcceptDone = false;
            return c;
        }
        if (st == kGWConnError || st == kGWConnClosed) {
            gw_log("port %u: accept failed %d", (unsigned)l->port,
                   (int)l->pending->err);
            GWConn_Destroy(l->pending);
            l->pending = NULL;
            l->fAcceptDone = false;
        }
        return NULL;
    }

    if (!l->fListen) return NULL;
    l->fListen = false;

    OTMemzero(&l->call, sizeof(l->call));
    l->call.addr.maxlen = sizeof(l->callAddr);
    l->call.addr.buf    = (unsigned char *)&l->callAddr;
    l->call.opt.maxlen  = 0;
    l->call.udata.maxlen = 0;

    err = OTListen(l->ep, &l->call);
    if (err == kOTNoDataErr) return NULL;
    if (err != noErr) {
        gw_log("port %u: OTListen %d", (unsigned)l->port, (int)err);
        return NULL;
    }

    l->pending = gw_conn_alloc_for_accept();
    if (l->pending == NULL) {
        /* No room for another endpoint: refuse this one rather than hang. */
        OTSndDisconnect(l->ep, &l->call);
        gw_log("port %u: out of memory, refused a connection", (unsigned)l->port);
        return NULL;
    }
    l->pending->remote = l->callAddr;

    err = OTAccept(l->ep, l->pending->ep, &l->call);
    if (err != noErr && err != kOTNoDataErr) {
        if (err == kOTLookErr) {
            OTResult look = OTLook(l->ep);
            if (look == T_DISCONNECT) OTRcvDisconnect(l->ep, NULL);
        }
        gw_log("port %u: OTAccept %d", (unsigned)l->port, (int)err);
        GWConn_Destroy(l->pending);
        l->pending = NULL;
        return NULL;
    }
    return NULL;                                /* wait for T_PASSCON */
}

void GWListener_Close(GWListener *l)
{
    if (l == NULL) return;
    if (l->pending != NULL) GWConn_Destroy(l->pending);
    if (l->ep != NULL) {
        OTRemoveNotifier(l->ep);
        OTCloseProvider(l->ep);
    }
    DisposePtr((Ptr)l);
}

/* ------------------------------------------------------------------ */
/* Stream                                                              */
/* ------------------------------------------------------------------ */

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
    s->state = (c != NULL && c->state == kGWConnReady)
                   ? kGWStreamReady : kGWStreamError;
}

int GWStream_UpgradeToTLS(GWStream *s, const char *host)
{
    EndpointRef ep;

    if (s == NULL || s->tls || s->plain == NULL) return 0;
    if (s->plain->state != kGWConnReady || s->plain->remoteEOF) return 0;

    ep = GWConn_DetachEndpoint(s->plain);
    GWConn_Destroy(s->plain);           /* closes the DNS provider, not the ep */
    s->plain = NULL;

    if (ep == NULL) {
        s->state = kGWStreamError;
        return 0;
    }

    /* Certainly owns the endpoint from here, including on failure. */
    s->sec = MacTLS_CreateOnEndpoint(host, ep);
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
    const char *phase = "idle";
    OSStatus    otErr = noErr;
    UInt32      addr = 0;
    int         tlsErr = 0;

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
        otErr = MacTLS_GetOTError(s->sec);
        addr  = (UInt32)MacTLS_GetResolvedAddress(s->sec);
        tlsErr = MacTLS_GetBearSSLError(s->sec);
    } else if (s != NULL && s->plain != NULL) {
        otErr = s->plain->err;
        addr  = s->plain->remote.fHost;
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
        snprintf(out, cap, "%s [%s, OT %d, TLS %d, %lu.%lu.%lu.%lu]",
                 GWStream_ErrorText(s), phase, (int)otErr, tlsErr,
                 (unsigned long)((addr >> 24) & 0xFF),
                 (unsigned long)((addr >> 16) & 0xFF),
                 (unsigned long)((addr >> 8) & 0xFF),
                 (unsigned long)(addr & 0xFF));
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
    if (s->plain != NULL && s->plain->err != noErr) return "TCP error";
    return "ok";
}
