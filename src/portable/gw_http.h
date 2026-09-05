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
 * the hop-by-hop ones, forces Host:, Connection: close and identity encoding.
 * Returns the number of bytes written, or 0 if it would not fit in cap.
 */
size_t gw_http_build_upstream(const GWRequest *req,
                              const char *client_head, size_t head_len,
                              char *out, size_t cap);

typedef struct {
    int    status;
    int    http_minor;
    size_t head_len;
    int    chunked;
    int    has_content_length;
    long   content_length;
    int    has_location;
    char   location[GW_MAX_PATH];
} GWResponse;

/*
 * Parse an origin response head. Same return convention as
 * gw_http_parse_request().
 */
int gw_http_parse_response(const char *buf, size_t len, GWResponse *res);

/*
 * Rewrite an origin response head for the client hop: drops Transfer-Encoding,
 * Content-Length, Connection and Keep-Alive, then appends Connection: close.
 * The client hop is always EOF-delimited, so no length is re-advertised.
 * Returns bytes written, or 0 on overflow.
 */
size_t gw_http_filter_response(const char *head, size_t head_len,
                               char *out, size_t cap);

/* 1 when a 3xx status that Gateway should follow. */
int gw_http_is_redirect(int status);

#ifdef __cplusplus
}
#endif

#endif /* GW_HTTP_H */
