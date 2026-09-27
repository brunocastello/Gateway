/*
 * gw_tunnel.c - the generic TLS tunnel. See gw_tunnel.h.
 *
 * One session is: accept plaintext locally, open the far leg (directly to
 * the remote, or to a forward proxy and through it), upgrade that leg to TLS
 * when tunnel_tls asks for it, then splice bytes until either side goes away.
 *
 * The proxy handshake runs on the raw GWConn before Certainly ever sees the
 * socket: on success the connection is adopted into a GWStream and upgraded
 * with GWStream_UpgradeToTLS(), the same detach-and-hand-over the mail
 * module uses for STARTTLS. After the handoff leftover handshake bytes must
 * not exist, because Certainly reads from the socket, not from our buffers --
 * so a 200 with trailing bytes is a failure, not a head start.
 *
 * tunnel_sni changes only what the ClientHello's SNI extension carries; the
 * certificate is always checked against remoteHost (PATCHES.md §34), never
 * against whatever name was sent, unless tunnel_insecure has turned
 * validation off entirely.
 */

#include "gw_tunnel.h"

#include <stdio.h>
#include <string.h>

#include "../gw_config.h"
#include "../portable/gw_b64.h"
#include "../portable/gw_fwd.h"
#include "../portable/gw_log.h"
#include "../portable/gw_util.h"

#define GW_TUNNEL_BUF   4096L
#define GW_TUNNEL_TX    1024
#define GW_TUNNEL_RX    1024
#define GW_TUNNEL_USER  128
/* 45 seconds without progress before the far leg is given up on. After the
 * splice is up there is no idle timeout at all: an SSH session sits quiet
 * for hours and that is not death. EOF and peer-gone reap those. */
#define GW_TUNNEL_HANDSHAKE (45 * 60)        /* ticks */

typedef enum {
    kTNLFree = 0,
    kTNLProxyConnect,   /* GWConn opening, to the proxy or the remote */
    kTNLProxyHello,     /* proxy handshake bytes on the wire */
    kTNLSettleWait,     /* proxy accepted; waiting out its setup race */
    kTNLSpliceWait,     /* TLS handshake running on the adopted stream */
    kTNLSplice,         /* bytes both ways until EOF or error */
    kTNLFlushClose,
    kTNLDone
} GWTunnelState;

typedef struct {
    GWTunnelState state;
    long          id;

    GWStream      cli;          /* the local application, always plaintext */
    GWStream      up;           /* the far leg, after the handshake */
    GWConn       *conn;         /* the far leg, during the handshake */

    int           proxyKind;
    int           useTls;
    char          remoteHost[GW_NET_HOST_MAX];
    UInt16        remotePort;

    char          tx[GW_TUNNEL_TX];     /* handshake bytes still to send */
    size_t        txLen, txSent;
    /*
     * Split offset for the HTTP CONNECT request: the request line goes out
     * first and the headers follow on the next pump -- socat's exact send
     * pattern, which is what one BlueCoat-style proxy actually answers. 0
     * disables the split (SOCKS, or a request that is the line alone).
     */
    size_t        txSplit;
    unsigned char rx[GW_TUNNEL_RX];     /* handshake bytes received */
    size_t        rxLen;
    int           socksPhase;           /* 0 greeting, 1 connection request */

    char         *cbuf;  size_t cLen;   /* from client   */
    char         *ubuf;  size_t uLen;   /* from far leg  */
    char         *oq;    size_t oLen, oSent; /* to client  */
    char         *pq;    size_t pLen, pSent; /* to far leg */

    unsigned long lastActivity;
    unsigned long settleUntil;  /* ticks: when kTNLSettleWait may proceed */
} GWTunnelSession;

static GWTunnelSession *sTunnel;
static long             sNextId;

/* ------------------------------------------------------------------ */
/* Buffer plumbing (the same push/flush/fill as the mail splice)       */
/* ------------------------------------------------------------------ */

