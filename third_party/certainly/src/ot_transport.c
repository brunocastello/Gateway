/*
 * ot_transport.c — Open Transport TCP wrapper
 *
 * HOW OT ASYNC WORKS:
 *
 * When we create an endpoint, we register a "notifier" — a callback
 * function that OT calls when events happen. The notifier runs at
 * INTERRUPT TIME, meaning:
 *   - It can't allocate memory (NewPtr)
 *   - It can't call most Toolbox functions
 *   - It can't move memory (no handle dereferencing)
 *   - It CAN set flags and copy small amounts of data
 *
 * So our notifier just sets boolean flags ("hey, data arrived!").
 * Then ot_transport_pump(), which runs at normal application time,
 * checks those flags and does the actual work. This two-phase
 * pattern (notifier sets flags → pump reads flags) is the standard
 * way to do async OT programming.
 */

#include "ot_transport.h"
#include <string.h>
#include <Memory.h>  /* NewPtrClear, DisposePtr */
#include <Events.h>  /* TickCount */

/* Forward declarations */
static pascal void ot_notifier(void *context, OTEventCode event,
                               OTResult result, void *cookie);
static OSStatus    ot_setup_endpoint(OTTransport *t);
static void        ot_start_dns(OTTransport *t);

/*
 * The notifier — runs at interrupt time when OT has something to tell us.
 *
 * OT event codes we care about:
 *   T_CONNECT          — TCP handshake completed successfully
 *   T_DATA             — bytes have arrived and are ready to read
 *   T_ORDREL           — peer sent FIN (orderly shutdown)
 *   T_DISCONNECT       — connection was reset or refused
 *   T_DNRSTRINGTOADDRCOMPLETE — DNS lookup finished
 *
 * We just set flags here. The pump function reads them.
 */
static pascal void ot_notifier(void *context, OTEventCode event,
                               OTResult result, void *cookie)
{
    OTTransport *t = (OTTransport *)context;

    switch (event) {
    case T_OPENCOMPLETE:
        /* Endpoint opened — we handle this synchronously, so ignore */
        break;

    case T_CONNECT:
        t->connectComplete = true;
        /* Must call OTRcvConnect to consume the event */
        OTRcvConnect(t->endpoint, NULL);
        break;

    case T_DATA:
    case T_EXDATA:
        t->dataAvailable = true;
        break;

    case T_ORDREL:
        t->ordRelReceived = true;
        break;

    case T_DISCONNECT:
        t->disconnectReceived = true;
        t->lastError = result;
        break;

    case T_DNRSTRINGTOADDRCOMPLETE:
        t->dnsComplete = true;
        t->lastError = result;
        break;

    default:
        break;
    }
}

/*
 * Consume a pending T_DISCONNECT and record why the peer went away.
 *
 * The notifier's `result` argument is 0 for T_DISCONNECT -- the reason lives
 * in the TDiscon that OTRcvDisconnect() fills in. Calling it is not optional
 * either: until the event is consumed, every later call on the endpoint fails
 * with kOTLookErr (Gateway patch - see PATCHES.md).
 */
static void ot_consume_disconnect(OTTransport *t)
{
    TDiscon discon;

    if (t->endpoint == NULL) return;

    OTMemzero(&discon, sizeof(discon));
    discon.udata.maxlen = 0;
    discon.udata.len    = 0;
    discon.udata.buf    = NULL;

    if (OTRcvDisconnect(t->endpoint, &discon) == noErr && discon.reason != 0)
        t->lastError = discon.reason;
}

/*
 * Issue OTConnect for hostInfo.addrs[t->addrIndex].
 * Returns true when the attempt started.
 */
static Boolean ot_connect_current(OTTransport *t)
{
    OTInitInetAddress(&t->remoteAddr, t->port,
                      t->hostInfo.addrs[t->addrIndex]);
    OTMemzero(&t->sndCall, sizeof(t->sndCall));
    t->sndCall.addr.maxlen = sizeof(t->remoteAddr);
    t->sndCall.addr.len    = sizeof(t->remoteAddr);
    t->sndCall.addr.buf    = (unsigned char *)&t->remoteAddr;

    t->connectComplete = false;
    t->disconnectReceived = false;

    t->lastError = OTConnect(t->endpoint, &t->sndCall, NULL);
    /* kOTNoDataErr means "started, not finished yet" — that is success here. */
    return (t->lastError == noErr || t->lastError == kOTNoDataErr);
}

