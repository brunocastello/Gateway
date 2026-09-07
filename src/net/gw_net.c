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

CTSocket GWConn_DetachSocket(GWConn *c)
{
    EndpointRef ep;

    if (c == NULL || c->ep == NULL) return CT_SOCKET_NONE;

    ep = c->ep;
    OTRemoveNotifier(ep);       /* our flags must stop being written to */
    c->ep = NULL;
    c->state = kGWConnClosed;
    return (CTSocket)ep;
}

/* ------------------------------------------------------------------ */
/* Accessors: what the shared stream layer is allowed to know          */
/* ------------------------------------------------------------------ */

GWConnState GWConn_GetState(const GWConn *c)
{
    return (c == NULL) ? kGWConnError : c->state;
}

int GWConn_PeerClosed(const GWConn *c)
{
    return (c == NULL) ? 1 : (c->remoteEOF ? 1 : 0);
}

long GWConn_LastError(const GWConn *c)
{
    return (c == NULL) ? 0 : (long)c->err;
}

UInt32 GWConn_PeerIPv4(const GWConn *c)
{
    return (c == NULL) ? 0 : (UInt32)c->remote.fHost;
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

    /*
     * Take the notifiers off before closing anything, and before the memory
     * they point at goes back to the heap.
     *
     * The notifier's context is this struct. A connection being destroyed
     * while a lookup or a connect is still outstanding -- which is what a
     * browser's Stop button produces, several at once -- can still have
     * T_DNRSTRINGTOADDRCOMPLETE or T_DISCONNECT delivered to it. Writing
     * those flags into freed memory corrupts the Memory Manager's free list,
     * and the crash then lands somewhere else entirely, long afterwards.
     */
    if (c->svc != NULL) {
        OTRemoveNotifier(c->svc);
        OTCloseProvider(c->svc);
    }
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

GWListener *GWListener_Open(UInt16 port, int backlog)
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
    bindReq.qlen        = (OTQLen)backlog;
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

GWConn *GWListener_Poll(GWListener *l, int accepting)
{
    OSStatus err;

    if (l == NULL || !l->open) return NULL;

    /*
     * An accept already in flight is pumped whether or not the caller has room
     * for it. Skipping that was a mistake: a connection part-way through being
     * accepted when the proxy filled up would sit there unpumped, so it never
     * completed and never reported its error either, and the listener could
     * not start the next listen because this one still held l->pending.
     *
     * When there is no room the connection is simply held here, finished but
     * undelivered, until a slot frees.
     */
    if (l->pending != NULL) {
        GWConnState st = GWConn_Pump(l->pending);
        if (st == kGWConnReady) {
            GWConn *c;

            if (!accepting) return NULL;
            c = l->pending;
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

    /* Room to start a new one? If not, leave the indication queued in OT. */
    if (!accepting) return NULL;

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
            /*
             * The client hung up between the T_LISTEN and the accept -- a
             * browser cancelling a page does this to every connection it had
             * queued. Acknowledge the disconnect and say nothing: it is
             * ordinary traffic, and logging it buried the real lines.
             */
            OTResult look = OTLook(l->ep);
            if (look == T_DISCONNECT) {
                OTRcvDisconnect(l->ep, NULL);
                GWConn_Destroy(l->pending);
                l->pending = NULL;
                return NULL;
            }
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