static int q_push(char *q, size_t cap, size_t *len, size_t *sent,
                  const void *data, size_t n)
{
    if (*sent > 0) {
        memmove(q, q + *sent, *len - *sent);
        *len -= *sent;
        *sent = 0;
    }
    if (*len + n > cap) return 0;
    memcpy(q + *len, data, n);
    *len += n;
    return 1;
}

/* 1: drained, 0: partially written, -1: the peer is gone. */
static int q_flush(GWStream *s, char *q, size_t *len, size_t *sent)
{
    while (*sent < *len) {
        long n = GWStream_Write(s, q + *sent, *len - *sent);
        if (n < 0) return -1;
        if (n == 0) return 0;
        *sent += (size_t)n;
    }
    *len = 0;
    *sent = 0;
    return 1;
}

/* Read from a stream into a staging buffer. Returns the GWStream_Read code. */
static long fill(GWStream *s, char *buf, size_t *len, size_t cap)
{
    long n;

    if (*len >= cap) return 0;
    n = GWStream_Read(s, buf + *len, cap - *len);
    if (n > 0) *len += (size_t)n;
    return n;
}

/* Raw-conn versions for the handshake, before any stream exists. */
static long conn_flush(GWTunnelSession *s)
{
    size_t cap = s->txLen;

    /*
     * First flight is the request line alone; the rest follows on the next
     * pump, normally its own TCP segment -- socat's shape, which one proxy
     * treats differently from a single write carrying the whole head. Costs
     * one extra pump (about a millisecond).
     */
    if (s->txSplit > 0 && s->txSent < s->txSplit)
        cap = s->txSplit;

    while (s->txSent < cap) {
        long n = GWConn_Send(s->conn, s->tx + s->txSent,
                             cap - s->txSent);
        if (n < 0) return -1;
        if (n == 0) return 0;
        s->txSent += (size_t)n;
        s->lastActivity = GWNet_Ticks();
    }
    if (cap < s->txLen) return 0;       /* remainder goes next pump */
    return 1;
}

static long conn_fill(GWTunnelSession *s)
{
    long n;

    if (s->rxLen >= sizeof(s->rx)) return 0;
    n = GWConn_Recv(s->conn, s->rx + s->rxLen, sizeof(s->rx) - s->rxLen);
    if (n > 0) {
        s->rxLen += (size_t)n;
        s->lastActivity = GWNet_Ticks();
    }
    return n;
}

static void session_reset(GWTunnelSession *s)
{
    if (s->conn != NULL) GWConn_Destroy(s->conn);
    GWStream_Destroy(&s->cli);
    GWStream_Destroy(&s->up);
    if (s->cbuf != NULL) DisposePtr((Ptr)s->cbuf);
    if (s->ubuf != NULL) DisposePtr((Ptr)s->ubuf);
    if (s->oq != NULL)   DisposePtr((Ptr)s->oq);
    if (s->pq != NULL)   DisposePtr((Ptr)s->pq);
    memset(s, 0, sizeof(*s));
    s->state = kTNLFree;
}

static void tunnel_fail(GWTunnelSession *s, const char *reason)
{
    gw_log("tunnel #%ld %s", s->id, reason);
    if (s->conn != NULL) {
        GWConn_Destroy(s->conn);
        s->conn = NULL;
    }
    GWStream_Destroy(&s->up);
    s->state = kTNLDone;
}

/* ------------------------------------------------------------------ */
/* Setup: snapshot the prefs into the session and open the far leg     */
/* ------------------------------------------------------------------ */

