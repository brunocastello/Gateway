/*
 * transport_win32.c — the Winsock implementation of certainly_transport.h.
 *
 * Winsock 1.1, not 2: Winsock 2 was a separate download for Windows 95 even on
 * OSR2, and nothing here needs it. select(), non-blocking sockets and
 * gethostbyname() are the whole surface, and all three are in 1.1.
 *
 * This is a far shorter file than transport_ot.c, and the reason is that
 * Open Transport wants an interrupt-time notifier and a flag-passing dance to
 * achieve what a non-blocking socket does by returning WSAEWOULDBLOCK. The
 * state machine below is the same shape; it just has less to arrange.
 */

#include "certainly_transport.h"

#include <winsock.h>
#include <windows.h>

#include <stdlib.h>
#include <string.h>

/* 30 seconds, in milliseconds: GetTickCount() counts those, not ticks. */
#define CT_CONNECT_TIMEOUT_MS 30000

struct CTransport {
    SOCKET          sock;
    CTransportState state;
    uint16_t        port;
    char            host[256];

    long            lastError;      /* WSAGetLastError() at the failure */
    uint32_t        addr;           /* resolved peer, host byte order */
    int             peerClosed;     /* FIN or reset seen */
    DWORD           startedAt;      /* GetTickCount() when connecting began */

    /*
     * Name resolution runs on its own thread.
     *
     * Winsock 1.1 offers WSAAsyncGetHostByName, but it reports completion by
     * posting to a window, which would tie this file to whichever HWND the
     * application happens to own. A thread that calls the blocking
     * gethostbyname() and sets a flag keeps the transport self-contained, and
     * a proxy that stalled every session on one slow lookup would be worse
     * than either.
     */
    HANDLE          dnsThread;
    volatile LONG   dnsDone;        /* 0 pending, 1 resolved, -1 failed */
    volatile DWORD  dnsAddr;        /* network byte order */
    volatile LONG   dnsError;
};

/* ------------------------------------------------------------------ */
/* Winsock lifetime                                                    */
/* ------------------------------------------------------------------ */

static int sWinsockUp;

