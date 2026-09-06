/*
 * ot_transport.h — Internal Open Transport TCP wrapper
 *
 * This module owns the OT endpoint lifecycle and provides a simple
 * interface for the Certainly core to send/receive raw TCP bytes.
 * It handles:
 *   - DNS resolution (async)
 *   - TCP connection (async)
 *   - Non-blocking send/receive
 *   - Connection state tracking
 *   - Orderly shutdown
 *
 * All functions are non-blocking. The caller drives progress by
 * calling ot_transport_pump() from the Mac OS event loop.
 */

#ifndef CERTAINLY_OT_TRANSPORT_H
#define CERTAINLY_OT_TRANSPORT_H

#include <OpenTransport.h>
#include <OpenTptInternet.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    kOTTransport_Idle,          /* created but not started          */
    kOTTransport_ResolvingDNS,  /* waiting for hostname lookup      */
    kOTTransport_Connecting,    /* TCP three-way handshake          */
    kOTTransport_Connected,     /* TCP connection established       */
    kOTTransport_Closing,       /* orderly shutdown in progress     */
    kOTTransport_Closed,        /* fully closed                     */
    kOTTransport_Error          /* something went wrong             */
} OTTransportState;

typedef struct {
    /* OT endpoint — the "phone" we talk through */
    EndpointRef     endpoint;

    /* Internet services provider handle used for DNS resolution.
     * Opened in ot_start_dns() and closed in ot_transport_destroy(). */
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
     * ot_transport_pump(), so the frame was gone by then (Gateway patch - see
     * PATCHES.md).
     */
    InetAddress     remoteAddr;
    TCall           sndCall;

    /* Which of hostInfo.addrs we are currently trying. */
    int             addrIndex;

    /* State tracking */
    OTTransportState state;
    volatile OSStatus lastError;

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
} OTTransport;

/* 30 seconds at 60 ticks/sec */
#define OT_CONNECT_TIMEOUT_TICKS (30 * 60)

/*
 * Create a transport and begin connecting to host:port.
 * Kicks off DNS resolution immediately. Returns NULL on allocation failure.
 */
OTTransport *ot_transport_create(const char *host, uint16_t port);

/*
 * Wrap an endpoint that is already open, bound and connected, and that has
 * already carried plaintext. This is what STARTTLS needs: the caller speaks
 * the cleartext part of the protocol itself, then hands the endpoint over.
 *
 * Ownership of ep transfers to the returned transport, which closes it in
 * ot_transport_destroy(). The caller must have removed its own notifier, or
 * accept that this function replaces it. Returns NULL on allocation failure.
 */
OTTransport *ot_transport_adopt(EndpointRef ep);

/*
 * Drive the connection state machine forward. Call this frequently.
 * Checks notifier flags, advances state (DNS done → connect, etc.).
 * Returns current state.
 */
OTTransportState ot_transport_pump(OTTransport *t);

/*
 * Send raw bytes over TCP. Non-blocking.
 * Returns bytes actually sent (may be less than len if buffer full).
 * Returns -1 on error.
 */
int ot_transport_send(OTTransport *t, const void *buf, size_t len);

/*
 * Receive raw bytes from TCP. Non-blocking.
 * Returns bytes read, 0 if nothing available, -1 on error.
 */
int ot_transport_recv(OTTransport *t, void *buf, size_t len);

/*
 * Begin orderly shutdown. Sends TCP FIN.
 * Actual closure completes asynchronously via pump.
 */
void ot_transport_close(OTTransport *t);

/*
 * Free all resources. Closes endpoint if still open.
 * t is invalid after this call.
 */
void ot_transport_destroy(OTTransport *t);

#endif /* CERTAINLY_OT_TRANSPORT_H */
