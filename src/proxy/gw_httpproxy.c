/*
 * gw_httpproxy.c - Module 1.
 *
 * Three request shapes reach :8765 (CLAUDE.md):
 *
 *   GET http://host/path       plain forward proxy
 *   GET https://host/path      Gateway terminates TLS with Certainly
 *   CONNECT host:443           raw bounce, Gateway stays out of the TLS
 *
 * The client hop is always HTTP/1.0 with Connection: close, so the response
 * body is EOF-delimited and a chunked origin has to be decoded on the way
 * through. Bodies stream rather than accumulate - a 2 MiB counter enforces the
 * cap without a 2 MiB buffer, which matters on an 8 MB partition.
 */

#include "gw_httpproxy.h"

#include <string.h>
#include <stdio.h>

#include "../gw_core.h"
#include "../portable/gw_chunked.h"
#include "../portable/gw_http.h"
#include "../portable/gw_log.h"
#include "../portable/gw_url.h"
#include "../portable/gw_util.h"
#include "../portable/gw_wayback.h"

#define GW_HEAD_MAX     16384L
#define GW_RAW_MAX      16384L
#define GW_OUT_MAX      32768L
#define GW_MAX_REDIRECT 5

/*
 * The Internet Archive refuses connections for a few seconds when it is asked
 * for too much at once, which a period page with thirty images does easily.
 * A refusal is not a missing asset, so back off briefly and ask again rather
 * than handing the browser a broken image.
 */
#define GW_WB_RETRIES   4
/*
 * Ticks, multiplied by the attempt: 2s, 4s, 6s, 8s. A refusal from the archive
 * is rate limiting rather than a fault, so backing off further than the old
 * one second is what actually clears it. With connects serialised the retries
 * should rarely be reached at all.
 */
#define GW_WB_BACKOFF   120

/*
 * How many upstream connections may be *opening* at once.
 *
 * This is the lever that stops the archive refusing us. Eight sessions
 * starting together each wanted their own connection, which is precisely the
 * burst that trips its rate limiter -- and every refusal then became a retry,
 * making the next burst worse.
 *
 * Established connections are not the problem, only new ones, so the cap is on
 * opening rather than on total. Holding it low also gives the pool time to
 * fill: a session that waits a moment usually finds a connection waiting for
 * it instead, and pays nothing at all.
 *
 * It defaults to one. Gateway is a single cooperative thread, so two TLS
 * handshakes running at once do not overlap -- they take turns on the same
 * CPU, and on this hardware the handshake is most of the cost of a request.
 * Opening them one at a time therefore costs almost no wall time while
 * removing the burst entirely. See GW_MaxConnects() for the preference.
 */
#define GW_CONNECT_WAIT  8          /* ticks before looking again */

/*
 * Idle upstream connections, kept for the next request to the same host.
 *
 * This is the difference between a period page loading and not. Every archive
 * fetch goes to web.archive.org, and a TLS handshake on this hardware costs
 * far more than the transfer that follows -- so a page of thirty assets was
 * paying for thirty handshakes, plus thirty more for the snapshot redirects.
 * Reusing one connection collapses that to a handful.
 *
 * The price is exactness: with no closing EOF to mark the end of a response,
 * the body has to be framed by Content-Length or by chunked, and a connection
 * is only kept when it was.
 */
#define GW_POOL_SIZE    6
#define GW_POOL_IDLE    (45 * 60)   /* ticks: drop after 45 seconds idle */
#define GW_IDLE_TIMEOUT (45 * 60)           /* ticks: 45 seconds */

typedef enum {
    kHPFree = 0,
    kHPRecvRequest,
    kHPConnect,
    kHPSendRequest,
    kHPRecvHead,
    kHPBody,
    kHPTunnelConnect,
    kHPTunnel,
    kHPRetryWait,
    kHPConnectWait,
    kHPFlushAndClose,
    kHPDone
} GWHttpState;

typedef struct {
    GWHttpState   state;
    long          id;

    GWStream      cli;
    GWStream      up;

    GWRequest     req;
    GWUrl         target;
    GWUrl         redirectTo;               /* resolved before deciding to follow */
    int           wayback;                  /* arrived on the archive listener */
    GWUrl         waybackOrigin;            /* what the client actually asked for */
    int           retries;
    unsigned long retryAt;

    /* Upstream connection reuse. */
    char          upHost[GW_MAX_HOST];
    UInt16        upPort;
    int           upTls;
    int           upPooled;                 /* came from the pool */
    int           upReusable;               /* framing lets us keep it */
    long          bodyLeft;                 /* -1 when the end is EOF */
    int           redirects;

    char         *chead;                    /* client request head           */
    size_t        cheadLen;
    size_t        cheadSent;                /* tunnel: client -> upstream    */

    char         *uhead;                    /* origin response head          */
    size_t        uheadLen;

    char         *ureq;                     /* rewritten upstream request    */
    size_t        ureqLen, ureqSent;

    char         *raw;                      /* scratch for one read          */
    char         *out;                      /* pending bytes for the client  */
    size_t        outLen, outSent;

    GWChunked     chunk;
    int           chunked;
    long          bodyBytes;
    long          bodyCap;                  /* 0 means no ceiling */
    long          reqBodyLeft;
    int           status;
    unsigned long lastActivity;
} GWHttpSession;

typedef struct {
    GWStream      stream;
    char          host[GW_MAX_HOST];
    UInt16        port;
    int           tls;
    int           live;
    unsigned long idleSince;
} GWUpstreamSlot;

static GWUpstreamSlot sPool[GW_POOL_SIZE];

static GWHttpSession *sSessions;
static int            sSessionCount;
static long           sNextId;

/*
 * How many sessions are part-way through opening a connection, counted
 * separately for archive and live-web traffic. They have different caps and
 * different reasons for them, so a burst of one must not starve the other.
 */