/* Shape the first handshake bytes. Returns 0 with the reason logged. */
static int build_handshake(GWTunnelSession *s)
{
    if (s->proxyKind == GW_FWD_HTTP) {
        const char *user = GWConfig_Str("tunnel_proxy_user", "");
        const char *pass = GWConfig_Str("tunnel_proxy_pass", "");
        char        b64[256];
        const char *auth = NULL;
        int         bare;

        b64[0] = '\0';
        if (user[0] != '\0') {
            /*
             * Private copies: GWConfig_Str hands back a rotating set of
             * slots, so user and pass must be captured before either is
             * used -- the second lookup would otherwise be free to reuse
             * the first one's buffer.
             */
            char userCopy[GW_TUNNEL_USER], passCopy[GW_TUNNEL_USER];
            char creds[GW_TUNNEL_USER * 2 + 1];

            gw_copy_n(userCopy, sizeof(userCopy), user, strlen(user));
            gw_copy_n(passCopy, sizeof(passCopy), pass, strlen(pass));
            if (strlen(userCopy) + 1 + strlen(passCopy) >= sizeof(creds)) {
                gw_log("tunnel #%ld proxy credentials too long", s->id);
                return 0;
            }
            snprintf(creds, sizeof(creds), "%s:%s", userCopy, passCopy);
            if (gw_b64_encode(creds, strlen(creds), b64, sizeof(b64)) == 0) {
                gw_log("tunnel #%ld cannot encode proxy credentials", s->id);
                return 0;
            }
            auth = b64;
        }

        /*
         * Bare (no Host line) when tunnel_host_header is 0: request line,
         * optional Proxy-Authorization, blank line -- exactly what socat
         * sends. HTTP/1.0 does not require Host; the authority is already
         * in the request line, and one proxy answered the Host-carrying
         * form with 200 and then stalled the tunnel past the handshake
         * timeout while passing the bare form. Default keeps Host.
         */
        bare = (GWConfig_Num("tunnel_host_header", 1) == 0);
        if (!bare)
            s->txLen = gw_fwd_connect_req(s->remoteHost, s->remotePort, auth,
                                          s->tx, sizeof(s->tx));
        else
            s->txLen = gw_fwd_connect_req_bare(s->remoteHost, s->remotePort,
                                               auth, s->tx, sizeof(s->tx));
        if (s->txLen == 0) {
            gw_log("tunnel #%ld cannot shape CONNECT request", s->id);
            return 0;
        }
        /*
         * The request line only in the log: credentials never reach it.
         * Names which form actually went out, since the wire otherwise
         * looks identical from the log alone.
         */
        {
            size_t eol = 0;

            while (eol < s->txLen && s->tx[eol] != '\r' && s->tx[eol] != '\n')
                eol++;
            gw_log("tunnel #%ld sending %.*s%s%s", s->id, (int)eol, s->tx,
                   bare ? " (bare)" : "", auth ? " +auth" : "");
            /* Request line first, headers next pump (see conn_flush()). */
            if (eol + 2 < s->txLen) s->txSplit = eol + 2;
        }
    } else if (s->proxyKind == GW_FWD_SOCKS5) {
        /*
         * No-auth only. RFC 1929 user/pass would be twenty more lines;
         * until they exist a configured login fails loudly rather than
         * connecting anonymously, which the log could not tell apart from
         * a proxy that simply does not ask.
         */
        if (GWConfig_Str("tunnel_proxy_user", "")[0] != '\0') {
            gw_log("tunnel #%ld SOCKS5 username/password is not implemented",
                   s->id);
            return 0;
        }
        s->txLen = 0;
        {
            unsigned char *tx = (unsigned char *)s->tx;
            size_t n = gw_fwd_socks_greet(tx, sizeof(s->tx));
            if (n == 0) {
                gw_log("tunnel #%ld cannot shape SOCKS greeting", s->id);
                return 0;
            }
            s->txLen = n;
        }
        s->socksPhase = 0;
    }
    s->txSent = 0;
    return 1;
}

