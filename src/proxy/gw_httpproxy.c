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

#include <Memory.h>
#include <string.h>
#include <stdio.h>

#include "../gw_core.h"
#include "../portable/gw_chunked.h"
#include "../portable/gw_http.h"
#include "../portable/gw_log.h"
#include "../portable/gw_url.h"

#define GW_HEAD_MAX     16384L
#define GW_RAW_MAX      16384L
#define GW_OUT_MAX      32768L
#define GW_MAX_REDIRECT 5
#define GW_IDLE_TIMEOUT (120 * 60)          /* ticks: two minutes */

typedef enum {
    kHPFree = 0,
    kHPRecvRequest,
    kHPConnect,
    kHPSendRequest,
    kHPRecvHead,
    kHPBody,
    kHPTunnelConnect,
    kHPTunnel,
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

static GWHttpSession *sSessions;
static long           sNextId;

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
    size_t n = strlen(statusLine);

    gw_log("#%ld %s", s->id, reason);
    if (s->out != NULL && s->outLen == 0 && n < (size_t)GW_OUT_MAX) {
        memcpy(s->out, statusLine, n);
        s->outLen = n;
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

static void session_start_upstream(GWHttpSession *s)
{
    int ok;

    s->ureqLen = gw_http_build_upstream(&s->req, s->chead, s->req.head_len,
                                        s->ureq, GW_HEAD_MAX);
    if (s->ureqLen == 0) {
        session_fail(s, "HTTP/1.0 502 Bad Gateway\r\nConnection: close\r\n\r\n"
                        "Gateway: request headers too large.\r\n",
                     "request headers too large");
        return;
    }
    s->ureqSent = 0;
    s->uheadLen = 0;

    if (s->req.url.tls)
        ok = GWStream_ConnectTLS(&s->up, s->req.url.host, s->req.url.port);
    else
        ok = GWStream_ConnectPlain(&s->up, s->req.url.host, s->req.url.port);

    if (!ok) {
        session_fail(s, "HTTP/1.0 502 Bad Gateway\r\nConnection: close\r\n\r\n"
                        "Gateway: could not start the upstream connection.\r\n",
                     "upstream connect failed to start");
        return;
    }
    s->state = kHPConnect;
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
        else if (s->up.eof)
            session_fail(s, "HTTP/1.0 502 Bad Gateway\r\n"
                            "Connection: close\r\n\r\n",
                         "upstream closed before sending a response");
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
        gw_http_should_follow((GWRedirectPolicy)GW_RedirectPolicy(),
                              s->target.tls, s->redirectTo.tls)) {
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
            GWStream_Destroy(&s->up);
            session_start_upstream(s);
            return;
        }
    }

    if (gw_http_is_redirect(res.status) && res.has_location)
        gw_log("#%ld passing %d to the client: %.60s",
               s->id, res.status, res.location);

    {
        /* Keep the length whenever the body passes through untouched. It is
         * only wrong to forward when de-chunking changes it. */
        size_t filtered = gw_http_filter_response(s->uhead, res.head_len,
                                                  s->out, (size_t)GW_OUT_MAX,
                                                  !res.chunked);
        if (filtered == 0) {
            session_fail(s, "HTTP/1.0 502 Bad Gateway\r\n"
                            "Connection: close\r\n\r\n",
                         "rewritten response head too large");
            return;
        }
        s->outLen = filtered;
        s->outSent = 0;
    }

    s->chunked = res.chunked;
    if (s->chunked) gw_chunked_init(&s->chunk);

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
    long n;
    int  flushed = session_flush(s);

    if (flushed < 0) { s->state = kHPDone; return; }
    if (flushed == 0) return;                /* client is flow controlled */

    if (s->bodyCap > 0 && s->bodyBytes > s->bodyCap) {
        gw_log("#%ld body hit the %ld MiB cap, truncating",
               s->id, s->bodyCap / (1024L * 1024L));
        s->state = kHPFlushAndClose;
        return;
    }

    /* Replay anything that arrived alongside the head first. */
    if (s->uheadLen > 0) {
        long used = body_emit(s, s->uhead, s->uheadLen);
        if (used < 0) {
            gw_log("#%ld malformed chunked body", s->id);
            s->state = kHPFlushAndClose;
            return;
        }
        if (used > 0) {
            s->uheadLen -= (size_t)used;
            if (s->uheadLen > 0)
                memmove(s->uhead, s->uhead + used, s->uheadLen);
        }
        return;
    }

    if (s->chunked && gw_chunked_done(&s->chunk)) {
        s->state = kHPFlushAndClose;
        return;
    }

    n = GWStream_Read(&s->up, s->raw, (size_t)GW_RAW_MAX);
    if (n == 0) return;
    if (n < 0) {                             /* EOF or error ends the body */
        s->state = kHPFlushAndClose;
        return;
    }
    s->lastActivity = GWNet_Ticks();

    {
        long used = body_emit(s, s->raw, (size_t)n);
        if (used < 0) {
            gw_log("#%ld malformed chunked body", s->id);
            s->state = kHPFlushAndClose;
            return;
        }
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

    switch (s->state) {
    case kHPRecvRequest:
        step_recv_request(s);
        break;

    case kHPConnect:
        if (s->up.state == kGWStreamReady) {
            s->state = kHPSendRequest;
        } else if (s->up.state == kGWStreamError ||
                   s->up.state == kGWStreamClosed) {
            {
                char why[160];
                session_fail(s, "HTTP/1.0 502 Bad Gateway\r\n"
                                "Connection: close\r\n\r\n"
                                "Gateway: could not reach the origin server.\r\n",
                             GWStream_Describe(&s->up, why, sizeof(why)));
            }
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
    sSessions = (GWHttpSession *)NewPtrClear(
        (Size)(sizeof(GWHttpSession) * GW_MAX_SESSIONS));
}

void GWProxy_Shutdown(void)
{
    int i;

    if (sSessions == NULL) return;
    for (i = 0; i < GW_MAX_SESSIONS; i++)
        if (sSessions[i].state != kHPFree) session_reset(&sSessions[i]);
    DisposePtr((Ptr)sSessions);
    sSessions = NULL;
}

int GWProxy_Accept(GWConn *c)
{
    int i;

    if (sSessions == NULL) return 0;

    for (i = 0; i < GW_MAX_SESSIONS; i++) {
        GWHttpSession *s = &sSessions[i];
        if (s->state != kHPFree) continue;

        memset(s, 0, sizeof(*s));
        if (!session_alloc_buffers(s)) {
            gw_log("out of memory accepting a connection");
            return 0;
        }
        GWStream_Adopt(&s->cli, c);
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
    for (i = 0; i < GW_MAX_SESSIONS; i++) session_step(&sSessions[i]);
}

int GWProxy_ActiveCount(void)
{
    int i, n = 0;

    if (sSessions == NULL) return 0;
    for (i = 0; i < GW_MAX_SESSIONS; i++)
        if (sSessions[i].state != kHPFree) n++;
    return n;
}
