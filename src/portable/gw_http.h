/*
 * gw_http.h - HTTP request/response head parsing and rewriting for Module 1.
 *
 * PORTABLE: no Mac or Windows headers. See gw_util.h.
 *
 * Gateway accepts three request shapes on :8765 (CLAUDE.md, Module 1):
 *
 *   1. GET http://host/path HTTP/1.0    classic forward proxy
 *   2. GET https://host/path HTTP/1.0   Classilla with
 *                                       network.http.proxy.use-http-proxy-for-https
 *   3. CONNECT host:443 HTTP/1.0        raw tunnel (git and friends)
 *
 * Origin-form ("GET /path" plus a Host: header) is also accepted and treated
 * as shape 1, which is what a browser does when it is pointed at Gateway as a
 * plain web server rather than as a proxy.
 */
#ifndef GW_HTTP_H
#define GW_HTTP_H

#include <stddef.h>

#include "gw_url.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_MAX_METHOD 16

typedef enum {
    kGWShapeInvalid = 0,
    kGWShapeAbsolute,       /* shapes 1 and 2 - target was an absolute URI  */
    kGWShapeOrigin,         /* origin-form target plus Host:                */
    kGWShapeConnect         /* shape 3                                      */
} GWReqShape;

typedef struct {
    GWReqShape shape;
    char       method[GW_MAX_METHOD];
    GWUrl      url;                 /* host/port/tls/path of the upstream hop */
    int        http_minor;          /* 0 or 1, as sent by the client          */
    size_t     head_len;            /* bytes through the terminating blank line */
    int        has_content_length;
    long       content_length;      /* client request body length, -1 if none */
} GWRequest;

/*
 * Parse a client request head.
 * Returns  1 when a complete head was parsed,
 *          0 when more bytes are needed,
 *         -1 when the request is malformed or unsupported.
 */
int gw_http_parse_request(const char *buf, size_t len, GWRequest *req);

/*
 * Build the request Gateway sends upstream. Copies the client's headers minus
 * the hop-by-hop ones, forces Host: and identity encoding.
 *
 * keep_alive asks for HTTP/1.1 with the connection held open, so the stream
 * can serve the next request too. That is worth a great deal here: every
 * archive fetch goes to the same host, and a TLS handshake on this hardware
 * costs more than the transfer does. It obliges the caller to frame the
 * response exactly -- by Content-Length or by chunked -- since there is no
 * closing EOF to mark the end.
 *
 * Returns the number of bytes written, or 0 if it would not fit in cap.
 */
size_t gw_http_build_upstream(const GWRequest *req,
                              const char *client_head, size_t head_len,
                              char *out, size_t cap, int keep_alive);

typedef struct {
    int    status;
    int    http_minor;
    size_t head_len;
    int    chunked;
    int    has_content_length;
    long   content_length;
    int    has_location;
    int    connection_close;    /* the origin will not hold the connection */
    char   location[GW_MAX_PATH];
} GWResponse;

/*
 * Parse an origin response head. Same return convention as
 * gw_http_parse_request().
 */
int gw_http_parse_response(const char *buf, size_t len, GWResponse *res);

/*
 * Rewrite an origin response head for the client hop: drops the hop-by-hop
 * headers and appends Connection: close.
 *
 * keep_length decides what happens to Content-Length. Pass 0 when Gateway is
 * de-chunking, because the body it emits is a different length from the one
 * the origin announced; pass 1 when the body goes through untouched, so the
 * client learns how many bytes to expect.
 *
 * That distinction matters more than it looks. A player fetching video will
 * generally not start without a length: it has nothing to size a buffer with,
 * no duration, and no way to seek. Dropping the header unconditionally made
 * every media fetch fail while ordinary pages were unaffected.
 *
 * Returns bytes written, or 0 on overflow.
 */
typedef struct {
    /*
     * Keep Content-Length. Pass 0 when de-chunking, since the body Gateway
     * emits is then a different length from the one the origin announced.
     */
    int keep_length;

    /*
     * Drop the "; charset=..." parameter from Content-Type. Some period
     * browsers choke on it; the Wayback settings page calls this "Encoding in
     * Content-Type".
     */
    int strip_charset;

    /*
     * Replace the origin's caching headers with a very long expiry.
     *
     * For archived pages this is not a liberty: a snapshot of a site as it
     * stood on a day in 2001 is immutable, and the archive nevertheless
     * serves it with max-age=1800. Taking that at face value means a browser
     * re-fetching every asset half an hour later, each one costing a request
     * through Gateway to a server that will return exactly what it did
     * before. Only ever set this for the Wayback listener.
     */
    int cache_forever;
} GWFilterOpts;

size_t gw_http_filter_response(const char *head, size_t head_len,
                               char *out, size_t cap,
                               const GWFilterOpts *opt);

/*
 * What to do about a redirect the origin sent.
 *
 * A forward proxy would normally pass a 3xx straight to the client and let it
 * follow, with its own cookie jar and its own idea of the final URL. Gateway
 * follows them itself, which is right for exactly one case: a browser too old
 * to speak modern TLS cannot follow a redirect from http:// to https:// on its
 * own, and would simply fail.
 *
 * So the default is to follow only that case and pass everything else along.
 * Following an ordinary http -> http redirect internally hides the response
 * from the client, discards any Set-Cookie it carried, and leaves the client
 * unaware of where it ended up -- which breaks token-authenticated media URLs.
 */
typedef enum {
    kGWRedirectAuto = 0,    /* follow only what the client cannot */
    kGWRedirectAlways,
    kGWRedirectNever
} GWRedirectPolicy;

int gw_http_should_follow(GWRedirectPolicy policy, int from_tls, int to_tls);

/* 1 when a 3xx status that Gateway should follow. */
int gw_http_is_redirect(int status);

#ifdef __cplusplus
}
#endif

#endif /* GW_HTTP_H */
