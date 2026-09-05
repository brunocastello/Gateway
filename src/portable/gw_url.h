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
#define GW_MAX_PATH 1024

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
 * Resolve a Location header against the request it answered. Handles absolute
 * URIs, absolute paths ("/x") and relative paths ("x"). Returns 1 on success.
 */
int gw_url_resolve(const GWUrl *base, const char *loc, size_t loc_len,
                   GWUrl *out);

#ifdef __cplusplus
}
#endif

#endif /* GW_URL_H */
