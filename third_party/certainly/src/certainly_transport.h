/*
 * certainly_transport.h - the TCP transport Certainly runs on.
 *
 * Certainly needs very little from a network stack: open a connection, pump
 * it, push bytes, pull bytes, close it. This header is that interface and
 * names no operating system, so one build of the TLS code serves any host
 * that can satisfy it. transport_ot.c is the Open Transport implementation
 * for Mac OS 9; transport_win32.c is the Winsock one.
 *
 * Every function is non-blocking. The caller drives progress by calling
 * ct_transport_pump() from its event loop.
 */

#ifndef CERTAINLY_TRANSPORT_H
#define CERTAINLY_TRANSPORT_H

/*
 * A connection handle owned by whoever opened it, for the adopt path.
 *
 * Open Transport hands out a pointer; Winsock hands out an integer, and
 * INVALID_SOCKET is all ones rather than zero. Neither fits the other, so the
 * type is per-platform and callers on each side use their own.
 *
 * This block comes before <stdbool.h> deliberately. MacTypes.h declares true
 * and false as enumerators, so a stdbool that got in first turns that
 * declaration into "enum { 0 = 0, 1 = 1 }" and the Universal Interfaces stop
 * compiling. The header this replaced had the same ordering for the same
 * reason; it just did not say so.
 */
#ifdef CERTAINLY_OPEN_TRANSPORT
#include <OpenTransport.h>
typedef EndpointRef CTSocket;
#define CT_SOCKET_NONE ((CTSocket)0)
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef CERTAINLY_OPEN_TRANSPORT
typedef uintptr_t CTSocket;
#define CT_SOCKET_NONE ((CTSocket)~(uintptr_t)0)
#endif

typedef enum {
    kCTransport_Idle,          /* created but not started          */
    kCTransport_ResolvingDNS,  /* waiting for hostname lookup      */
    kCTransport_Connecting,    /* TCP three-way handshake          */
    kCTransport_Connected,     /* TCP connection established       */
    kCTransport_Closing,       /* orderly shutdown in progress     */
    kCTransport_Closed,        /* fully closed                     */
    kCTransport_Error          /* something went wrong             */
} CTransportState;

/*
 * Opaque. The state lives entirely inside whichever implementation is linked;
 * certainly.c used to read six fields of the Open Transport struct directly,
 * and those reads are now the accessors at the bottom of this file.
 */
typedef struct CTransport CTransport;

/* 30 seconds at 60 ticks/sec */
#define CT_CONNECT_TIMEOUT_TICKS (30 * 60)

/*
 * Create a transport and begin connecting to host:port.
 * Starts name resolution immediately. Returns NULL on allocation failure.
 */
CTransport *ct_transport_create(const char *host, uint16_t port);

/*
 * Wrap a connection that is already open and has already carried plaintext.
 * This is what STARTTLS needs: the caller speaks the cleartext part of the
 * protocol itself, then hands the connection over.
 *
 * Ownership transfers to the returned transport, which closes it in
 * ct_transport_destroy(). Returns NULL on allocation failure.
 */
CTransport *ct_transport_adopt(CTSocket sock);

/*
 * Drive the connection state machine forward. Call this frequently.
 * Returns the current state.
 */
CTransportState ct_transport_pump(CTransport *t);

/*
 * Send raw bytes. Non-blocking. Returns bytes actually sent, which may be
 * fewer than len when the send buffer is full, or -1 on error.
 */
int ct_transport_send(CTransport *t, const void *buf, size_t len);

/*
 * Receive raw bytes. Non-blocking. Returns bytes read, 0 if nothing is
 * available, -1 on error.
 */
int ct_transport_recv(CTransport *t, void *buf, size_t len);

/* Begin orderly shutdown; completion is reported through pump. */
void ct_transport_close(CTransport *t);

/* Free everything, closing the connection if it is still open. */
void ct_transport_destroy(CTransport *t);

/*
 * Close a bare handle that was never adopted.
 *
 * ct_transport_adopt() takes ownership unconditionally, so a caller that fails
 * before reaching it -- out of memory, hostname too long -- still has to
 * dispose of the connection, and must not need to know how. Open Transport
 * closes a provider; Winsock calls closesocket.
 */
void ct_socket_close(CTSocket sock);

/* ------------------------------------------------------------------ */
/* What the TLS core needs to know about the connection under it       */
/* ------------------------------------------------------------------ */

/* The state, without advancing anything. */
CTransportState ct_transport_state(const CTransport *t);

/*
 * True once the peer has closed its side, however it did so -- an orderly
 * shutdown and a reset both count. The TLS layer only cares that no more
 * bytes are coming.
 */
int ct_transport_peer_closed(const CTransport *t);

/* The port this transport was created for, for reconnects. */
uint16_t ct_transport_port(const CTransport *t);

/* The platform's own error number, for diagnostics. */
long ct_transport_last_error(const CTransport *t);

/* The resolved peer address in host byte order, or 0 if not yet known. */
uint32_t ct_transport_peer_ipv4(const CTransport *t);

/*
 * Raw bytes that have crossed the socket in each direction, ciphertext
 * included -- what the peer actually saw, as distinct from what the TLS layer
 * believes it handed over. The distinction matters: a request that the record
 * layer accepted but that never reached the wire looks, from every level
 * above, exactly like a request the peer ignored.
 */
void ct_transport_counters(const CTransport *t,
                           unsigned long *sent, unsigned long *received);

#endif /* CERTAINLY_TRANSPORT_H */