static int connecting_count(int wayback)
{
    int i, n = 0;

    if (sSessions == NULL) return 0;
    for (i = 0; i < sSessionCount; i++)
        if (sSessions[i].state == kHPConnect &&
            sSessions[i].wayback == wayback) n++;
    return n;
}

/* ------------------------------------------------------------------ */
/* Idle upstream connections                                           */
/* ------------------------------------------------------------------ */

static void pool_drop(GWUpstreamSlot *slot)
{
    GWStream_Destroy(&slot->stream);
    slot->live = 0;
}

/* Hand an idle connection back out, if one goes to the same place. */
static int pool_take(const char *host, UInt16 port, int tls, GWStream *out)
{
    int i;

    for (i = 0; i < GW_POOL_SIZE; i++) {
        GWUpstreamSlot *slot = &sPool[i];

        if (!slot->live) continue;
        if (slot->port != port || slot->tls != tls) continue;
        if (gw_stricmp(slot->host, host) != 0) continue;

        /* Only if it is still up: the far end may have closed it since. */
        if (GWStream_Pump(&slot->stream) != kGWStreamReady) {
            pool_drop(slot);
            continue;
        }

        *out = slot->stream;
        GWStream_Init(&slot->stream);       /* ownership moves to the caller */
        slot->live = 0;
        return 1;
    }
    return 0;
}

/* Keep a finished connection for the next request, or let it go. */
static void pool_put(GWStream *s, const char *host, UInt16 port, int tls)
{
    int i;

    if (GWStream_Pump(s) != kGWStreamReady) {
        GWStream_Destroy(s);
        return;
    }

    for (i = 0; i < GW_POOL_SIZE; i++) {
        GWUpstreamSlot *slot = &sPool[i];

        if (slot->live) continue;

        slot->stream = *s;
        GWStream_Init(s);                   /* ownership moves to the pool */
        gw_copy_n(slot->host, sizeof(slot->host), host, strlen(host));
        slot->port = port;
        slot->tls = tls;
        slot->idleSince = GWNet_Ticks();
        slot->live = 1;
        return;
    }

    GWStream_Destroy(s);                    /* pool full */
}

/* Idle connections still need pumping, or a close goes unnoticed until it is
 * handed to a request that then fails. */
static void pool_poll(void)
{
    int i;

    for (i = 0; i < GW_POOL_SIZE; i++) {
        GWUpstreamSlot *slot = &sPool[i];

        if (!slot->live) continue;
        if (GWStream_Pump(&slot->stream) != kGWStreamReady ||
            GWNet_Ticks() - slot->idleSince > GW_POOL_IDLE)
            pool_drop(slot);
    }
}

static void pool_clear(void)
{
    int i;
    for (i = 0; i < GW_POOL_SIZE; i++)
        if (sPool[i].live) pool_drop(&sPool[i]);
}

/* ------------------------------------------------------------------ */

static void session_free_buffers(GWHttpSession *s)
{
    if (s->chead) { DisposePtr((Ptr)s->chead); s->chead = NULL; }
    if (s->uhead) { DisposePtr((Ptr)s->uhead); s->uhead = NULL; }
    if (s->ureq)  { DisposePtr((Ptr)s->ureq);  s->ureq  = NULL; }
    if (s->raw)   { DisposePtr((Ptr)s->raw);   s->raw   = NULL; }
    if (s->out)   { DisposePtr((Ptr)s->out);   s->out   = NULL; }
}

static int session_alloc_buffers(GWHttpSession *s)
{
    s->chead = NewPtr(GW_HEAD_MAX);
    s->uhead = NewPtr(GW_HEAD_MAX);
    s->ureq  = NewPtr(GW_HEAD_MAX);
    s->raw   = NewPtr(GW_RAW_MAX);
    s->out   = NewPtr(GW_OUT_MAX);

    if (s->chead == NULL || s->uhead == NULL || s->ureq == NULL ||
        s->raw == NULL || s->out == NULL) {
        session_free_buffers(s);
        return 0;
    }
    return 1;
}

static void session_reset(GWHttpSession *s)
{
    GWStream_Destroy(&s->cli);
    GWStream_Destroy(&s->up);
    session_free_buffers(s);
    memset(s, 0, sizeof(*s));
    s->state = kHPFree;
}

/* Queue a NUL-terminated status line for the client and stop talking upstream. */
static void session_fail(GWHttpSession *s, const char *statusLine,
                         const char *reason)
{
    /*
     * Spliced into every error Gateway generates itself.
     *
     * Without them the browser caches the failure. A 404 is cacheable by
     * default, and a bare 502 with no freshness information gets a heuristic
     * lifetime, so Internet Explorer would store the error page *as the
     * image* -- and on the next load serve it from disk without asking us at
     * all. That looks exactly like a proxy that has stopped working: one
     * request for the HTML, every image broken, and nothing in the log. A
     * transient refusal from the archive has to stay transient.
     *
     * Pragma and Expires are there for the HTTP/1.0 clients this exists for;
     * Cache-Control alone would not reach them.
     */
    static const char kNoStore[] =
        "Cache-Control: no-store, no-cache, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Expires: 0\r\n";
    const size_t extra = sizeof(kNoStore) - 1;
    const char  *rest;
    size_t       head, tail;

    gw_log("#%ld %s", s->id, reason);

    rest = strstr(statusLine, "\r\n");
    head = (rest != NULL) ? (size_t)(rest - statusLine) + 2 : 0;
    tail = (head > 0) ? strlen(statusLine + head) : strlen(statusLine);

    if (s->out != NULL && s->outLen == 0 && head > 0 &&
        head + extra + tail < (size_t)GW_OUT_MAX) {
        memcpy(s->out, statusLine, head);
        memcpy(s->out + head, kNoStore, extra);
        memcpy(s->out + head + extra, statusLine + head, tail);
        s->outLen = head + extra + tail;
        s->outSent = 0;
        s->state = kHPFlushAndClose;
    } else if (s->out != NULL && s->outLen == 0 && tail < (size_t)GW_OUT_MAX) {
        memcpy(s->out, statusLine, tail);
        s->outLen = tail;
        s->outSent = 0;
        s->state = kHPFlushAndClose;
    } else {
        s->state = kHPDone;
    }
    GWStream_Destroy(&s->up);
}