/*
 * A refused or unreachable address is not the end of the story: a resolver
 * commonly hands back several, and large services rotate through them. Move on
 * to the next one rather than failing the whole connection (Gateway patch -
 * see PATCHES.md).
 */
static Boolean ot_try_next_address(OTTransport *t)
{
    while (t->addrIndex + 1 < kMaxHostAddrs) {
        t->addrIndex++;
        if (t->hostInfo.addrs[t->addrIndex] == 0) return false;
        if (ot_connect_current(t)) return true;
    }
    return false;
}

/*
 * Set up a TCP endpoint.
 *
 * OTOpenEndpointInContext creates an endpoint bound to a "configuration."
 * The string "tcp" tells OT we want a TCP/IP endpoint (as opposed to
 * "udp", "tilisten" for a listening socket, etc.). This is OT's
 * STREAMS heritage showing — you configure protocol stacks as strings.
 */
static OSStatus ot_setup_endpoint(OTTransport *t)
{
    OSStatus        err;
    OTConfigurationRef config;
    TBind           bindReq;
    InetAddress     localAddr;

    config = OTCreateConfiguration("tcp");
    if (config == NULL) return kOTOutOfMemoryErr;

    t->endpoint = OTOpenEndpoint(
        config,
        0,              /* flags — 0 means default (async capable) */
        NULL,           /* endpoint info — we don't need it */
        &err
    );
    if (err != noErr) return err;

    /*
     * Bind first, while the endpoint is still synchronous, so OTBind blocks
     * until it has actually completed.
     *
     * Gateway patch (see PATCHES.md). This used to switch the endpoint to
     * asynchronous mode and then bind, which makes OTBind return immediately
     * and report completion later as T_BINDCOMPLETE -- an event the notifier
     * ignores. Nothing then guaranteed the endpoint was bound by the time
     * OTConnect ran after DNS resolution, and OTConnect on an unbound
     * endpoint fails with kOTOutStateErr. It happened to work whenever the
     * lookup was slower than the bind, which is most of the time and none of
     * the time you want to depend on.
     *
     * All zeros means "any local address, any port" -- bind(INADDR_ANY, 0).
     */
    OTInitInetAddress(&localAddr, 0, kOTAnyInetAddress);
    OTMemzero(&bindReq, sizeof(bindReq));
    bindReq.addr.maxlen = sizeof(localAddr);
    bindReq.addr.len    = sizeof(localAddr);
    bindReq.addr.buf    = (unsigned char *)&localAddr;
    bindReq.qlen        = 0;  /* not a listening socket */

    err = OTBind(t->endpoint, &bindReq, NULL);
    if (err != noErr) return err;

    /* Install our notifier so we get async event callbacks */
    err = OTInstallNotifier(t->endpoint, ot_notifier, t);
    if (err != noErr) return err;

    /* Switch to async mode — all future calls return immediately */
    err = OTSetAsynchronous(t->endpoint);
    if (err != noErr) return err;

    /* Don't block on incomplete operations */
    return OTSetNonBlocking(t->endpoint);
}

/*
 * Start async DNS resolution.
 *
 * OTInetStringToAddress takes a hostname and resolves it to an IP.
 * The result lands in t->hostInfo.addrs[0] when the notifier fires
 * T_DNRSTRINGTOADDRCOMPLETE. This is OT's built-in DNS resolver —
 * it uses whatever DNS servers are configured in the TCP/IP control panel.
 */
static void ot_start_dns(OTTransport *t)
{
    OSStatus   err;

    t->inetSvc = OTOpenInternetServices(
        kDefaultInternetServicesPath,
        0, &err
    );
    if (err != noErr) {
        t->lastError = err;
        t->state = kOTTransport_Error;
        return;
    }

    OTInstallNotifier(t->inetSvc, ot_notifier, t);
    OTSetAsynchronous(t->inetSvc);

    /* t->host, never the caller's buffer: this call is asynchronous and OT
     * reads the name later, when the resolver runs. */
    err = OTInetStringToAddress(t->inetSvc, t->host, &t->hostInfo);
    if (err != noErr && err != kOTNoError) {
        t->lastError = err;
        t->state = kOTTransport_Error;
    }
}