static void begin_far_leg(GWTunnelSession *s)
{
    const char *host;
    long        port;
    char        dialHost[GW_NET_HOST_MAX];

    if (s->proxyKind == GW_FWD_NONE) {
        host = s->remoteHost;
        port = s->remotePort;
    } else {
        host = GWConfig_Str("tunnel_proxy_host", "");
        port = GWConfig_Num("tunnel_proxy_port",
                            s->proxyKind == GW_FWD_SOCKS5 ? 1080 : 8080);
        if (host[0] == '\0') {
            gw_log("tunnel #%ld no tunnel_proxy_host in prefs", s->id);
            s->state = kTNLDone;
            return;
        }
        if (port <= 0 || port > 65535) {
            gw_log("tunnel #%ld bad tunnel_proxy_port", s->id);
            s->state = kTNLDone;
            return;
        }
    }

    /* Copy before connecting: the resolver reads the name asynchronously
     * off the GWConn, and the config slot it came in may be reused. */
    gw_copy_n(dialHost, sizeof(dialHost), host, strlen(host));

    s->conn = GWConn_Connect(dialHost, (UInt16)port);
    if (s->conn == NULL) {
        gw_log("tunnel #%ld cannot start connect to %s:%ld", s->id,
               dialHost, port);
        s->state = kTNLDone;
        return;
    }
    gw_log("tunnel #%ld connecting to %s:%ld%s", s->id, dialHost, port,
           s->proxyKind == GW_FWD_NONE ? (s->useTls ? " (TLS)" : " (plain)")
                                       : " (proxy)");
    s->lastActivity = GWNet_Ticks();
    s->state = kTNLProxyConnect;
}

/* The proxy has done its part (or there was none): adopt the socket and
 * either upgrade it to TLS or splice it as it stands. */
static void begin_tls_or_splice(GWTunnelSession *s)
{
    GWStream_Adopt(&s->up, s->conn);
    s->conn = NULL;             /* Adopt owns it now, either way */
    if (s->up.state != kGWStreamReady) {
        tunnel_fail(s, "far leg broke before the handoff");
        return;
    }
    if (!s->useTls) {
        gw_log("tunnel #%ld plain relay to %s:%u", s->id,
               s->remoteHost, (unsigned)s->remotePort);
        s->state = kTNLSplice;
        return;
    }
    /*
     * remoteHost is always the real far end, whatever tunnel_sni sends on
     * the wire: PATCHES.md §34 keeps the two decoupled inside Certainly, so
     * this call authenticates the actual host exactly as a direct dial
     * would, even though a proxy moved the bytes.
     *
     * tunnel_tls12 forces the 1.2 engine from the first handshake bytes,
     * for far ends with no TLS 1.3 (an old stunnel): the 1.3 ClientHello
     * would only buy a fallback that an adopted connection cannot perform
     * (PATCHES.md §29). The success line in kTNLSpliceWait still names the
     * version that actually negotiated.
     */
    if (GWConfig_Num("tunnel_tls12", 0) != 0) {
        if (!GWStream_UpgradeToTLS12(&s->up, s->remoteHost)) {
            tunnel_fail(s, "cannot start TLS 1.2 on the far leg");
            return;
        }
    } else if (!GWStream_UpgradeToTLS(&s->up, s->remoteHost)) {
        tunnel_fail(s, "cannot start TLS on the far leg");
        return;
    }
    /*
     * Testing only: skip certificate validation on the far leg entirely.
     * Loud on purpose -- with this on, anyone between Gateway and the far
     * end can read the tunnel, and nothing will say so again. Tunnel only;
     * mail never takes this path whatever the prefs say.
     */
    if (GWConfig_Num("tunnel_insecure", 0) != 0) {
        GWStream_SetInsecure(&s->up);
        gw_log("tunnel #%ld WARNING: TLS certificate validation DISABLED "
               "(tunnel_insecure)", s->id);
    }
    /*
     * Diagnosis for SNI-policing middleboxes: socat's handshake carries no
     * SNI where Gateway's carries the far hostname, and one corporate proxy
     * answered only the former. This changes only what goes out on the
     * wire -- validation always checks remoteHost regardless (§34).
     */
    {
        const char *want = GWConfig_Str("tunnel_sni", "");
        char sni[GW_NET_HOST_MAX];

        if (want[0] != '\0') {
            gw_copy_n(sni, sizeof(sni), want, strlen(want));
            if (gw_stricmp(sni, "none") == 0 || strcmp(sni, "-") == 0) {
                GWStream_SetSNI(&s->up, NULL);
                gw_log("tunnel #%ld SNI omitted (tunnel_sni)", s->id);
            } else {
                GWStream_SetSNI(&s->up, sni);
                gw_log("tunnel #%ld SNI %s (tunnel_sni)", s->id, sni);
            }
        }
    }
    s->lastActivity = GWNet_Ticks();
    s->state = kTNLSpliceWait;
}

