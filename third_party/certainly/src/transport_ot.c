/*
 * transport_ot.c — the Open Transport implementation of certainly_transport.h
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
 * Then ct_transport_pump(), which runs at normal application time,
 * checks those flags and does the actual work. This two-phase
 * pattern (notifier sets flags → pump reads flags) is the standard
 * way to do async OT programming.
 */

#include "certainly_transport.h"

/*
 * The Internet-protocol half of Open Transport: InetSvcRef, InetHostInfo,
 * InetAddress and the DNS events. It used to come in through the transport
 * header, which is now platform neutral, so it belongs here with the rest of
 * the implementation.
 */
#include <OpenTptInternet.h>
#include <string.h>
#include <Memory.h>  /* NewPtrClear, DisposePtr */
#include <Events.h>  /* TickCount */

/*
 * The Open Transport shape of a CTransport. It was in the header until the
 * interface was made portable; certainly.c read six of these fields directly
 * and now goes through the accessors at the bottom of this file.
 */
struct CTransport {
    /* OT endpoint - the "phone" we talk through */
    EndpointRef     endpoint;

    /* Internet services provider handle used for DNS resolution.
     * Opened in ot_start_dns() and closed in ct_transport_destroy(). */
    InetSvcRef      inetSvc;

    /* DNS resolution result */
    InetHostInfo    hostInfo;
    uint16_t        port;

    /*
     * Our own copy of the hostname. OTInetStringToAddress() is issued
     * asynchronously and Open Transport does not copy the name: the buffer has
     * to stay put until T_DNRSTRINGTOADDRCOMPLETE arrives. Holding the
     * caller's pointer worked only as long as callers passed string literals
     * (Gateway patch - see PATCHES.md).
     */
    char            host[256];

    /*
     * The address and call structure OTConnect() is given. Like the hostname
     * above, these must outlive the call: OTConnect on an asynchronous
     * endpoint returns immediately and Open Transport reads the address later,
     * when it actually sends the SYN. They used to be locals in
     * ct_transport_pump(), so the frame was gone by then (Gateway patch - see
     * PATCHES.md).
     */
    InetAddress     remoteAddr;
    TCall           sndCall;

    /* Which of hostInfo.addrs we are currently trying. */
    int             addrIndex;

    /* State tracking */
    CTransportState state;
    volatile OSStatus lastError;

    /* Raw bytes across the endpoint, for ct_transport_counters(). */
    unsigned long   bytesSent;
    unsigned long   bytesRecv;

    /* Connection start time for timeout tracking (in ticks) */
    uint32_t        connect_start_ticks;

    /* Flags set by the notifier callback (runs at interrupt time) */
    volatile bool   connectComplete;
    volatile bool   dataAvailable;
    volatile bool   ordRelReceived;
    volatile bool   disconnectReceived;
    volatile bool   dnsComplete;

    /* True once we've called OTRcvOrderlyDisconnect to clear the event */
    bool            ordRelConsumed;
};


/* Forward declarations */
static pascal void ot_notifier(void *context, OTEventCode event,
                               OTResult result, void *cookie);
static OSStatus    ot_setup_endpoint(CTransport *t);
static void        ot_start_dns(CTransport *t);

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
    CTransport *t = (CTransport *)context;

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
static void ot_consume_disconnect(CTransport *t)
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
static Boolean ot_connect_current(CTransport *t)
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
static Boolean ot_try_next_address(CTransport *t)
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
static OSStatus ot_setup_endpoint(CTransport *t)
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
static void ot_start_dns(CTransport *t)
{
    OSStatus   err;

    t->inetSvc = OTOpenInternetServices(
        kDefaultInternetServicesPath,
        0, &err
    );
    if (err != noErr) {
        t->lastError = err;
        t->state = kCTransport_Error;
        return;
    }

    OTInstallNotifier(t->inetSvc, ot_notifier, t);
    OTSetAsynchronous(t->inetSvc);

    /* t->host, never the caller's buffer: this call is asynchronous and OT
     * reads the name later, when the resolver runs. */
    err = OTInetStringToAddress(t->inetSvc, t->host, &t->hostInfo);
    if (err != noErr && err != kOTNoError) {
        t->lastError = err;
        t->state = kCTransport_Error;
    }
}

CTransport *ct_transport_create(const char *host, uint16_t port)
{
    CTransport *t;
    OSStatus     err;

    t = (CTransport *)NewPtrClear(sizeof(CTransport));
    if (t == NULL) return NULL;

    t->state = kCTransport_Idle;
    t->port  = port;

    if (host == NULL || strlen(host) >= sizeof(t->host)) {
        t->lastError = kOTBadNameErr;
        t->state = kCTransport_Error;
        return t;
    }
    strcpy(t->host, host);

    err = ot_setup_endpoint(t);
    if (err != noErr) {
        t->lastError = err;
        t->state = kCTransport_Error;
        return t;
    }

    /* Record connection start time for timeout tracking */
    t->connect_start_ticks = (uint32_t)TickCount();

    /* Start DNS resolution */
    t->state = kCTransport_ResolvingDNS;
    ot_start_dns(t);

    return t;
}