/* Push whatever is pending to the client. Returns 1 when the buffer drained,
 * 0 when there is more to go, -1 on a dead client. */
static int session_flush(GWHttpSession *s)
{
    while (s->outSent < s->outLen) {
        long n = GWStream_Write(&s->cli, s->out + s->outSent,
                                s->outLen - s->outSent);
        if (n < 0) return -1;
        if (n == 0) return 0;               /* flow controlled */
        s->outSent += (size_t)n;
        s->lastActivity = GWNet_Ticks();
    }
    s->outLen = 0;
    s->outSent = 0;
    return 1;
}

static int session_queue(GWHttpSession *s, const char *data, size_t len)
{
    if (s->outLen - s->outSent + len > (size_t)GW_OUT_MAX) return 0;
    if (s->outSent > 0) {
        memmove(s->out, s->out + s->outSent, s->outLen - s->outSent);
        s->outLen -= s->outSent;
        s->outSent = 0;
    }
    memcpy(s->out + s->outLen, data, len);
    s->outLen += len;
    return 1;
}

/* Answer with something Gateway generated, then close. */
static void session_serve(GWHttpSession *s, const char *type,
                          const char *body, size_t body_len)
{
    int n = snprintf(s->out, (size_t)GW_OUT_MAX,
                     "HTTP/1.0 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lu\r\n"
                     "Connection: close\r\n\r\n",
                     type, (unsigned long)body_len);

    if (n < 0 || (size_t)n + body_len >= (size_t)GW_OUT_MAX) {
        s->state = kHPDone;
        return;
    }
    memcpy(s->out + n, body, body_len);
    s->outLen = (size_t)n + body_len;
    s->outSent = 0;
    s->state = kHPFlushAndClose;
}

static void session_redirect(GWHttpSession *s, const char *url)
{
    int n = snprintf(s->out, (size_t)GW_OUT_MAX,
                     "HTTP/1.0 302 Found\r\n"
                     "Location: %s\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n\r\n", url);

    if (n < 0 || (size_t)n >= (size_t)GW_OUT_MAX) {
        s->state = kHPDone;
        return;
    }
    s->outLen = (size_t)n;
    s->outSent = 0;
    s->state = kHPFlushAndClose;
}

/*
 * The settings page, which is the only way to change the era. Served on the
 * archive's own hostname so existing bookmarks work, and on "gateway" so the
 * feature does not depend on shadowing a real host.
 */
static int wayback_settings(GWHttpSession *s)
{
    GWWaybackSettings *set = GW_WaybackSettings();
    static char page[4096];
    static char target[GW_MAX_PATH];
    const char *query = strchr(s->req.url.path, '?');
    size_t n;

    if (query != NULL) {
        query++;
        if (gw_wayback_apply_query(query, strlen(query), set,
                                   target, sizeof(target))) {
            GW_WaybackSave();
            gw_log("#%ld wayback: %s, going to %.40s",
                   s->id, set->date, target);
            session_redirect(s, target);
            return 1;
        }
        GW_WaybackSave();
    }

    n = gw_wayback_settings_page(set, page, sizeof(page));
    if (n == 0) {
        session_fail(s, "HTTP/1.0 500 Internal Server Error\r\n"
                        "Connection: close\r\n\r\n",
                     "settings page would not fit");
        return 1;
    }
    session_serve(s, "text/html", page, n);
    return 1;
}

/*
 * Point a request at the archive, unless it is for the settings page or for a
 * host on the allow-list. Returns 1 when the session is already finished.
 */
static int wayback_prepare(GWHttpSession *s)
{
    GWWaybackSettings *set = GW_WaybackSettings();
    static char path[GW_MAX_PATH];
    char host[GW_MAX_HOST];

    if (GW_WaybackServesSettings() &&
        (gw_stricmp(s->req.url.host, GW_WB_HOST) == 0 ||
         gw_stricmp(s->req.url.host, "gateway") == 0))
        return wayback_settings(s);

    if (GW_WaybackHostIsLive(s->req.url.host)) {
        gw_log("#%ld live: %s", s->id, s->req.url.host);
        return 0;
    }

    /* GeoCities is not in the archive so much as at its successor. */
    if (set->geocities &&
        gw_wayback_geocities_host(s->req.url.host, host, sizeof(host))) {
        gw_copy_n(s->req.url.host, sizeof(s->req.url.host), host, strlen(host));
        s->req.url.tls = 1;
        s->req.url.port = 443;
        s->target = s->req.url;
        gw_log("#%ld geocities -> %s", s->id, host);
        return 0;
    }

    s->waybackOrigin = s->req.url;
    if (gw_wayback_path(set->date, &s->waybackOrigin, path, sizeof(path)) == 0) {
        session_fail(s, "HTTP/1.0 414 URI Too Long\r\n"
                        "Connection: close\r\n\r\n",
                     "archived URL would not fit");
        return 1;
    }

    s->req.url.tls = 1;
    s->req.url.port = 443;
    gw_copy_n(s->req.url.host, sizeof(s->req.url.host),
              GW_WB_HOST, strlen(GW_WB_HOST));
    gw_copy_n(s->req.url.path, sizeof(s->req.url.path), path, strlen(path));
    s->target = s->req.url;
    return 0;
}