/* ------------------------------------------------------------------ */
/* Handshake steps                                                     */
/* ------------------------------------------------------------------ */

static void proxy_ready(GWTunnelSession *s);

static void step_proxy_connect(GWTunnelSession *s)
{
    switch (GWConn_Pump(s->conn)) {
    case kGWConnReady:
        if (s->proxyKind == GW_FWD_NONE) {
            begin_tls_or_splice(s);
        } else {
            s->rxLen = 0;
            s->state = kTNLProxyHello;
        }
        break;
    case kGWConnError:
    case kGWConnClosed: {
        char peer[64];
        GWConn_PeerText(s->conn, peer, sizeof(peer));
        gw_log("tunnel #%ld connect failed to %s (OT %ld)", s->id, peer,
               GWConn_LastError(s->conn));
        GWConn_Destroy(s->conn);
        s->conn = NULL;
        s->state = kTNLDone;
        break;
    }
    default:
        break;
    }
}

static void step_http_hello(GWTunnelSession *s)
{
    size_t head_len = 0;
    long   code = 0;
    int    r;

    if (conn_flush(s) < 0) {
        tunnel_fail(s, "proxy broke while sending CONNECT");
        return;
    }
    if (s->txSent < s->txLen) return;       /* flow controlled: next slice */

    r = (int)conn_fill(s);
    if (r == -1) {
        tunnel_fail(s, "proxy broke during CONNECT reply");
        return;
    }
    if (r == -2) {
        tunnel_fail(s, "proxy closed the connection during CONNECT");
        return;
    }

    switch (gw_fwd_connect_reply((const char *)s->rx, s->rxLen,
                                 &head_len, &code)) {
    case 0:
        if (s->rxLen >= sizeof(s->rx)) {
            gw_log("tunnel #%ld proxy reply too long (%u bytes, no head)",
                   s->id, (unsigned)s->rxLen);
            tunnel_fail(s, "proxy reply overflowed the buffer");
        }
        return;
    case 1:
        break;
    default:
        tunnel_fail(s, "proxy reply is not an HTTP status line");
        return;
    }

    if (code != 200) {
        gw_log("tunnel #%ld proxy refused CONNECT (%ld)%s", s->id, code,
               code == 407 ? ": proxy authentication required" : "");
        tunnel_fail(s, "proxy refused the CONNECT request");
        return;
    }
    if (s->rxLen != head_len) {
        /*
         * Anything past the head would be the first TLS bytes, and Certainly
         * reads from the socket rather than from this buffer -- handing over
         * now would silently drop them. No proxy in practice pipelines here,
         * so refuse rather than corrupt.
         */
        tunnel_fail(s, "proxy sent bytes past its CONNECT reply");
        return;
    }
    {
        /* Name the reply beyond its status: chained proxies (Via) tell
         * apart backends that share one address, and a 200 that differs
         * between two clients is the whole diagnosis when one stalls. */
        size_t viaLen = 0, eol = 0;
        const char *via;
        char status[80], node[80];

        while (eol < head_len && s->rx[eol] != '\r' && s->rx[eol] != '\n')
            eol++;
        gw_copy_n(status, sizeof(status), (const char *)s->rx, eol);
        via = gw_header_find((const char *)s->rx, head_len, "Via", &viaLen);
        if (via != NULL && viaLen > 0) {
            gw_copy_n(node, sizeof(node), via, viaLen);
            gw_log("tunnel #%ld proxy said: %s (Via: %s)", s->id, status,
                   node);
        } else {
            gw_log("tunnel #%ld proxy said: %s", s->id, status);
        }
    }
    proxy_ready(s);
}