OTTransport *ot_transport_create(const char *host, uint16_t port)
{
    OTTransport *t;
    OSStatus     err;

    t = (OTTransport *)NewPtrClear(sizeof(OTTransport));
    if (t == NULL) return NULL;

    t->state = kOTTransport_Idle;
    t->port  = port;

    if (host == NULL || strlen(host) >= sizeof(t->host)) {
        t->lastError = kOTBadNameErr;
        t->state = kOTTransport_Error;
        return t;
    }
    strcpy(t->host, host);

    err = ot_setup_endpoint(t);
    if (err != noErr) {
        t->lastError = err;
        t->state = kOTTransport_Error;
        return t;
    }

    /* Record connection start time for timeout tracking */
    t->connect_start_ticks = (uint32_t)TickCount();

    /* Start DNS resolution */
    t->state = kOTTransport_ResolvingDNS;
    ot_start_dns(t);

    return t;
}

OTTransport *ot_transport_adopt(EndpointRef ep)
{
    OTTransport *t;
    OSStatus     err;

    if (ep == NULL) return NULL;

    t = (OTTransport *)NewPtrClear(sizeof(OTTransport));
    if (t == NULL) return NULL;

    t->endpoint = ep;
    t->connect_start_ticks = (uint32_t)TickCount();

    /*
     * Take the endpoint over from whoever was driving it in the clear. Its
     * notifier pointed at their state, so ours has to replace it before any
     * further event arrives.
     */
    OTRemoveNotifier(ep);

    err = OTInstallNotifier(ep, ot_notifier, t);
    if (err == noErr) err = OTSetAsynchronous(ep);
    if (err == noErr) err = OTSetNonBlocking(ep);
    if (err != noErr) {
        t->lastError = err;
        t->state = kOTTransport_Error;
        return t;
    }

    /* TCP is already up; the handshake can start on the next pump. */
    t->state = kOTTransport_Connected;
    return t;
}

OTTransportState ot_transport_pump(OTTransport *t)
{
    switch (t->state) {

    case kOTTransport_ResolvingDNS:
        if ((uint32_t)TickCount() - t->connect_start_ticks > OT_CONNECT_TIMEOUT_TICKS) {
            t->lastError = -3259;  /* kETIMEDOUTErr */
            t->state = kOTTransport_Error;
            break;
        }
        if (t->disconnectReceived) {
            ot_consume_disconnect(t);
            t->state = kOTTransport_Error;
            break;
        }
        if (t->dnsComplete) {
            if (t->lastError != noErr) {
                t->state = kOTTransport_Error;
                break;
            }
            /*
             * DNS resolved. Now start the TCP connection.
             *
             * t->hostInfo.addrs[0] contains the resolved IP.
             * We build a TCall struct — OT's equivalent of
             * sockaddr_in — and call OTConnect.
             *
             * OTConnect in async mode returns immediately.
             * The notifier fires T_CONNECT when the TCP
             * three-way handshake (SYN → SYN-ACK → ACK)
             * completes successfully.
             */
            {
                /* t->remoteAddr and t->sndCall live in the transport, never on
                 * the stack: this call is asynchronous and OT dereferences
                 * them after we have returned. */
                t->addrIndex = 0;
                if (!ot_connect_current(t)) {
                    t->state = kOTTransport_Error;
                    break;
                }
                t->state = kOTTransport_Connecting;
            }
        }
        break;

    case kOTTransport_Connecting:
        if ((uint32_t)TickCount() - t->connect_start_ticks > OT_CONNECT_TIMEOUT_TICKS) {
            t->lastError = -3259;  /* kETIMEDOUTErr */
            t->state = kOTTransport_Error;
            break;
        }
        if (t->disconnectReceived) {
            ot_consume_disconnect(t);
            /* That address refused us; the resolver may have given others. */
            if (ot_try_next_address(t)) break;
            t->state = kOTTransport_Error;
            break;
        }
        if (t->connectComplete) {
            t->state = kOTTransport_Connected;
        }
        break;

    case kOTTransport_Connected:
        if (t->disconnectReceived) {
            ot_consume_disconnect(t);
            t->state = kOTTransport_Error;
            break;
        }
        /*
         * When the peer sends FIN (T_ORDREL), OT refuses further
         * sends with kOTLookErr until we acknowledge the event by
         * calling OTRcvOrderlyDisconnect(). This consumes the pending
         * event but does NOT close our send direction — TCP half-close
         * allows us to keep sending data even after the peer closed
         * its side. This is critical for TLS 1.3 where the server may
         * finish sending everything and close before we've sent our
         * client Finished.
         *
         * After OTRcvOrderlyDisconnect(), we stay in Connected state
         * so sends still work. The caller will drive the final close
         * via ot_transport_close() when it's done sending.
         */
        if (t->ordRelReceived && !t->ordRelConsumed) {
            OTRcvOrderlyDisconnect(t->endpoint);
            t->ordRelConsumed = true;
        }
        break;

    case kOTTransport_Closing:
        if (t->ordRelReceived || t->disconnectReceived) {
            t->state = kOTTransport_Closed;
        }
        break;

    default:
        break;
    }

    return t->state;
}