static void session_start_upstream(GWHttpSession *s)
{
    int ok;

    s->ureqLen = gw_http_build_upstream(&s->req, s->chead, s->req.head_len,
                                        s->ureq, GW_HEAD_MAX, 1);
    if (s->ureqLen == 0) {
        session_fail(s, "HTTP/1.0 502 Bad Gateway\r\nConnection: close\r\n\r\n"
                        "Gateway: request headers too large.\r\n",
                     "request headers too large");
        return;
    }
    s->ureqSent = 0;
    s->uheadLen = 0;
    s->bodyLeft = -1;
    s->upReusable = 0;

    gw_copy_n(s->upHost, sizeof(s->upHost), s->req.url.host,
              strlen(s->req.url.host));
    s->upPort = s->req.url.port;
    s->upTls = s->req.url.tls;

    /* An idle connection to the same place saves a handshake, which on this
     * hardware is most of the cost of the request. */
    if (pool_take(s->upHost, s->upPort, s->upTls, &s->up)) {
        s->upPooled = 1;
        s->state = kHPSendRequest;
        return;
    }
    s->upPooled = 0;

    /*
     * Nothing idle to reuse, so this needs a new connection -- and those are
     * rationed. Wait rather than joining a burst; by the time we look again
     * there is often one back in the pool.
     */
    if (connecting_count(s->wayback) >=
        (s->wayback ? GW_WaybackConnects() : GW_MaxConnects())) {
        s->retryAt = GWNet_Ticks() + GW_CONNECT_WAIT;
        s->state = kHPConnectWait;
        return;
    }

    gw_log("#%ld opening a connection to %s", s->id, s->upHost);

    if (s->req.url.tls)
        ok = GWStream_ConnectTLS(&s->up, s->upHost, s->upPort);
    else
        ok = GWStream_ConnectPlain(&s->up, s->upHost, s->upPort);

    if (!ok) {
        session_fail(s, "HTTP/1.0 502 Bad Gateway\r\nConnection: close\r\n\r\n"
                        "Gateway: could not start the upstream connection.\r\n",
                     "upstream connect failed to start");
        return;
    }
    s->state = kHPConnect;
}

/*
 * A pooled connection can have been closed by the far end between being put
 * away and being handed out again, and nothing reveals that until the request
 * on it fails. Once, and only once, start over on a fresh connection.
 */
static int session_retry_fresh(GWHttpSession *s)
{
    if (!s->upPooled) return 0;

    gw_log("#%ld pooled connection was stale, reconnecting", s->id);
    GWStream_Destroy(&s->up);
    s->upPooled = 0;
    s->ureqSent = 0;
    s->uheadLen = 0;

    if (s->req.url.tls)
        GWStream_ConnectTLS(&s->up, s->upHost, s->upPort);
    else
        GWStream_ConnectPlain(&s->up, s->upHost, s->upPort);

    s->state = kHPConnect;
    return 1;
}

/* The response is complete: keep the connection if its framing allowed it. */
static void session_finish_body(GWHttpSession *s)
{
    if (s->upReusable && s->uheadLen == 0)
        pool_put(&s->up, s->upHost, s->upPort, s->upTls);
    else
        GWStream_Destroy(&s->up);

    s->state = kHPFlushAndClose;
}

/* ------------------------------------------------------------------ */

static void step_recv_request(GWHttpSession *s)
{
    long n;
    int  parsed;

    if (s->cheadLen >= (size_t)GW_HEAD_MAX) {
        session_fail(s, "HTTP/1.0 431 Request Header Fields Too Large\r\n"
                        "Connection: close\r\n\r\n",
                     "client head exceeded 16K");
        return;
    }

    n = GWStream_Read(&s->cli, s->chead + s->cheadLen,
                      (size_t)GW_HEAD_MAX - s->cheadLen);
    if (n == -1) { s->state = kHPDone; return; }
    if (n == -2 && s->cheadLen == 0) { s->state = kHPDone; return; }
    if (n > 0) {
        s->cheadLen += (size_t)n;
        s->lastActivity = GWNet_Ticks();
    }

    parsed = gw_http_parse_request(s->chead, s->cheadLen, &s->req);
    if (parsed == 0) {
        if (n == -2) s->state = kHPDone;    /* client hung up mid-request */
        return;
    }
    if (parsed < 0) {
        session_fail(s, "HTTP/1.0 400 Bad Request\r\nConnection: close\r\n\r\n"
                        "Gateway: could not parse that request.\r\n",
                     "malformed request");
        return;
    }

    gw_log("#%ld %s %s%s:%u%.48s%s", s->id, s->req.method,
           s->req.url.tls ? "https " : "", s->req.url.host,
           (unsigned)s->req.url.port, s->req.url.path,
           s->req.shape == kGWShapeConnect ? " (CONNECT)" : "");

    s->target = s->req.url;

    /* Module 3's only entry point on the request side. */
    if (s->wayback && s->req.shape != kGWShapeConnect) {
        if (wayback_prepare(s)) return;
    }

    if (s->req.shape == kGWShapeConnect) {
        if (!GWStream_ConnectPlain(&s->up, s->req.url.host, s->req.url.port)) {
            session_fail(s, "HTTP/1.0 502 Bad Gateway\r\n"
                            "Connection: close\r\n\r\n",
                         "CONNECT upstream failed to start");
            return;
        }
        /* Anything the client already sent past the request head is tunnel
         * payload; keep it and forward it once the tunnel is up. */
        s->cheadSent = s->req.head_len;
        s->state = kHPTunnelConnect;
        return;
    }

    s->reqBodyLeft = s->req.has_content_length ? s->req.content_length : 0;
    s->cheadSent = s->req.head_len;     /* first unread client body byte */
    session_start_upstream(s);
}