static void step_socks_hello(GWTunnelSession *s)
{
    int r;

    if (conn_flush(s) < 0) {
        tunnel_fail(s, "proxy broke while sending SOCKS request");
        return;
    }
    if (s->txSent < s->txLen) return;

    r = (int)conn_fill(s);
    if (r == -1) {
        tunnel_fail(s, "proxy broke during SOCKS reply");
        return;
    }
    if (r == -2) {
        tunnel_fail(s, "proxy closed the connection during SOCKS");
        return;
    }

    if (s->socksPhase == 0) {
        switch (gw_fwd_socks_greet_reply(s->rx, s->rxLen)) {
        case 0: return;         /* still arriving */
        case 1: break;
        default:
            tunnel_fail(s, "SOCKS proxy refused no-auth (needs login?)");
            return;
        }
        {
            size_t n = gw_fwd_socks_connect(s->remoteHost, s->remotePort,
                                            (unsigned char *)s->tx,
                                            sizeof(s->tx));
            if (n == 0) {
                tunnel_fail(s, "cannot shape SOCKS connect request");
                return;
            }
            s->txLen = n;
            s->txSent = 0;
            s->txSplit = 0;
            s->rxLen = 0;
            s->socksPhase = 1;
        }
        return;
    }

    switch (gw_fwd_socks_conn_reply(s->rx, s->rxLen)) {
    case 0: return;
    case 1: break;
    default:
        gw_log("tunnel #%ld SOCKS proxy refused %s:%u (REP %u)", s->id,
               s->remoteHost, (unsigned)s->remotePort,
               s->rxLen >= 2 ? (unsigned)s->rx[1] : 99);
        tunnel_fail(s, "SOCKS proxy refused the connection");
        return;
    }
    gw_log("tunnel #%ld SOCKS proxy to %s:%u established", s->id,
           s->remoteHost, (unsigned)s->remotePort);
    proxy_ready(s);
}

/*
 * The proxy handshake is done; the tunnel is supposedly live. Either start
 * TLS at once or let the tunnel settle first: one proxy answers 200 before
 * its upstream splice is ready, and the first flight sent inside a
 * millisecond falls into the void with no RST and no reply -- 30 seconds of
 * silence that reads as a TLS failure. Waiting past the race once per
 * connection costs nothing next to the handshake itself.
 */
static void proxy_ready(GWTunnelSession *s)
{
    long ms = GWConfig_Num("tunnel_settle_ms", 0);

    if (ms < 0) ms = 0;
    if (ms > 30000) ms = 30000;
    if (s->proxyKind != GW_FWD_NONE && ms > 0) {
        s->settleUntil = GWNet_Ticks() + (unsigned long)(ms * 60 / 1000);
        s->lastActivity = GWNet_Ticks();
        gw_log("tunnel #%ld letting the tunnel settle %ldms", s->id, ms);
        s->state = kTNLSettleWait;
        return;
    }
    begin_tls_or_splice(s);
}

/* ------------------------------------------------------------------ */
/* Splice                                                              */
/* ------------------------------------------------------------------ */

static void step_splice(GWTunnelSession *s)
{
    long n;

    /* client -> far leg */
    if (s->pLen == s->pSent) {
        if (s->cLen > 0) {
            q_push(s->pq, (size_t)GW_TUNNEL_BUF, &s->pLen, &s->pSent,
                   s->cbuf, s->cLen);
            s->cLen = 0;
            s->lastActivity = GWNet_Ticks();
        } else {
            n = fill(&s->cli, s->cbuf, &s->cLen, (size_t)GW_TUNNEL_BUF);
            if (n > 0) {
                q_push(s->pq, (size_t)GW_TUNNEL_BUF, &s->pLen, &s->pSent,
                       s->cbuf, s->cLen);
                s->cLen = 0;
                s->lastActivity = GWNet_Ticks();
            } else if (n == -2) {
                GWStream_Close(&s->up);
            } else if (n == -1) {
                s->state = kTNLDone;
                return;
            }
        }
    }
    if (q_flush(&s->up, s->pq, &s->pLen, &s->pSent) < 0) {
        s->state = kTNLFlushClose;
        return;
    }

    /* far leg -> client */
    if (s->oLen == s->oSent) {
        if (s->uLen > 0) {
            q_push(s->oq, (size_t)GW_TUNNEL_BUF, &s->oLen, &s->oSent,
                   s->ubuf, s->uLen);
            s->uLen = 0;
            s->lastActivity = GWNet_Ticks();
        } else {
            n = fill(&s->up, s->ubuf, &s->uLen, (size_t)GW_TUNNEL_BUF);
            if (n > 0) {
                q_push(s->oq, (size_t)GW_TUNNEL_BUF, &s->oLen, &s->oSent,
                       s->ubuf, s->uLen);
                s->uLen = 0;
                s->lastActivity = GWNet_Ticks();
            } else if (n == -2 || n == -1) {
                s->state = kTNLFlushClose;
            }
        }
    }
    if (q_flush(&s->cli, s->oq, &s->oLen, &s->oSent) < 0) s->state = kTNLDone;
}