CTransport *ct_transport_adopt(EndpointRef ep)
{
    CTransport *t;
    OSStatus     err;

    if (ep == NULL) return NULL;

    t = (CTransport *)NewPtrClear(sizeof(CTransport));
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
        t->state = kCTransport_Error;
        return t;
    }

    /* TCP is already up; the handshake can start on the next pump. */
    t->state = kCTransport_Connected;
    return t;
}

CTransportState ct_transport_pump(CTransport *t)
{
    switch (t->state) {

    case kCTransport_ResolvingDNS:
        if ((uint32_t)TickCount() - t->connect_start_ticks > CT_CONNECT_TIMEOUT_TICKS) {
            t->lastError = -3259;  /* kETIMEDOUTErr */
            t->state = kCTransport_Error;
            break;
        }
        if (t->disconnectReceived) {
            ot_consume_disconnect(t);
            t->state = kCTransport_Error;
            break;
        }
        if (t->dnsComplete) {
            if (t->lastError != noErr) {
                t->state = kCTransport_Error;
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
                    t->state = kCTransport_Error;
                    break;
                }
                t->state = kCTransport_Connecting;
            }
        }
        break;

    case kCTransport_Connecting:
        if ((uint32_t)TickCount() - t->connect_start_ticks > CT_CONNECT_TIMEOUT_TICKS) {
            t->lastError = -3259;  /* kETIMEDOUTErr */
            t->state = kCTransport_Error;
            break;
        }
        if (t->disconnectReceived) {
            ot_consume_disconnect(t);
            /* That address refused us; the resolver may have given others. */
            if (ot_try_next_address(t)) break;
            t->state = kCTransport_Error;
            break;
        }
        if (t->connectComplete) {
            t->state = kCTransport_Connected;
        }
        break;

    case kCTransport_Connected:
        if (t->disconnectReceived) {
            ot_consume_disconnect(t);
            t->state = kCTransport_Error;
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
         * via ct_transport_close() when it's done sending.
         */
        if (t->ordRelReceived && !t->ordRelConsumed) {
            OTRcvOrderlyDisconnect(t->endpoint);
            t->ordRelConsumed = true;
        }
        break;

    case kCTransport_Closing:
        if (t->ordRelReceived || t->disconnectReceived) {
            t->state = kCTransport_Closed;
        }
        break;

    default:
        break;
    }

    return t->state;
}

int ct_transport_send(CTransport *t, const void *buf, size_t len)
{
    OTResult result;

    if (t->state != kCTransport_Connected) return -1;

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
    t->bytesSent += (unsigned long)result;
    return (int)result;
}

int ct_transport_recv(CTransport *t, void *buf, size_t len)
{
    OTResult result;
    OTFlags  flags = 0;

    if (t->state != kCTransport_Connected &&
        t->state != kCTransport_Closing) return -1;

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
    t->bytesRecv += (unsigned long)result;
    return (int)result;
}

void ct_transport_close(CTransport *t)
{
    if (t->state == kCTransport_Connected) {
        /*
         * OTSndOrderlyDisconnect sends a TCP FIN — "I'm done
         * sending, but I'll still read your remaining data."
         * The peer responds with their own FIN eventually.
         */
        OTSndOrderlyDisconnect(t->endpoint);
        t->state = kCTransport_Closing;
    }
}

void ct_transport_destroy(CTransport *t)
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

/* ------------------------------------------------------------------ */
/* Accessors: what the TLS core is allowed to know                     */
/* ------------------------------------------------------------------ */

CTransportState ct_transport_state(const CTransport *t)
{
    return (t == NULL) ? kCTransport_Error : t->state;
}

int ct_transport_peer_closed(const CTransport *t)
{
    if (t == NULL) return 1;
    return (t->ordRelReceived || t->disconnectReceived) ? 1 : 0;
}

uint16_t ct_transport_port(const CTransport *t)
{
    return (t == NULL) ? 0 : t->port;
}

long ct_transport_last_error(const CTransport *t)
{
    return (t == NULL) ? 0 : (long)t->lastError;
}

uint32_t ct_transport_peer_ipv4(const CTransport *t)
{
    if (t == NULL || !t->dnsComplete) return 0;
    return (uint32_t)t->hostInfo.addrs[0];
}

void ct_socket_close(CTSocket sock)
{
    if (sock != CT_SOCKET_NONE) OTCloseProvider(sock);
}

void ct_transport_counters(const CTransport *t,
                           unsigned long *sent, unsigned long *received)
{
    if (sent != NULL)     *sent     = (t == NULL) ? 0 : t->bytesSent;
    if (received != NULL) *received = (t == NULL) ? 0 : t->bytesRecv;
}