int ot_transport_send(OTTransport *t, const void *buf, size_t len)
{
    OTResult result;

    if (t->state != kOTTransport_Connected) return -1;

    /*
     * OTSnd sends data. Returns:
     *   > 0: number of bytes actually sent (may be less than len)
     *   kOTFlowErr: send buffer is full, try again later
     *   other negative: error
     *
     * This is non-blocking because we called OTSetNonBlocking.
     */
    result = OTSnd(t->endpoint, (void *)buf, len, 0);

    if (result == kOTFlowErr) return 0;  /* buffer full, not an error */
    if (result < 0) {
        t->lastError = result;
        return -1;
    }
    return (int)result;
}

int ot_transport_recv(OTTransport *t, void *buf, size_t len)
{
    OTResult result;
    OTFlags  flags = 0;

    if (t->state != kOTTransport_Connected &&
        t->state != kOTTransport_Closing) return -1;

    /*
     * OTRcv receives data. Returns:
     *   > 0: number of bytes read
     *   kOTNoDataErr: nothing available right now
     *   other negative: error
     */
    result = OTRcv(t->endpoint, buf, len, &flags);

    if (result == kOTNoDataErr) {
        t->dataAvailable = false;  /* consumed all pending data */
        /*
         * If the peer sent ordRel, there will be no more data ever.
         * Return -1 so the caller knows to stop trying.
         */
        if (t->ordRelReceived) {
            return -1;
        }
        return 0;
    }
    if (result < 0) {
        t->lastError = result;
        return -1;
    }
    return (int)result;
}

void ot_transport_close(OTTransport *t)
{
    if (t->state == kOTTransport_Connected) {
        /*
         * OTSndOrderlyDisconnect sends a TCP FIN — "I'm done
         * sending, but I'll still read your remaining data."
         * The peer responds with their own FIN eventually.
         */
        OTSndOrderlyDisconnect(t->endpoint);
        t->state = kOTTransport_Closing;
    }
}

void ot_transport_destroy(OTTransport *t)
{
    if (t == NULL) return;

    /*
     * Remove the notifiers before closing, and before this struct -- which is
     * the context they were installed with -- is handed back to the heap
     * (Gateway patch, see PATCHES.md).
     *
     * Neither provider had its notifier removed here. A transport torn down
     * with a lookup or a connect still outstanding could have an event
     * delivered afterwards, writing into freed memory and corrupting the
     * Memory Manager's free list. It shows up as a crash somewhere unrelated,
     * some time later.
     */
    if (t->inetSvc != NULL) {
        OTRemoveNotifier(t->inetSvc);
        OTCloseProvider(t->inetSvc);
    }

    if (t->endpoint != NULL) {
        OTRemoveNotifier(t->endpoint);
        OTCloseProvider(t->endpoint);
    }
    DisposePtr((Ptr)t);
}
