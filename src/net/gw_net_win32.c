/*
 * gw_net_win32.c - the Winsock implementation of gw_transport.h.
 *
 * Winsock 1.1 throughout, for Windows 95 OSR2 and up. The Mac implementation
 * in gw_net.c is 630 lines and this is a little over half that, for the reason
 * docs/porting.md section 3 predicted: Open Transport needs the tilisten
 * module, a T_LISTEN notification and an OTListen/OTAccept pair against a
 * second endpoint to accept a connection, where Winsock needs listen() and a
 * non-blocking accept() polled from the loop.
 *
 * The stream layer is not here. gw_stream.c is shared with the Mac build.
 */

#include "gw_net_win32.h"

#include <stdio.h>
#include <string.h>

/* 30 seconds. GetTickCount() counts milliseconds, not sixtieths. */
#define GW_CONNECT_TIMEOUT_MS 30000

struct GWConn {
    SOCKET        sock;
    GWConnState   state;
    UInt16        port;
    char          host[GW_NET_HOST_MAX];

    long          err;              /* WSAGetLastError() at the failure */
    UInt32        addr;             /* peer, host byte order */
    int           remoteEOF;        /* peer sent FIN, or reset us */
    int           sentFIN;
    DWORD         startedAt;

    /* See transport_win32.c: resolution runs on a thread so that one slow
     * lookup cannot stall every other session in the cooperative loop. */
    HANDLE        dnsThread;
    volatile LONG dnsDone;          /* 0 pending, 1 resolved, -1 failed */
    volatile DWORD dnsAddr;         /* network byte order */
    volatile LONG dnsError;
};

struct GWListener {
    SOCKET sock;
    UInt16 port;
    int    open;
};

/* ------------------------------------------------------------------ */
/* Library                                                             */
/* ------------------------------------------------------------------ */

static int sUp;

OSStatus GWNet_Init(void)
{
    WSADATA wsa;

    if (sUp) return 0;
    if (WSAStartup(MAKEWORD(1, 1), &wsa) != 0) return (OSStatus)WSAGetLastError();
    sUp = 1;
    return 0;
}

void GWNet_Shutdown(void)
{
    if (!sUp) return;
    WSACleanup();
    sUp = 0;
}

unsigned long GWNet_Ticks(void)
{
    /*
     * Sixtieths of a second, because every timeout above this layer was
     * written against the Mac's tick rate -- GW_IDLE_TIMEOUT is 45 * 60 and
     * means 45 seconds. The 64-bit intermediate is not fussiness: GetTickCount
     * reaches a milliard milliseconds in under a fortnight of uptime, and
     * multiplying that by 60 in 32 bits wraps long before the counter does.
     */
    return (unsigned long)((ULONGLONG)GetTickCount() * 60 / 1000);
}

/* ------------------------------------------------------------------ */
/* Name resolution                                                     */
/* ------------------------------------------------------------------ */

