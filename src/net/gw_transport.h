/*
 * gw_transport.h - what a platform has to provide for Gateway to run on it.
 *
 * This is the interface the modules in src/proxy/ speak, and nothing in it
 * names an operating system: GWConn and GWListener are opaque, so the Open
 * Transport implementation in gw_net.c and a Winsock one can both satisfy it
 * without the 2,100 lines above either of them noticing. docs/porting.md
 * section 2 is the reasoning; this file is that section made real.
 *
 * Three conventions matter more than the signatures, because every state
 * machine above depends on them:
 *
 *   - Nothing blocks. Every call returns at once and the pump does whatever
 *     small amount of work is available this pass.
 *   - Send returning 0 means flow controlled, try the next slice. It is not
 *     an error and not a closed connection.
 *   - Recv distinguishes four cases: >0 bytes, 0 for nothing right now, -1
 *     for a broken connection, -2 for an orderly close by the peer.
 *     Collapsing 0 and -2 breaks every splice.
 */
#ifndef GW_TRANSPORT_H
#define GW_TRANSPORT_H

#include <stddef.h>

#include "../gw_platform.h"

/*
 * For CTSocket, the platform's own connection handle. The stream layer hands
 * one to Certainly when a plaintext connection is upgraded in place, which is
 * the only place Gateway and the TLS library have to agree on what a socket
 * is. Relative, because certainly/src is private to that library's own build.
 */
#include "../../third_party/certainly/src/certainly_transport.h"

#define GW_NET_HOST_MAX      256
#define GW_CONNECT_TIMEOUT   (30 * 60)      /* ticks: 30 seconds */

/* ------------------------------------------------------------------ */
/* Raw TCP connection                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    kGWConnIdle = 0,
    kGWConnResolving,
    kGWConnConnecting,
    kGWConnAccepting,
    kGWConnReady,
    kGWConnClosing,
    kGWConnClosed,
    kGWConnError
} GWConnState;

/*
 * Opaque on purpose. The proxy layer touches no field of either of these --
 * verified by grep before the split, and worth keeping true.
 */
typedef struct GWConn     GWConn;
typedef struct GWListener GWListener;

OSStatus      GWNet_Init(void);
void          GWNet_Shutdown(void);
unsigned long GWNet_Ticks(void);

/* Start a DNS lookup and TCP connect. Never blocks; drive with GWConn_Pump. */
GWConn      *GWConn_Connect(const char *host, UInt16 port);
GWConnState  GWConn_Pump(GWConn *c);

/* >= 0: bytes handed to the stack (0 means flow-controlled). -1: error. */
long         GWConn_Send(GWConn *c, const void *buf, size_t len);

/* >= 0: bytes read (0 means nothing yet). -1: error. -2: peer sent FIN. */
long         GWConn_Recv(GWConn *c, void *buf, size_t len);

/*
 * Hand the connection to someone else. Our own bookkeeping comes off it, so
 * GWConn_Destroy() no longer closes it; ownership passes to the caller, which
 * on the STARTTLS path is Certainly. Returns CT_SOCKET_NONE if there is
 * nothing to hand over.
 */
CTSocket     GWConn_DetachSocket(GWConn *c);

/* State, without advancing anything. */
GWConnState  GWConn_GetState(const GWConn *c);

/* True once the peer has closed its side, however it did so. */
int          GWConn_PeerClosed(const GWConn *c);

/* The platform's own error number, for diagnostics. */
long         GWConn_LastError(const GWConn *c);

/* The peer address in host byte order, or 0 if not known. */
UInt32       GWConn_PeerIPv4(const GWConn *c);

void         GWConn_Close(GWConn *c);       /* orderly: sends FIN */
void         GWConn_Destroy(GWConn *c);
void         GWConn_PeerText(GWConn *c, char *out, size_t cap);

/* ------------------------------------------------------------------ */
/* Listener                                                            */
/* ------------------------------------------------------------------ */

