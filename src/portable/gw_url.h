/*
 * gw_url.h - absolute-URI splitting and redirect resolution.
 *
 * PORTABLE: no Mac or Windows headers. See gw_util.h.
 */
#ifndef GW_URL_H
#define GW_URL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_MAX_HOST 256

/*
 * Signed media URLs -- the ones a video player is redirected to -- routinely
 * run past a kilobyte of query string. At 1024 those were rejected outright as
 * an unusable Location header.
 */
#define GW_MAX_PATH 4096

typedef struct {
    char           host[GW_MAX_HOST];
    unsigned short port;
    int            tls;                 /* 1 when the scheme was https */
    char           path[GW_MAX_PATH];   /* origin-form, always starts with '/' */
} GWUrl;

/*
 * Split an absolute URI ("http://host[:port]/path?query").
 * Returns 1 on success, 0 when the string is not an absolute http(s) URI.
 */
int gw_url_split(const char *url, size_t len, GWUrl *out);

/*
 * Split an authority-form target as used by CONNECT ("host:443").
 * defport is used when no port is present. Returns 1 on success.
 */
int gw_url_split_authority(const char *s, size_t len, unsigned short defport,
                           char *host, size_t host_cap, unsigned short *port);

/*
 * Percent-decode a URL component in place into out. '+' becomes a space, which
 * is what a GET form sends. Returns the decoded length.
 */
size_t gw_url_decode(const char *src, size_t len, char *out, size_t cap);

/*
 * Look up a field in an application/x-www-form-urlencoded query string, the
 * part after '?'. The value is percent-decoded. Returns 1 when the field is
 * present, even with an empty value.
 */
int gw_url_query_get(const char *query, size_t len, const char *name,
                     char *out, size_t cap);

/*
 * Whether a field appears at all. Checkboxes on a GET form arrive as "name=on"
 * and are simply absent when unchecked, so presence is the whole signal.
 */
int gw_url_query_has(const char *query, size_t len, const char *name);

/*
 * Resolve a Location header against the request it answered. Handles absolute
 * URIs, absolute paths ("/x") and relative paths ("x"). Returns 1 on success.
 */
int gw_url_resolve(const GWUrl *base, const char *loc, size_t loc_len,
                   GWUrl *out);

#ifdef __cplusplus
}
#endif

#endif /* GW_URL_H */
