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
    /*
     * The same stream, with Gateway as the server rather than the client.
     * Set only by GWStream_UpgradeToTLSServer(), for the browser side of a
     * MITM'd CONNECT; `tls` is true alongside it, and every operation that
     * reaches for `sec` checks this first.
     */
    struct MacTLS_Server  *srv;
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

/*
 * The same detach, but for a far end with no TLS 1.3: BearSSL's 1.2 engine
 * drives from the first pump and no fallback reconnect is needed, which is
 * what makes 1.2-only servers reachable through a proxy tunnel or STARTTLS.
 * Returns 0 if the stream is not in a state that can be upgraded.
 */
int           GWStream_UpgradeToTLS12(GWStream *s, const char *host);

/*
 * The same upgrade with the roles reversed: Gateway answers the handshake
 * instead of starting it, presenting `leaf` for whatever host the client
 * asked for. For the browser side of a CONNECT that Gateway terminates.
 *
 * `key` is a `const br_rsa_private_key *` -- void so this header stays clear
 * of BearSSL, as it already stays clear of Certainly. The DER is not copied.
 */
int           GWStream_UpgradeToTLSServer(GWStream *s,
                                          const unsigned char *leaf,
                                          size_t leaf_len,
                                          const unsigned char *ca,
                                          size_t ca_len,
                                          const void *key);
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
int           GWStream_ServerError(const GWStream *s);
int           GWStream_PeerGone(const GWStream *s);

/* 0 when unknown or plain, otherwise 12 or 13. */
int           GWStream_TlsVersion(const GWStream *s);
/* The highest protocol version the browser offered in its ClientHello
 * (0x0300 = SSL 3.0, 0x0301 = TLS 1.0, and so on). 0 when unknown, plain, or
 * before the hello arrived. Server side only, so connect_mitm. */
unsigned int  GWStream_ClientHelloVersion(const GWStream *s);
/*
 * How far the browser got with a server-side handshake, for the log line
 * that says what happened to it. Mirrors Certainly's MacTLS_ServerStage
 * without this header having to see Certainly's.
 */
enum {
    kGWStageNothing = 0,    /* no bytes at all */
    kGWStageHello,          /* a hello, then gone before we answered */
    kGWStageCertificate,    /* our certificate went out, nothing came back */
    kGWStageReplied,        /* it answered our certificate, then left */
    kGWStageFinished,       /* it finished its side, then left */
    kGWStageDone            /* the handshake completed */
};
int           GWStream_ServerStage(const GWStream *s);
/* Engineer's detail for log_debug: one line on the hello and the engine, and
 * the first bytes the browser sent, in hex. Empty strings when not a
 * server-side stream. */
void          GWStream_ServerDescribe(const GWStream *s, char *out, size_t cap);
void          GWStream_ServerHelloHex(const GWStream *s, char *out, size_t cap);
/* The same opening bytes as GWStream_ServerHelloHex(), raw rather than hex,
 * for gw_hello_ciphers() (src/portable/gw_hello.h) to decode the offered
 * cipher list from. Returns the number of bytes written to out. */
size_t        GWStream_ServerHelloRaw(const GWStream *s, unsigned char *out, size_t cap);
/* The version the browser's handshake settled on, 0x0300 = SSL 3.0 and so
 * on, or 0 before it completed. */
unsigned int  GWStream_ServerVersion(const GWStream *s);
/* 1 when the browser-side handshake resumed a cached session. */
int           GWStream_ServerResumed(const GWStream *s);
/* 1 when it tried to, completed or not; see MacTLS_ServerResumeTried. */
int           GWStream_ServerResumeTried(const GWStream *s);
/* 1 while TLS still holds bytes that have not reached the socket. A write to
 * a TLS stream only stages plaintext, so closing on the strength of the write
 * having returned throws the tail away. Plain streams always answer 0: the
 * socket has the bytes already. */
int           GWStream_SendPending(const GWStream *s);
const char   *GWStream_ErrorText(const GWStream *s);

/*
 * 1 when a TLS handshake failure was a TLS 1.2 fallback that could not run
 * on this stream because it was adopted (STARTTLS) rather than dialed by
 * Certainly itself -- see MacTLS_FallbackNoRoute(). The remedy is specific
 * (the far end needs TLS 1.3 enabled), which GWStream_Describe()'s generic
 * failure text cannot say.
 */
int           GWStream_FallbackNoRoute(const GWStream *s);

/*
 * Testing only (tunnel_insecure): stop validating the far end's certificate
 * on this stream. Client side only -- never use on a mail or web-proxy
 * stream. Must run before the first Pump, i.e. right after an Upgrade call
 * returns.
 */
void          GWStream_SetInsecure(GWStream *s);

/*
 * Diagnosis (tunnel_sni): replace the name the handshake sends as SNI, or
 * omit SNI entirely when sni is NULL. Client side only, before the first
 * Pump. This changes only what goes out on the wire -- certificate
 * validation always checks the host the stream was upgraded with
 * (PATCHES.md §34), never this override.
 */
void          GWStream_SetSNI(GWStream *s, const char *sni);

/*
 * A failure line with enough in it to act on: what went wrong, how far the
 * connection got, the Open Transport error number, and the address DNS
 * produced. Writes into out and returns it, so it can be passed straight to a
 * logging call.
 */
const char   *GWStream_Describe(const GWStream *s, char *out, size_t cap);

/*
 * The readable half of the same diagnosis: one plain sentence naming the far
 * end ("the certificate for mail.example.com has expired") written into out,
 * and its code (docs/log-codes.md, the T table) returned for gw_logc().
 * GWStream_Describe() is then the log_debug line under it.
 *
 * NULL, with out empty, when the stream holds no error to explain -- a far end
 * that simply closed, or one that never started -- so the caller says what
 * happened in its own words and with its own code. host is the name to put in
 * the sentence; NULL reads "the far end".
 */
const char   *GWStream_Explain(const GWStream *s, const char *host,
                               char *out, size_t cap);

#endif /* GW_TRANSPORT_H */