static void step_send_request(GWHttpSession *s)
{
    while (s->ureqSent < s->ureqLen) {
        long n = GWStream_Write(&s->up, s->ureq + s->ureqSent,
                                s->ureqLen - s->ureqSent);
        if (n < 0) {
            if (session_retry_fresh(s)) return;
            session_fail(s, "HTTP/1.0 502 Bad Gateway\r\n"
                            "Connection: close\r\n\r\n",
                         "upstream write failed");
            return;
        }
        if (n == 0) return;
        s->ureqSent += (size_t)n;
    }

    /* Relay whatever request body the client announced. */
    if (s->reqBodyLeft > 0) {
        size_t have = s->cheadLen > s->cheadSent ? s->cheadLen - s->cheadSent : 0;

        if (have > 0) {
            size_t take = have;
            long n;
            if ((long)take > s->reqBodyLeft) take = (size_t)s->reqBodyLeft;
            n = GWStream_Write(&s->up, s->chead + s->cheadSent, take);
            if (n < 0) {
                session_fail(s, "HTTP/1.0 502 Bad Gateway\r\n"
                                "Connection: close\r\n\r\n",
                             "upstream body write failed");
                return;
            }
            if (n > 0) {
                s->cheadSent += (size_t)n;
                s->reqBodyLeft -= n;
            }
            return;
        }
        {
            long n = GWStream_Read(&s->cli, s->raw, (size_t)GW_RAW_MAX);
            if (n <= 0) {
                if (n == -1 || n == -2) s->reqBodyLeft = 0;
                return;
            }
            if (n > s->reqBodyLeft) n = s->reqBodyLeft;
            {
                long w = GWStream_Write(&s->up, s->raw, (size_t)n);
                if (w < 0) {
                    session_fail(s, "HTTP/1.0 502 Bad Gateway\r\n"
                                    "Connection: close\r\n\r\n",
                                 "upstream body write failed");
                    return;
                }
                s->reqBodyLeft -= w;
            }
            return;
        }
    }

    s->state = kHPRecvHead;
}

static void session_fail(GWHttpSession *s, const char *statusLine,
                         const char *reason);

/*
 * Whether to chase this redirect ourselves.
 *
 * Normally only when the client could not: plaintext to TLS. The archive is
 * the exception. Asking for /web/<date>/<url> gets a redirect to the exact
 * snapshot, and handing that back to the browser would send it to an
 * archive.org address -- which arrives here again and gets archived a second
 * time. So Gateway follows that hop itself, and rebuilds the target with the
 * id_ modifier the archive's own Location leaves off.
 */
static int redirect_should_follow(GWHttpSession *s, const GWResponse *res)
{
    static char stamp[GW_WB_STAMP];
    static char original[GW_MAX_PATH];
    static char rebuilt[GW_MAX_PATH];

    if (s->wayback &&
        gw_stricmp(s->target.host, GW_WB_HOST) == 0 &&
        gw_wayback_parse(res->location, strlen(res->location),
                         stamp, sizeof(stamp),
                         original, sizeof(original))) {
        GWWaybackSettings *set = GW_WaybackSettings();
        int n;

        if (!gw_wayback_in_tolerance(set->date, stamp, set->tolerance)) {
            gw_log("#%ld snapshot %.8s is outside +%ld days of %s",
                   s->id, stamp, set->tolerance, set->date);
            session_fail(s, "HTTP/1.0 404 Not Found\r\n"
                            "Content-Type: text/html\r\n"
                            "Connection: close\r\n\r\n"
                            "<html><body><p>No snapshot of this page near the "
                            "date Gateway is set to.</p></body></html>\r\n",
                         "snapshot outside the tolerance");
            return 0;
        }

        n = snprintf(rebuilt, sizeof(rebuilt), "/web/%sid_/%s",
                     stamp, original);
        if (n > 0 && (size_t)n < sizeof(rebuilt)) {
            s->redirectTo = s->target;      /* stays on web.archive.org */
            gw_copy_n(s->redirectTo.path, sizeof(s->redirectTo.path),
                      rebuilt, (size_t)n);
            return 1;
        }
    }

    return gw_http_should_follow((GWRedirectPolicy)GW_RedirectPolicy(),
                                 s->target.tls, s->redirectTo.tls);
}