static int winsock_start(void)
{
    WSADATA wsa;

    if (sWinsockUp) return 1;
    if (WSAStartup(MAKEWORD(1, 1), &wsa) != 0) return 0;
    sWinsockUp = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Name resolution                                                     */
/* ------------------------------------------------------------------ */

static DWORD WINAPI resolve_thread(LPVOID param)
{
    CTransport     *t = (CTransport *)param;
    struct hostent *he;

    he = gethostbyname(t->host);
    if (he != NULL && he->h_addrtype == AF_INET && he->h_length == 4 &&
        he->h_addr_list != NULL && he->h_addr_list[0] != NULL) {
        DWORD a;

        memcpy(&a, he->h_addr_list[0], 4);
        t->dnsAddr = a;
        /* Written last: the reader takes a non-zero dnsDone as meaning the
         * address beside it is complete. x86 does not reorder stores. */
        t->dnsDone = 1;
    } else {
        t->dnsError = (LONG)WSAGetLastError();
        t->dnsDone  = -1;
    }
    return 0;
}

static int start_resolve(CTransport *t)
{
    DWORD          tid;
    unsigned long  literal;

    /* A dotted quad needs no thread and no resolver. */
    literal = inet_addr(t->host);
    if (literal != INADDR_NONE) {
        t->dnsAddr = literal;
        t->dnsDone = 1;
        return 1;
    }

    t->dnsThread = CreateThread(NULL, 0, resolve_thread, t, 0, &tid);
    return t->dnsThread != NULL;
}

/* ------------------------------------------------------------------ */
/* Connecting                                                          */
/* ------------------------------------------------------------------ */

static int begin_connect(CTransport *t)
{
    struct sockaddr_in sa;
    unsigned long      nonblocking = 1;

    t->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (t->sock == INVALID_SOCKET) {
        t->lastError = WSAGetLastError();
        return 0;
    }
    if (ioctlsocket(t->sock, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        t->lastError = WSAGetLastError();
        return 0;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(t->port);
    sa.sin_addr.s_addr = t->dnsAddr;

    if (connect(t->sock, (struct sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR) {
        int err = WSAGetLastError();

        /* The expected answer from a non-blocking connect. */
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
            t->lastError = err;
            return 0;
        }
    }

    t->addr      = ntohl(t->dnsAddr);
    t->startedAt = GetTickCount();
    return 1;
}

/* Has the connect finished, and did it work? 0 pending, 1 up, -1 failed. */
static int connect_status(CTransport *t)
{
    struct timeval tv;
    fd_set         writable, failed;
    int            n, err;
    int            len = sizeof(err);

    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(t->sock, &writable);
    FD_SET(t->sock, &failed);
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    n = select(0, NULL, &writable, &failed, &tv);
    if (n == SOCKET_ERROR) {
        t->lastError = WSAGetLastError();
        return -1;
    }
    if (n == 0) {
        if (GetTickCount() - t->startedAt > CT_CONNECT_TIMEOUT_MS) {
            t->lastError = WSAETIMEDOUT;
            return -1;
        }
        return 0;
    }

    if (FD_ISSET(t->sock, &failed)) {
        if (getsockopt(t->sock, SOL_SOCKET, SO_ERROR, (char *)&err, &len) == 0)
            t->lastError = err;
        return -1;
    }

    /*
     * Writable means the connect finished, but not that it succeeded -- some
     * stacks report a refusal that way rather than through the exception set.
     * SO_ERROR is the one answer that distinguishes them.
     */
    if (getsockopt(t->sock, SOL_SOCKET, SO_ERROR, (char *)&err, &len) == 0 &&
        err != 0) {
        t->lastError = err;
        return -1;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Interface                                                           */
/* ------------------------------------------------------------------ */

CTransport *ct_transport_create(const char *host, uint16_t port)
{
    CTransport *t;

    if (host == NULL || host[0] == '\0') return NULL;
    if (!winsock_start()) return NULL;

    t = (CTransport *)calloc(1, sizeof(*t));
    if (t == NULL) return NULL;

    t->sock = INVALID_SOCKET;
    t->port = port;
    if (strlen(host) >= sizeof(t->host)) {
        free(t);
        return NULL;
    }
    strcpy(t->host, host);

    if (!start_resolve(t)) {
        t->state = kCTransport_Error;
        return t;
    }
    t->state     = kCTransport_ResolvingDNS;
    t->startedAt = GetTickCount();
    return t;
}

CTransport *ct_transport_adopt(CTSocket sock)
{
    CTransport   *t;
    unsigned long nonblocking = 1;

    if (sock == CT_SOCKET_NONE) return NULL;
    if (!winsock_start()) return NULL;

    t = (CTransport *)calloc(1, sizeof(*t));
    if (t == NULL) return NULL;

    t->sock  = (SOCKET)sock;
    t->state = kCTransport_Connected;
    ioctlsocket(t->sock, FIONBIO, &nonblocking);
    return t;
}

CTransportState ct_transport_pump(CTransport *t)
{
    if (t == NULL) return kCTransport_Error;

    switch (t->state) {
    case kCTransport_ResolvingDNS:
        if (t->dnsDone == 0) {
            if (GetTickCount() - t->startedAt > CT_CONNECT_TIMEOUT_MS) {
                t->lastError = WSAETIMEDOUT;
                t->state = kCTransport_Error;
            }
            break;
        }
        if (t->dnsDone < 0) {
            t->lastError = t->dnsError;
            t->state = kCTransport_Error;
            break;
        }
        if (!begin_connect(t)) {
            t->state = kCTransport_Error;
            break;
        }
        t->state = kCTransport_Connecting;
        break;

    case kCTransport_Connecting: {
        int r = connect_status(t);

        if (r > 0)      t->state = kCTransport_Connected;
        else if (r < 0) t->state = kCTransport_Error;
        break;
    }

    default:
        break;
    }
    return t->state;
}

int ct_transport_send(CTransport *t, const void *buf, size_t len)
{
    int n;

    if (t == NULL || t->sock == INVALID_SOCKET) return -1;
    if (len == 0) return 0;

    n = send(t->sock, (const char *)buf, (int)len, 0);
    if (n == SOCKET_ERROR) {
        int err = WSAGetLastError();

        /* Not an error: the send buffer is full, try the next slice. */
        if (err == WSAEWOULDBLOCK) return 0;
        t->lastError = err;
        return -1;
    }
    return n;
}

int ct_transport_recv(CTransport *t, void *buf, size_t len)
{
    int n;

    if (t == NULL || t->sock == INVALID_SOCKET) return -1;
    if (len == 0) return 0;

    n = recv(t->sock, (char *)buf, (int)len, 0);
    if (n == 0) {
        /*
         * The peer closed its side. The interface reports that through
         * ct_transport_peer_closed(), not through this return value, so the
         * TLS core sees "nothing available" here and asks separately whether
         * anything more is coming.
         */
        t->peerClosed = 1;
        return 0;
    }
    if (n == SOCKET_ERROR) {
        int err = WSAGetLastError();

        if (err == WSAEWOULDBLOCK) return 0;
        if (err == WSAECONNRESET || err == WSAECONNABORTED) {
            t->peerClosed = 1;
            t->lastError = err;
            return 0;
        }
        t->lastError = err;
        return -1;
    }
    return n;
}

void ct_transport_close(CTransport *t)
{
    if (t == NULL || t->sock == INVALID_SOCKET) return;

    shutdown(t->sock, 1 /* SD_SEND */);
    t->state = kCTransport_Closing;
}

void ct_transport_destroy(CTransport *t)
{
    if (t == NULL) return;

    /*
     * A resolver thread still running holds a pointer to this struct, so it
     * has to finish before the memory goes back. gethostbyname always returns
     * eventually, and a connection being torn down mid-lookup is rare enough
     * that waiting is cheaper than the machinery to avoid it.
     */
    if (t->dnsThread != NULL) {
        WaitForSingleObject(t->dnsThread, INFINITE);
        CloseHandle(t->dnsThread);
    }

    if (t->sock != INVALID_SOCKET) closesocket(t->sock);
    free(t);
}

void ct_socket_close(CTSocket sock)
{
    if (sock != CT_SOCKET_NONE) closesocket((SOCKET)sock);
}

/* ------------------------------------------------------------------ */
/* Accessors                                                           */
/* ------------------------------------------------------------------ */

CTransportState ct_transport_state(const CTransport *t)
{
    return (t == NULL) ? kCTransport_Error : t->state;
}

int ct_transport_peer_closed(const CTransport *t)
{
    return (t == NULL) ? 1 : t->peerClosed;
}

uint16_t ct_transport_port(const CTransport *t)
{
    return (t == NULL) ? 0 : t->port;
}

long ct_transport_last_error(const CTransport *t)
{
    return (t == NULL) ? 0 : t->lastError;
}

uint32_t ct_transport_peer_ipv4(const CTransport *t)
{
    return (t == NULL) ? 0 : t->addr;
}
