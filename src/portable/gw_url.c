#include "gw_url.h"
#include "gw_util.h"

#include <string.h>

int gw_url_split_authority(const char *s, size_t len, unsigned short defport,
                           char *host, size_t host_cap, unsigned short *port)
{
    size_t i;
    size_t hlen = len;
    long p = defport;

    while (len > 0 && (*s == ' ' || *s == '\t')) { s++; len--; }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    if (len == 0) return 0;

    hlen = len;
    for (i = 0; i < len; i++) {
        if (s[i] == ':') {
            hlen = i;
            p = gw_parse_dec(s + i + 1, len - i - 1);
            break;
        }
        if (s[i] == '/') { hlen = i; break; }
    }
    if (hlen == 0 || hlen >= host_cap) return 0;
    if (p < 1 || p > 65535) return 0;

    gw_copy_n(host, host_cap, s, hlen);
    *port = (unsigned short)p;
    return 1;
}

int gw_url_split(const char *url, size_t len, GWUrl *out)
{
    size_t off;
    size_t auth_end;
    unsigned short defport;

    memset(out, 0, sizeof(*out));

    if (gw_starts_ci(url, len, "https://")) {
        out->tls = 1;
        defport = 443;
        off = 8;
    } else if (gw_starts_ci(url, len, "http://")) {
        out->tls = 0;
        defport = 80;
        off = 7;
    } else {
        return 0;
    }

    auth_end = off;
    while (auth_end < len && url[auth_end] != '/') auth_end++;

    if (!gw_url_split_authority(url + off, auth_end - off, defport,
                                out->host, sizeof(out->host), &out->port))
        return 0;

    if (auth_end >= len) {
        out->path[0] = '/';
        out->path[1] = '\0';
    } else {
        if (len - auth_end >= sizeof(out->path)) return 0;
        gw_copy_n(out->path, sizeof(out->path), url + auth_end, len - auth_end);
    }
    return 1;
}

int gw_url_resolve(const GWUrl *base, const char *loc, size_t loc_len,
                   GWUrl *out)
{
    while (loc_len > 0 && (*loc == ' ' || *loc == '\t')) { loc++; loc_len--; }
    while (loc_len > 0 &&
           (loc[loc_len - 1] == '\r' || loc[loc_len - 1] == '\n' ||
            loc[loc_len - 1] == ' '  || loc[loc_len - 1] == '\t'))
        loc_len--;
    if (loc_len == 0) return 0;

    if (gw_starts_ci(loc, loc_len, "http://") ||
        gw_starts_ci(loc, loc_len, "https://"))
        return gw_url_split(loc, loc_len, out);

    /* Same origin; only the path changes. */
    *out = *base;

    if (loc[0] == '/') {
        if (loc_len >= sizeof(out->path)) return 0;
        gw_copy_n(out->path, sizeof(out->path), loc, loc_len);
        return 1;
    }

    {
        /* Relative reference: replace the last segment of the base path. */
        size_t keep = strlen(base->path);
        while (keep > 0 && base->path[keep - 1] != '/') keep--;
        if (keep + loc_len >= sizeof(out->path)) return 0;
        memcpy(out->path, base->path, keep);
        memcpy(out->path + keep, loc, loc_len);
        out->path[keep + loc_len] = '\0';
    }
    return 1;
}

static int hexdigit(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

size_t gw_url_decode(const char *src, size_t len, char *out, size_t cap)
{
    size_t i, o = 0;

    if (cap == 0) return 0;

    for (i = 0; i < len && o + 1 < cap; i++) {
        if (src[i] == '+') {
            out[o++] = ' ';
        } else if (src[i] == '%' && i + 2 < len) {
            int hi = hexdigit((unsigned char)src[i + 1]);
            int lo = hexdigit((unsigned char)src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                out[o++] = src[i];
            }
        } else {
            out[o++] = src[i];
        }
    }
    out[o] = '\0';
    return o;
}

/* Locate "name=" in the query, returning the offset of its value. */
static long query_find(const char *query, size_t len, const char *name,
                       size_t *val_len)
{
    size_t nlen = strlen(name);
    size_t off = 0;

    while (off < len) {
        size_t end = off;
        size_t eq;

        while (end < len && query[end] != '&') end++;

        eq = off;
        while (eq < end && query[eq] != '=') eq++;

        if (eq - off == nlen && gw_strnicmp(query + off, name, nlen) == 0) {
            if (eq < end) {
                *val_len = end - eq - 1;
                return (long)(eq + 1);
            }
            *val_len = 0;               /* bare "name" with no '=' */
            return (long)end;
        }
        off = end < len ? end + 1 : len;
    }
    return -1;
}

int gw_url_query_get(const char *query, size_t len, const char *name,
                     char *out, size_t cap)
{
    size_t val_len = 0;
    long   at = query_find(query, len, name, &val_len);

    if (cap) out[0] = '\0';
    if (at < 0) return 0;

    gw_url_decode(query + at, val_len, out, cap);
    return 1;
}

int gw_url_query_has(const char *query, size_t len, const char *name)
{
    size_t val_len = 0;
    return query_find(query, len, name, &val_len) >= 0;
}