static void step_recv_head(GWHttpSession *s)
{
    /* Static rather than automatic: GWResponse carries a 4 KB Location, and
     * the cooperative loop runs one session at a time, so this is filled and
     * finished with before any other session sees it. */
    static GWResponse res;
    long n;
    int  parsed;

    if (s->uheadLen < (size_t)GW_HEAD_MAX) {
        n = GWStream_Read(&s->up, s->uhead + s->uheadLen,
                          (size_t)GW_HEAD_MAX - s->uheadLen);
        if (n > 0) {
            s->uheadLen += (size_t)n;
            s->lastActivity = GWNet_Ticks();
        } else if (n == -1) {
            if (s->uheadLen == 0 && session_retry_fresh(s)) return;
            session_fail(s, "HTTP/1.0 502 Bad Gateway\r\n"
                            "Connection: close\r\n\r\n",
                         "upstream read failed");
            return;
        }
    }

    parsed = gw_http_parse_response(s->uhead, s->uheadLen, &res);
    if (parsed == 0) {
        if (s->uheadLen >= (size_t)GW_HEAD_MAX)
            session_fail(s, "HTTP/1.0 502 Bad Gateway\r\n"
                            "Connection: close\r\n\r\n",
                         "response head exceeded 16K");
        else if (s->up.eof) {
            if (s->uheadLen == 0 && session_retry_fresh(s)) return;
            session_fail(s, "HTTP/1.0 502 Bad Gateway\r\n"
                            "Connection: close\r\n\r\n",
                         "upstream closed before sending a response");
        }
        return;
    }
    if (parsed < 0) {
        session_fail(s, "HTTP/1.0 502 Bad Gateway\r\n"
                        "Connection: close\r\n\r\n",
                     "unparsable response head");
        return;
    }

    s->status = res.status;
    GW_SetStatus("TLS %s  HTTP %d  %s",
                 s->up.tls ? (GWStream_TlsVersion(&s->up) == 13 ? "1.3" :
                              GWStream_TlsVersion(&s->up) == 12 ? "1.2" : "?")
                           : "-",
                 res.status, s->req.url.host);
    if (res.has_content_length)
        gw_log("#%ld <- %d %s %ld bytes", s->id, res.status,
               s->req.url.host, res.content_length);
    else
        gw_log("#%ld <- %d %s%s", s->id, res.status, s->req.url.host,
               res.chunked ? " chunked" : " no length");

    /*
     * Redirects. Gateway follows one only when the client could not have: see
     * gw_http_should_follow(). Anything else is passed back so the browser
     * follows it itself, with its cookies and its own idea of the final URL --
     * which is what a media fetch depends on.
     */
    if (gw_http_is_redirect(res.status) && res.has_location &&
        s->req.shape != kGWShapeConnect &&
        gw_url_resolve(&s->target, res.location, strlen(res.location),
                       &s->redirectTo) &&
        redirect_should_follow(s, &res)) {
        if (s->redirects >= GW_MAX_REDIRECT) {
            session_fail(s, "HTTP/1.0 508 Loop Detected\r\n"
                            "Connection: close\r\n\r\n"
                            "Gateway: too many redirects.\r\n",
                         "redirect limit reached");
            return;
        }
        {
            GWUrl next = s->redirectTo;

            s->redirects++;
            s->target = next;
            s->req.url = next;
            if (res.status == 303) {
                strcpy(s->req.method, "GET");
                s->req.has_content_length = 0;
                s->req.content_length = -1;
            }
            s->reqBodyLeft = 0;
            gw_log("#%ld -> %s%s:%u%.44s (redirect %d)",
                   s->id, next.tls ? "https://" : "http://", next.host,
                   (unsigned)next.port, next.path, s->redirects);

            /*
             * The archive answers a dated request with an empty redirect to
             * the exact snapshot, so this is the common case on the Wayback
             * port -- and keeping the connection here halves the handshakes
             * for a page.
             *
             * Consume the head before asking whether anything is left over.
             * This used to test s->uheadLen while it still held the whole
             * response head, which is never zero, so the connection was
             * dropped every time and every asset paid for a fresh TLS
             * handshake. Only bytes *past* the head disqualify reuse: they
             * would belong to a response nobody asked for.
             */
            s->uheadLen -= res.head_len;
            if (s->uheadLen > 0)
                memmove(s->uhead, s->uhead + res.head_len, s->uheadLen);

            if (!res.connection_close && res.has_content_length &&
                res.content_length == 0 && s->uheadLen == 0)
                pool_put(&s->up, s->upHost, s->upPort, s->upTls);
            else
                GWStream_Destroy(&s->up);

            s->uheadLen = 0;            /* the next response starts clean */
            session_start_upstream(s);
            return;
        }
    }

    /*
     * redirect_should_follow() can finish the session outright -- a snapshot
     * outside the tolerance answers the client itself. Without this the
     * error page it queued would be overwritten by the relay below, and the
     * session would carry on reading from an upstream that has been closed.
     */
    if (s->state != kHPRecvHead) return;

    if (gw_http_is_redirect(res.status) && res.has_location)
        gw_log("#%ld passing %d to the client: %.60s",
               s->id, res.status, res.location);

    {
        /* Keep the length whenever the body passes through untouched. It is
         * only wrong to forward when de-chunking changes it. */
        GWFilterOpts opt;
        size_t filtered;

        opt.keep_length = !res.chunked;
        opt.strip_charset = s->wayback && !GW_WaybackSettings()->ct_encoding;
        /* An archived snapshot never changes, so let the browser keep it. */
        opt.cache_forever = s->wayback && res.status == 200 &&
                            GW_WaybackCaches();

        filtered = gw_http_filter_response(s->uhead, res.head_len, s->out,
                                           (size_t)GW_OUT_MAX, &opt);
        if (filtered == 0) {
            session_fail(s, "HTTP/1.0 502 Bad Gateway\r\n"
                            "Connection: close\r\n\r\n",
                         "rewritten response head too large");
            return;
        }
        s->outLen = filtered;
        s->outSent = 0;
    }

    /*
     * How the body ends. With the connection held open there is no closing
     * EOF to fall back on, so a response that says neither Content-Length nor
     * chunked has to be read to EOF and the connection then dropped.
     */
    s->chunked = res.chunked;
    if (s->chunked) gw_chunked_init(&s->chunk);

    if (gw_stricmp(s->req.method, "HEAD") == 0 ||
        res.status == 204 || res.status == 304 ||
        (res.status >= 100 && res.status < 200))
        s->bodyLeft = 0;                       /* no body at all */
    else if (res.chunked)
        s->bodyLeft = -1;                      /* the decoder says when */
    else if (res.has_content_length)
        s->bodyLeft = res.content_length;
    else
        s->bodyLeft = -1;                      /* until EOF; cannot be kept */

    s->upReusable = !res.connection_close &&
                    (res.chunked || res.has_content_length ||
                     s->bodyLeft == 0);

    /* Whatever of the body already arrived with the head gets replayed by
     * shifting it to the front of uhead. */
    s->uheadLen -= res.head_len;
    if (s->uheadLen > 0)
        memmove(s->uhead, s->uhead + res.head_len, s->uheadLen);

    s->bodyBytes = 0;
    s->state = kHPBody;
}

/* Move up to len bytes of raw body through the chunked decoder (or straight
 * through) into the pending client buffer. Returns bytes of input consumed. */