/* ------------------------------------------------------------------ */

static void session_step(GWTunnelSession *s)
{
    if (s->state == kTNLFree) return;

    /* A handshake that makes no progress is a dead proxy, not a slow one.
     * The splice itself is exempt: quiet is normal there. */
    if (s->state != kTNLSplice &&
        GWNet_Ticks() - s->lastActivity > GW_TUNNEL_HANDSHAKE) {
        gw_log("tunnel #%ld handshake timed out", s->id);
        tunnel_fail(s, "handshake timed out");
    }

    GWStream_Pump(&s->cli);
    if (s->up.state != kGWStreamIdle) GWStream_Pump(&s->up);
    if (s->cli.state == kGWStreamError) {
        s->state = kTNLDone;
    }

    switch (s->state) {
    case kTNLProxyConnect:
        step_proxy_connect(s);
        break;

    case kTNLProxyHello:
        if (s->proxyKind == GW_FWD_HTTP) step_http_hello(s);
        else step_socks_hello(s);
        break;

    case kTNLSettleWait:
        /* Signed compare so a tick-counter wrap still ends the wait. */
        if ((long)(GWNet_Ticks() - s->settleUntil) >= 0) {
            s->lastActivity = GWNet_Ticks();
            begin_tls_or_splice(s);
        }
        break;

    case kTNLSpliceWait:
        if (s->up.state == kGWStreamReady) {
            int ver = GWStream_TlsVersion(&s->up);
            gw_log("tunnel #%ld %s to %s:%u", s->id,
                   ver == 13 ? "TLS 1.3" : ver == 12 ? "TLS 1.2" : "TLS",
                   s->remoteHost, (unsigned)s->remotePort);
            s->state = kTNLSplice;
        } else if (s->up.state == kGWStreamError ||
                   s->up.state == kGWStreamClosed) {
            char why[160];
            gw_log("tunnel #%ld %s", s->id,
                   GWStream_Describe(&s->up, why, sizeof(why)));
            /*
             * The far end speaks only TLS 1.2, and falling back needs a
             * fresh connection the tunnel cannot re-open past the proxy
             * (or would defeat the point of an adopted socket). Say so with
             * the remedy: the diagnostics above carry no code, no address
             * and no version, and "handshake failed" alone sends the
             * reader to the proxy, which already did its part.
             */
            if (GWStream_FallbackNoRoute(&s->up))
                gw_log("tunnel #%ld %s:%u chose TLS 1.2 -- enable TLS 1.3 "
                       "on the far end, or set tunnel_tls12 = 1", s->id,
                       s->remoteHost, (unsigned)s->remotePort);
            tunnel_fail(s, "TLS handshake with the far leg failed");
        }
        break;

    case kTNLSplice:
        step_splice(s);
        break;

    case kTNLFlushClose: {
        int r = q_flush(&s->cli, s->oq, &s->oLen, &s->oSent);
        if (r != 0) s->state = kTNLDone;
        break;
    }

    default:
        break;
    }

    if (s->state == kTNLDone) {
        GWStream_Close(&s->cli);
        session_reset(s);
    }
}

/* ------------------------------------------------------------------ */

