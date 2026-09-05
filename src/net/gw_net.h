/*
 * gw_net.h - Open Transport listeners, connections and the stream abstraction.
 *
 * NOT PORTABLE. This header pulls in <OpenTransport.h>, which only exists in
 * Apple's Universal Interfaces - the Multiversal Interfaces that Retro68 ships
 * have no Open Transport at all (CLAUDE.md rule 3). Only translation units
 * that are compiled with the vendored Universal CIncludes on their include
 * path may include this file. src/main.cpp must not: it talks to the core
 * through the plain-C gw_core.h instead.
 */
#ifndef GW_NET_H
#define GW_NET_H

#include <OpenTransport.h>
#include <OpenTptInternet.h>

#include <stddef.h>

#include <certainly.h>

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

typedef struct GWConn {
    EndpointRef       ep;
    InetSvcRef        svc;
    InetHostInfo      hinfo;
    InetAddress       remote;
    UInt16            port;
    GWConnState       state;

    /* Written from the notifier, which runs at interrupt time: the notifier
     * only ever sets flags, and GWConn_Pump() does the real work at app time. */
    volatile OSStatus err;
    volatile Boolean  fConnect;
    volatile Boolean  fData;
    volatile Boolean  fOrdRel;
    volatile Boolean  fDisconnect;
    volatile Boolean  fDns;
    volatile Boolean  fPassCon;

    unsigned long     startTicks;
    Boolean           ordRelConsumed;
    Boolean           remoteEOF;
    Boolean           sentFIN;
    char              host[GW_NET_HOST_MAX];
} GWConn;

OSStatus     GWNet_Init(void);
void         GWNet_Shutdown(void);
unsigned long GWNet_Ticks(void);

/* Start a DNS lookup and TCP connect. Never blocks; drive with GWConn_Pump. */
GWConn      *GWConn_Connect(const char *host, UInt16 port);
GWConnState  GWConn_Pump(GWConn *c);

/* >= 0: bytes handed to OT (0 means flow-controlled, try again). -1: error. */
long         GWConn_Send(GWConn *c, const void *buf, size_t len);

/* >= 0: bytes read (0 means nothing yet). -1: error. -2: peer sent FIN. */
long         GWConn_Recv(GWConn *c, void *buf, size_t len);

/*
 * Hand the endpoint to someone else. Our notifier comes off and c->ep is
 * cleared, so GWConn_Destroy() no longer closes it. Used by STARTTLS, where
 * Certainly takes over a socket Gateway has been speaking plaintext on.
 */
EndpointRef  GWConn_DetachEndpoint(GWConn *c);

void         GWConn_Close(GWConn *c);       /* orderly: sends FIN */
void         GWConn_Destroy(GWConn *c);
void         GWConn_PeerText(GWConn *c, char *out, size_t cap);

/* ------------------------------------------------------------------ */
/* Listener                                                            */
/* ------------------------------------------------------------------ */

typedef struct GWListener {
    EndpointRef       ep;
    UInt16            port;
    Boolean           open;
    volatile Boolean  fListen;
    volatile Boolean  fAcceptDone;
    volatile OSStatus err;

    GWConn           *pending;              /* endpoint an accept is landing on */
    TCall             call;
    InetAddress       callAddr;
} GWListener;

/*
 * Bind a listening endpoint to kOTAnyInetAddress on the given port, so both
 * 127.0.0.1 and the address from the TCP/IP control panel reach it
 * (CLAUDE.md rule 7 - OT loopback alone is not dependable).
 */
GWListener  *GWListener_Open(UInt16 port, OTQLen qlen);

/* Returns a fully accepted connection, or NULL when nothing is ready yet. */
GWConn      *GWListener_Poll(GWListener *l);
void         GWListener_Close(GWListener *l);

/* ------------------------------------------------------------------ */
/* Stream: one interface over "raw TCP" and "TLS via Certainly"        */
/* ------------------------------------------------------------------ */

typedef enum {
    kGWStreamIdle = 0,
    kGWStreamConnecting,
    kGWStreamReady,
    kGWStreamClosed,
    kGWStreamError
} GWStreamState;

typedef struct GWStream {
    Boolean          tls;
    GWConn          *plain;
    MacTLS_Context  *sec;
    GWStreamState    state;
    Boolean          eof;
    unsigned long    startTicks;
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

/* 0 when unknown or plain, otherwise 12 or 13. */
int           GWStream_TlsVersion(const GWStream *s);
const char   *GWStream_ErrorText(const GWStream *s);

#endif /* GW_NET_H */