static long body_emit(GWHttpSession *s, const char *data, size_t len)
{
    size_t room;

    if (s->outSent > 0) {
        memmove(s->out, s->out + s->outSent, s->outLen - s->outSent);
        s->outLen -= s->outSent;
        s->outSent = 0;
    }
    room = (size_t)GW_OUT_MAX - s->outLen;
    if (room == 0) return 0;

    if (!s->chunked) {
        if (len > room) len = room;
        memcpy(s->out + s->outLen, data, len);
        s->outLen += len;
        s->bodyBytes += (long)len;
        return (long)len;
    }

    {
        size_t produced = 0;
        long   used = gw_chunked_feed(&s->chunk, data, len,
                                      s->out + s->outLen, room, &produced);
        if (used < 0) return -1;
        s->outLen += produced;
        s->bodyBytes += (long)produced;
        return used;
    }
}

static void step_body(GWHttpSession *s)
{
    long   n;
    size_t want;
    int    flushed = session_flush(s);

    if (flushed < 0) { s->state = kHPDone; return; }
    if (flushed == 0) return;                /* client is flow controlled */

    if (s->bodyCap > 0 && s->bodyBytes > s->bodyCap) {
        gw_log("#%ld body hit the %ld MiB cap, truncating",
               s->id, s->bodyCap / (1024L * 1024L));
        s->upReusable = 0;                   /* the rest is still on the wire */
        session_finish_body(s);
        return;
    }

    /* Replay anything that arrived alongside the head first. */
    if (s->uheadLen > 0) {
        long used = body_emit(s, s->uhead, s->uheadLen);
        if (used < 0) {
            gw_log("#%ld malformed chunked body", s->id);
            s->upReusable = 0;
            session_finish_body(s);
            return;
        }
        if (used > 0) {
            s->uheadLen -= (size_t)used;
            if (s->uheadLen > 0)
                memmove(s->uhead, s->uhead + used, s->uheadLen);
            if (s->bodyLeft > 0) s->bodyLeft -= used;
        }
        if (s->bodyLeft != 0 || s->uheadLen > 0) return;
    }

    /* Done? Either the decoder says so, or the announced length ran out. */
    if ((s->chunked && gw_chunked_done(&s->chunk)) || s->bodyLeft == 0) {
        session_finish_body(s);
        return;
    }

    /*
     * Never read past the end of this response. With the connection held open
     * the next bytes on the wire belong to the next request, and swallowing
     * them here would desynchronise the stream for whoever gets it next.
     */
    want = (size_t)GW_RAW_MAX;
    if (s->bodyLeft > 0 && (long)want > s->bodyLeft) want = (size_t)s->bodyLeft;

    n = GWStream_Read(&s->up, s->raw, want);
    if (n == 0) return;
    if (n < 0) {
        /*
         * EOF is a legitimate end only when the response never said how long
         * it was; otherwise the origin cut us short and the connection is not
         * fit to keep.
         */
        if (s->bodyLeft > 0 || (s->chunked && !gw_chunked_done(&s->chunk)))
            s->upReusable = 0;
        session_finish_body(s);
        return;
    }
    s->lastActivity = GWNet_Ticks();

    {
        long used = body_emit(s, s->raw, (size_t)n);
        if (used < 0) {
            gw_log("#%ld malformed chunked body", s->id);
            s->upReusable = 0;
            session_finish_body(s);
            return;
        }
        if (s->bodyLeft > 0) s->bodyLeft -= used;
        if (used < n) {
            /* Output buffer filled: stash the remainder back in uhead. */
            size_t left = (size_t)(n - used);
            if (left > (size_t)GW_HEAD_MAX) left = (size_t)GW_HEAD_MAX;
            memcpy(s->uhead, s->raw + used, left);
            s->uheadLen = left;
        }
    }
}

static void step_tunnel_connect(GWHttpSession *s)
{
    static const char kEstablished[] =
        "HTTP/1.0 200 Connection Established\r\n\r\n";

    if (!session_queue(s, kEstablished, sizeof(kEstablished) - 1)) {
        s->state = kHPDone;
        return;
    }
    {
        int flushed = session_flush(s);
        if (flushed < 0) { s->state = kHPDone; return; }
        if (flushed == 0) return;
    }
    gw_log("#%ld tunnel open to %s:%u", s->id, s->req.url.host,
           (unsigned)s->req.url.port);
    s->state = kHPTunnel;
}

static void step_tunnel(GWHttpSession *s)
{
    long n;

    /* client -> upstream: drain the leftover request bytes, then read more */
    if (s->cheadSent < s->cheadLen) {
        n = GWStream_Write(&s->up, s->chead + s->cheadSent,
                           s->cheadLen - s->cheadSent);
        if (n < 0) { s->state = kHPDone; return; }
        if (n > 0) {
            s->cheadSent += (size_t)n;
            s->lastActivity = GWNet_Ticks();
        }
    } else {
        n = GWStream_Read(&s->cli, s->chead, (size_t)GW_HEAD_MAX);
        if (n > 0) {
            s->cheadLen = (size_t)n;
            s->cheadSent = 0;
            s->lastActivity = GWNet_Ticks();
        } else if (n == -2) {
            GWStream_Close(&s->up);
        } else if (n == -1) {
            s->state = kHPDone;
            return;
        }
    }

    /* upstream -> client */
    if (session_flush(s) < 0) { s->state = kHPDone; return; }
    if (s->outLen == 0) {
        n = GWStream_Read(&s->up, s->raw, (size_t)GW_RAW_MAX);
        if (n > 0) {
            session_queue(s, s->raw, (size_t)n);
            s->lastActivity = GWNet_Ticks();
        } else if (n == -2 || n == -1) {
            s->state = kHPFlushAndClose;
        }
    }
}