/*
 * Bind to every local address on the given port, so both 127.0.0.1 and the
 * machine's configured address reach it (CLAUDE.md rule 7 -- on Mac OS 9, OT
 * loopback alone is not dependable).
 */
GWListener  *GWListener_Open(UInt16 port, int backlog);

/*
 * One slice of the listener's state machine. `accepting` says whether the
 * caller has somewhere to put a finished connection; when it is 0 the listener
 * still drives an accept already in flight but starts no new one, leaving the
 * surplus queued in the operating system's backlog. Returns a fully accepted
 * connection, or NULL when nothing is ready.
 */
GWConn      *GWListener_Poll(GWListener *l, int accepting);
void         GWListener_Close(GWListener *l);

/* ------------------------------------------------------------------ */
/* Stream: one interface over "raw TCP" and "TLS"                      */
/* ------------------------------------------------------------------ */

typedef enum {
    kGWStreamIdle = 0,
    kGWStreamConnecting,
    kGWStreamReady,
    kGWStreamClosed,
    kGWStreamError
} GWStreamState;

/*
 * struct MacTLS_Context rather than the typedef, so this header does not have
 * to include certainly.h -- which still pulls in Open Transport of its own
 * accord. Certainly's transport needs the same treatment as this file did;
 * see docs/porting.md section 2.
 */
typedef struct GWStream {
    Boolean                tls;
    GWConn                *plain;
    struct MacTLS_Context *sec;
    GWStreamState          state;
    Boolean                eof;
    unsigned long          startTicks;
} GWStream;

void          GWStream_Init(GWStream *s);
int           GWStream_ConnectPlain(GWStream *s, const char *host, UInt16 port);
int           GWStream_ConnectTLS(GWStream *s, const char *host, UInt16 port);
void          GWStream_Adopt(GWStream *s, GWConn *c);

/*
 * Turn a live plaintext stream into a TLS one in place (STARTTLS). Call it
 * once the server's "ready to start TLS" reply has been read in full and
 * nothing is left queued in either direction. Returns 0 if the stream is not
 * in a state that can be upgraded.
 */
int           GWStream_UpgradeToTLS(GWStream *s, const char *host);
GWStreamState GWStream_Pump(GWStream *s);

/* Same conventions as GWConn_Send / GWConn_Recv. */
long          GWStream_Write(GWStream *s, const void *buf, size_t len);
long          GWStream_Read(GWStream *s, void *buf, size_t len);


void          GWStream_Close(GWStream *s);
void          GWStream_Destroy(GWStream *s);

/*
 * 1 when the far end has closed its side or the connection has failed.
 *
 * For a client connection this means the browser has gone -- navigated away,
 * stopped, or quit -- and any work still being done on its behalf is wasted.
 */
int           GWStream_PeerGone(const GWStream *s);

/*
 * Raw bytes across the socket in each direction, ciphertext included. Sampled
 * at the moment a handshake completes and again at a failure, the difference
 * is exactly how much application data went out -- which no other measurement
 * here distinguishes from a handshake that happened to be that size.
 */
void          GWStream_Counters(const GWStream *s,
                                unsigned long *sent, unsigned long *received);

/* The negotiated cipher suite, or 0. 0x1301 AES-128-GCM, 0x1303 ChaCha20. */
unsigned int  GWStream_CipherSuite(const GWStream *s);

/* 0 when unknown or plain, otherwise 12 or 13. */
int           GWStream_TlsVersion(const GWStream *s);
const char   *GWStream_ErrorText(const GWStream *s);

/*
 * A failure line with enough in it to act on: what went wrong, how far the
 * connection got, the Open Transport error number, and the address DNS
 * produced. Writes into out and returns it, so it can be passed straight to a
 * logging call.
 */
const char   *GWStream_Describe(const GWStream *s, char *out, size_t cap);

#endif /* GW_TRANSPORT_H */