static DWORD WINAPI resolve_thread(LPVOID param)
{
    GWConn         *c = (GWConn *)param;
    struct hostent *he;

    he = gethostbyname(c->host);
    if (he != NULL && he->h_addrtype == AF_INET && he->h_length == 4 &&
        he->h_addr_list != NULL && he->h_addr_list[0] != NULL) {
        DWORD a;

        memcpy(&a, he->h_addr_list[0], 4);
        c->dnsAddr = a;
        c->dnsDone = 1;         /* written last, on purpose */
    } else {
        c->dnsError = (LONG)WSAGetLastError();
        c->dnsDone  = -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Connection                                                          */
/* ------------------------------------------------------------------ */

static int set_nonblocking(SOCKET s)
{
    unsigned long on = 1;

    return ioctlsocket(s, FIONBIO, &on) != SOCKET_ERROR;
}

GWConn *GWConn_Connect(const char *host, UInt16 port)
{
    GWConn       *c;
    DWORD         tid;
    unsigned long literal;

    if (host == NULL || host[0] == '\0') return NULL;
    if (strlen(host) >= GW_NET_HOST_MAX) return NULL;
    if (GWNet_Init() != 0) return NULL;

    c = (GWConn *)NewPtrClear(sizeof(GWConn));
    if (c == NULL) return NULL;

    c->sock = INVALID_SOCKET;
    c->port = port;
    strcpy(c->host, host);
    c->startedAt = GetTickCount();

    literal = inet_addr(host);
    if (literal != INADDR_NONE) {
        c->dnsAddr = literal;
        c->dnsDone = 1;
    } else {
        c->dnsThread = CreateThread(NULL, 0, resolve_thread, c, 0, &tid);
        if (c->dnsThread == NULL) {
            c->err = (long)GetLastError();
            c->state = kGWConnError;
            return c;
        }
    }
    c->state = kGWConnResolving;
    return c;
}

static int begin_connect(GWConn *c)
{
    struct sockaddr_in sa;

    c->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->sock == INVALID_SOCKET || !set_nonblocking(c->sock)) {
        c->err = WSAGetLastError();
        return 0;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(c->port);
    sa.sin_addr.s_addr = c->dnsAddr;

    if (connect(c->sock, (struct sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR) {
        int err = WSAGetLastError();

        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
            c->err = err;
            return 0;
        }
    }
    c->addr      = ntohl(c->dnsAddr);
    c->startedAt = GetTickCount();
    return 1;
}

GWConnState GWConn_Pump(GWConn *c)
{
    if (c == NULL) return kGWConnError;

    switch (c->state) {
    case kGWConnResolving:
        if (c->dnsDone == 0) {
            if (GetTickCount() - c->startedAt > GW_CONNECT_TIMEOUT_MS) {
                c->err = WSAETIMEDOUT;
                c->state = kGWConnError;
            }
            break;
        }
        if (c->dnsDone < 0) {
            c->err = c->dnsError;
            c->state = kGWConnError;
            break;
        }
        c->state = begin_connect(c) ? kGWConnConnecting : kGWConnError;
        break;

    case kGWConnConnecting: {
        struct timeval tv;
        fd_set         writable, failed;
        int            n, err, len = sizeof(err);

        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(c->sock, &writable);
        FD_SET(c->sock, &failed);
        tv.tv_sec = 0;
        tv.tv_usec = 0;

        n = select(0, NULL, &writable, &failed, &tv);
        if (n == SOCKET_ERROR) {
            c->err = WSAGetLastError();
            c->state = kGWConnError;
            break;
        }
        if (n == 0) {
            if (GetTickCount() - c->startedAt > GW_CONNECT_TIMEOUT_MS) {
                c->err = WSAETIMEDOUT;
                c->state = kGWConnError;
            }
            break;
        }
        /*
         * Writable does not mean connected: a refusal can arrive that way
         * rather than in the exception set. SO_ERROR is what separates them.
         */
        if (getsockopt(c->sock, SOL_SOCKET, SO_ERROR, (char *)&err, &len) == 0
            && err != 0) {
            c->err = err;
            c->state = kGWConnError;
            break;
        }
        if (FD_ISSET(c->sock, &failed)) {
            c->state = kGWConnError;
            break;
        }
        c->state = kGWConnReady;
        break;
    }

    default:
        break;
    }
    return c->state;
}

long GWConn_Send(GWConn *c, const void *buf, size_t len)
{
    int n;

    if (c == NULL || c->sock == INVALID_SOCKET) return -1;
    if (len == 0) return 0;

    n = send(c->sock, (const char *)buf, (int)len, 0);
    if (n == SOCKET_ERROR) {
        int err = WSAGetLastError();

        if (err == WSAEWOULDBLOCK) return 0;   /* flow controlled, not broken */
        c->err = err;
        return -1;
    }
    return n;
}

long GWConn_Recv(GWConn *c, void *buf, size_t len)
{
    int n;

    if (c == NULL || c->sock == INVALID_SOCKET) return -1;
    if (c->remoteEOF) return -2;
    if (len == 0) return 0;

    n = recv(c->sock, (char *)buf, (int)len, 0);
    if (n == 0) {
        c->remoteEOF = 1;
        return -2;                              /* orderly close by the peer */
    }
    if (n == SOCKET_ERROR) {
        int err = WSAGetLastError();

        if (err == WSAEWOULDBLOCK) return 0;
        if (err == WSAECONNRESET || err == WSAECONNABORTED) {
            c->remoteEOF = 1;
            c->err = err;
            return -2;
        }
        c->err = err;
        return -1;
    }
    return n;
}

CTSocket GWConn_DetachSocket(GWConn *c)
{
    SOCKET s;

    if (c == NULL || c->sock == INVALID_SOCKET) return CT_SOCKET_NONE;

    s = c->sock;
    c->sock = INVALID_SOCKET;
    c->state = kGWConnClosed;
    return (CTSocket)s;
}

GWConnState GWConn_GetState(const GWConn *c)
{
    return (c == NULL) ? kGWConnError : c->state;
}

int GWConn_PeerClosed(const GWConn *c)
{
    return (c == NULL) ? 1 : c->remoteEOF;
}

long GWConn_LastError(const GWConn *c)
{
    return (c == NULL) ? 0 : c->err;
}

UInt32 GWConn_PeerIPv4(const GWConn *c)
{
    return (c == NULL) ? 0 : c->addr;
}

void GWConn_Close(GWConn *c)
{
    if (c == NULL || c->sock == INVALID_SOCKET || c->sentFIN) return;

    shutdown(c->sock, 1 /* SD_SEND */);
    c->sentFIN = 1;
    if (c->state == kGWConnReady) c->state = kGWConnClosing;
}

void GWConn_Destroy(GWConn *c)
{
    if (c == NULL) return;

    /* A resolver thread still running holds this pointer; it has to finish
     * before the memory goes back. See transport_win32.c. */
    if (c->dnsThread != NULL) {
        WaitForSingleObject(c->dnsThread, INFINITE);
        CloseHandle(c->dnsThread);
    }
    if (c->sock != INVALID_SOCKET) closesocket(c->sock);
    DisposePtr((Ptr)c);
}

void GWConn_PeerText(GWConn *c, char *out, size_t cap)
{
    UInt32 a;

    if (out == NULL || cap == 0) return;
    if (c == NULL) { out[0] = '\0'; return; }

    a = c->addr;
    snprintf(out, cap, "%lu.%lu.%lu.%lu",
             (unsigned long)((a >> 24) & 0xFF), (unsigned long)((a >> 16) & 0xFF),
             (unsigned long)((a >> 8) & 0xFF),  (unsigned long)(a & 0xFF));
}

/* ------------------------------------------------------------------ */
/* Listener                                                            */
/* ------------------------------------------------------------------ */

GWListener *GWListener_Open(UInt16 port, int backlog)
{
    GWListener        *l;
    struct sockaddr_in sa;

    if (GWNet_Init() != 0) return NULL;

    l = (GWListener *)NewPtrClear(sizeof(GWListener));
    if (l == NULL) return NULL;
    l->port = port;

    l->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l->sock == INVALID_SOCKET) {
        gw_log("listen %u: socket %d", (unsigned)port, WSAGetLastError());
        DisposePtr((Ptr)l);
        return NULL;
    }

    /*
     * No SO_REUSEADDR. On Windows it does not mean "allow a quick rebind" as
     * it does on BSD -- it lets a second socket bind a port that is already
     * in use, so a stray second Gateway would silently steal connections
     * instead of failing to start. docs/porting.md section 3.
     */

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);  /* CLAUDE.md rule 7 */

    if (bind(l->sock, (struct sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR ||
        listen(l->sock, backlog) == SOCKET_ERROR ||
        !set_nonblocking(l->sock)) {
        gw_log("listen %u: bind/listen %d", (unsigned)port, WSAGetLastError());
        closesocket(l->sock);
        DisposePtr((Ptr)l);
        return NULL;
    }

    l->open = 1;
    gw_log("listening on port %u", (unsigned)port);
    return l;
}

GWConn *GWListener_Poll(GWListener *l, int accepting)
{
    struct sockaddr_in sa;
    int                len = sizeof(sa);
    SOCKET             s;
    GWConn            *c;

    if (l == NULL || !l->open) return NULL;

    /*
     * When there is nowhere to put a connection, simply do not accept one: it
     * stays in the listen backlog and the client waits. Accepting and then
     * closing sends a reset, which a browser reads as "this resource is gone"
     * -- the bug that made archived pages load with half their images missing.
     *
     * There is no pending state to drive here, unlike Open Transport, so this
     * is the whole of it.
     */
    if (!accepting) return NULL;

    s = accept(l->sock, (struct sockaddr *)&sa, &len);
    if (s == INVALID_SOCKET) return NULL;       /* WSAEWOULDBLOCK, normally */

    if (!set_nonblocking(s)) {
        closesocket(s);
        return NULL;
    }

    c = (GWConn *)NewPtrClear(sizeof(GWConn));
    if (c == NULL) {
        closesocket(s);
        return NULL;
    }
    c->sock  = s;
    c->state = kGWConnReady;
    c->addr  = ntohl(sa.sin_addr.s_addr);
    c->port  = ntohs(sa.sin_port);
    return c;
}

void GWListener_Close(GWListener *l)
{
    if (l == NULL) return;
    if (l->sock != INVALID_SOCKET) closesocket(l->sock);
    DisposePtr((Ptr)l);
}