static void session_step(GWHttpSession *s)
{
    if (s->state == kHPFree) return;

    if (GWNet_Ticks() - s->lastActivity > GW_IDLE_TIMEOUT) {
        gw_log("#%ld idle timeout", s->id);
        s->state = kHPDone;
    }

    GWStream_Pump(&s->cli);
    if (s->up.state != kGWStreamIdle) GWStream_Pump(&s->up);

    if (s->cli.state == kGWStreamError) s->state = kHPDone;

    /*
     * If the browser has gone before a single byte of the response was sent,
     * there is nothing worth finishing. Clicking a link on a half-loaded page
     * abandons a dozen requests at once, and holding their session slots --
     * and their share of the upstream connections -- until each fetch
     * completes is what leaves nothing for the page being navigated to.
     *
     * Only these states, deliberately: once a response has started, the write
     * will fail on its own and end the session, and a client that merely
     * half-closed still gets its answer.
     */
    if ((s->state == kHPConnect || s->state == kHPConnectWait ||
         s->state == kHPRetryWait || s->state == kHPSendRequest) &&
        GWStream_PeerGone(&s->cli))
        s->state = kHPDone;

    switch (s->state) {
    case kHPRecvRequest:
        step_recv_request(s);
        break;

    case kHPConnect:
        if (s->up.state == kGWStreamReady) {
            s->state = kHPSendRequest;
        } else if (s->up.state == kGWStreamError ||
                   s->up.state == kGWStreamClosed) {
            /*
             * The archive refuses connections when it is being hit hard, and
             * a refusal says nothing about whether the asset exists. Wait a
             * moment and ask again before giving up on it.
             */
            if (s->wayback && s->retries < GW_WB_RETRIES) {
                s->retries++;
                s->retryAt = GWNet_Ticks() +
                             (unsigned long)(GW_WB_BACKOFF * s->retries);
                GWStream_Destroy(&s->up);
                s->state = kHPRetryWait;
                break;
            }
            {
                char why[160];
                session_fail(s, "HTTP/1.0 502 Bad Gateway\r\n"
                                "Connection: close\r\n\r\n"
                                "Gateway: could not reach the origin server.\r\n",
                             GWStream_Describe(&s->up, why, sizeof(why)));
            }
        }
        break;

    case kHPConnectWait:
        if (GWNet_Ticks() >= s->retryAt) {
            s->lastActivity = GWNet_Ticks();
            session_start_upstream(s);
        }
        break;

    case kHPRetryWait:
        if (GWNet_Ticks() >= s->retryAt) {
            gw_log("#%ld retrying (%d of %d)", s->id, s->retries,
                   GW_WB_RETRIES);
            s->lastActivity = GWNet_Ticks();
            session_start_upstream(s);
        }
        break;

    case kHPSendRequest:
        step_send_request(s);
        break;

    case kHPRecvHead:
        step_recv_head(s);
        break;

    case kHPBody:
        step_body(s);
        break;

    case kHPTunnelConnect:
        if (s->up.state == kGWStreamReady) {
            step_tunnel_connect(s);
        } else if (s->up.state == kGWStreamError ||
                   s->up.state == kGWStreamClosed) {
            session_fail(s, "HTTP/1.0 502 Bad Gateway\r\n"
                            "Connection: close\r\n\r\n",
                         "CONNECT upstream unreachable");
        }
        break;

    case kHPTunnel:
        step_tunnel(s);
        break;

    case kHPFlushAndClose: {
        int r = session_flush(s);
        if (r != 0) s->state = kHPDone;
        break;
    }

    default:
        break;
    }

    if (s->state == kHPDone) {
        GWStream_Close(&s->cli);
        session_reset(s);
    }
}

/* ------------------------------------------------------------------ */

void GWProxy_Init(void)
{
    if (sSessions != NULL) return;

    sSessionCount = GW_MaxSessions();
    sSessions = (GWHttpSession *)NewPtrClear(
        (Size)(sizeof(GWHttpSession) * sSessionCount));
    if (sSessions == NULL) sSessionCount = 0;
}

void GWProxy_Shutdown(void)
{
    int i;

    if (sSessions == NULL) return;
    for (i = 0; i < sSessionCount; i++)
        if (sSessions[i].state != kHPFree) session_reset(&sSessions[i]);
    DisposePtr((Ptr)sSessions);
    sSessions = NULL;
    sSessionCount = 0;
    pool_clear();
}

int GWProxy_Accept(GWConn *c, int wayback)
{
    int i;

    if (sSessions == NULL) return 0;

    for (i = 0; i < sSessionCount; i++) {
        GWHttpSession *s = &sSessions[i];
        if (s->state != kHPFree) continue;

        memset(s, 0, sizeof(*s));
        if (!session_alloc_buffers(s)) {
            gw_log("out of memory accepting a connection");
            return 0;
        }
        GWStream_Adopt(&s->cli, c);
        s->wayback = wayback;
        s->bodyCap = GW_MaxBodyBytes();
        s->id = ++sNextId;
        s->state = kHPRecvRequest;
        s->lastActivity = GWNet_Ticks();
        return 1;
    }
    return 0;
}

void GWProxy_Poll(void)
{
    int i;

    if (sSessions == NULL) return;
    for (i = 0; i < sSessionCount; i++) session_step(&sSessions[i]);
    pool_poll();
}

/*
 * Whether a slot is free right now.
 *
 * The caller polls its listeners only when this is true. Refusing to accept
 * leaves the connection sitting in Open Transport's listen backlog, where the
 * client simply waits; accepting and then destroying it -- which is what
 * Gateway used to do -- sends the client a reset, and a browser that gets a
 * reset on an image usually abandons that image rather than retrying it. That
 * is a page that never finishes loading. Every session is bounded by
 * GW_IDLE_TIMEOUT, so a slot cannot be held indefinitely and the backlog
 * cannot deadlock.
 */
int GWProxy_CanAccept(void)
{
    int i;

    if (sSessions == NULL) return 0;
    for (i = 0; i < sSessionCount; i++)
        if (sSessions[i].state == kHPFree) return 1;
    return 0;
}

int GWProxy_ActiveCount(void)
{
    int i, n = 0;

    if (sSessions == NULL) return 0;
    for (i = 0; i < sSessionCount; i++)
        if (sSessions[i].state != kHPFree) n++;
    return n;
}