void GWTunnel_Init(void)
{
    if (sTunnel != NULL) return;
    sTunnel = (GWTunnelSession *)NewPtrClear(
        (Size)(sizeof(GWTunnelSession) * GW_MAX_TUNNEL_SESSIONS));
}

void GWTunnel_Shutdown(void)
{
    int i;

    if (sTunnel == NULL) return;
    for (i = 0; i < GW_MAX_TUNNEL_SESSIONS; i++)
        if (sTunnel[i].state != kTNLFree) session_reset(&sTunnel[i]);
    DisposePtr((Ptr)sTunnel);
    sTunnel = NULL;
}

int GWTunnel_Accept(GWConn *c)
{
    int        i;
    const char *remote;
    long       remotePort;
    int        kind;

    if (sTunnel == NULL) return 0;

    for (i = 0; i < GW_MAX_TUNNEL_SESSIONS; i++) {
        GWTunnelSession *s = &sTunnel[i];
        if (s->state != kTNLFree) continue;

        memset(s, 0, sizeof(*s));
        s->cbuf = NewPtr(GW_TUNNEL_BUF);
        s->ubuf = NewPtr(GW_TUNNEL_BUF);
        s->oq   = NewPtr(GW_TUNNEL_BUF);
        s->pq   = NewPtr(GW_TUNNEL_BUF);
        if (s->cbuf == NULL || s->ubuf == NULL ||
            s->oq == NULL || s->pq == NULL) {
            session_reset(s);
            gw_log("out of memory accepting a tunnel connection");
            return 0;
        }

        remote = GWConfig_Str("tunnel_remote_host", "");
        remotePort = GWConfig_Num("tunnel_remote_port", 443);
        kind = gw_fwd_kind(GWConfig_Str("tunnel_proxy", "none"));
        s->id = ++sNextId;
        if (remote[0] == '\0') {
            gw_log("tunnel: no tunnel_remote_host in prefs; dropping a client");
            session_reset(s);
            return 0;
        }
        if (remotePort <= 0 || remotePort > 65535) {
            gw_log("tunnel: bad tunnel_remote_port; dropping a client");
            session_reset(s);
            return 0;
        }
        if (kind < 0) {
            gw_log("tunnel: unknown tunnel_proxy (want none, http or socks5)");
            session_reset(s);
            return 0;
        }
        /* Copy now: the config slots rotate and the handshake outlives them. */
        gw_copy_n(s->remoteHost, sizeof(s->remoteHost), remote, strlen(remote));
        s->remotePort = (UInt16)remotePort;
        s->proxyKind = kind;
        s->useTls = GWConfig_Num("tunnel_tls", 1) != 0;

        if (!build_handshake(s)) {
            session_reset(s);
            return 0;
        }
        GWStream_Adopt(&s->cli, c);
        s->lastActivity = GWNet_Ticks();
        gw_log("tunnel #%ld client accepted for %s:%u%s%s", s->id,
               s->remoteHost, (unsigned)s->remotePort,
               s->useTls ? " via TLS" : " plain",
               s->proxyKind == GW_FWD_NONE ? "" :
               s->proxyKind == GW_FWD_HTTP ? " via HTTP proxy" : " via SOCKS5");
        begin_far_leg(s);
        return 1;
    }
    return 0;
}

int GWTunnel_CanAccept(void)
{
    int i;

    if (sTunnel == NULL) return 0;
    for (i = 0; i < GW_MAX_TUNNEL_SESSIONS; i++)
        if (sTunnel[i].state == kTNLFree) return 1;
    return 0;
}

void GWTunnel_Poll(void)
{
    int i;

    if (sTunnel == NULL) return;
    for (i = 0; i < GW_MAX_TUNNEL_SESSIONS; i++) session_step(&sTunnel[i]);
}

int GWTunnel_ActiveCount(void)
{
    int i, n = 0;

    if (sTunnel == NULL) return 0;
    for (i = 0; i < GW_MAX_TUNNEL_SESSIONS; i++)
        if (sTunnel[i].state != kTNLFree) n++;
    return n;
}
