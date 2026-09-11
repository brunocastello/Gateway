#include "gw_http.h"
#include "gw_util.h"

#include <string.h>

/* Headers that describe this hop only and must never be relayed either way. */
static const char *kHopByHop[] = {
    "Connection",
    "Proxy-Connection",
    "Proxy-Authorization",
    "Proxy-Authenticate",
    "Keep-Alive",
    "TE",
    "Trailer",
    "Transfer-Encoding",
    "Upgrade",
    NULL
};

/* Dropped from requests only, because Gateway writes its own. */
static const char *kRequestOnly[] = {
    "Host",
    "Accept-Encoding",      /* Gateway forces identity */
    "Content-Length",       /* re-emitted verbatim when a body follows */
    NULL
};

static int header_in(const char **list, const char *line, size_t line_len)
{
    int i;
    for (i = 0; list[i] != NULL; i++) {
        size_t n = strlen(list[i]);
        if (line_len > n && gw_strnicmp(line, list[i], n) == 0 &&
            line[n] == ':')
            return 1;
    }
    return 0;
}

static int is_named(const char *line, size_t line_len, const char *name)
{
    size_t n = strlen(name);
    return line_len > n && gw_strnicmp(line, name, n) == 0 && line[n] == ':';
}

static int is_hop_by_hop(const char *line, size_t line_len)
{
    return header_in(kHopByHop, line, line_len) ||
           header_in(kRequestOnly, line, line_len);
}

static int append(char *out, size_t cap, size_t *used, const char *s, size_t n)
{
    if (*used + n > cap) return 0;
    memcpy(out + *used, s, n);
    *used += n;
    return 1;
}

static int appends(char *out, size_t cap, size_t *used, const char *s)
{
    return append(out, cap, used, s, strlen(s));
}

int gw_http_parse_request(const char *buf, size_t len, GWRequest *req)
{
    size_t head_len;
    size_t i, tok, target, target_end, ver;

    memset(req, 0, sizeof(*req));
    req->content_length = -1;

    if (!gw_find_head_end(buf, len, &head_len)) return 0;
    req->head_len = head_len;

    /* method */
    for (i = 0; i < head_len && buf[i] != ' ' && buf[i] != '\r' &&
                buf[i] != '\n'; i++)
        ;
    if (i == 0 || i >= sizeof(req->method) || buf[i] != ' ') return -1;
    gw_copy_n(req->method, sizeof(req->method), buf, i);

    tok = i;
    while (tok < head_len && buf[tok] == ' ') tok++;
    target = tok;
    while (tok < head_len && buf[tok] != ' ' && buf[tok] != '\r' &&
           buf[tok] != '\n')
        tok++;
    target_end = tok;
    if (target_end == target) return -1;

    /* version - default to HTTP/1.0 when absent (HTTP/0.9-ish clients) */
    req->http_minor = 0;
    while (tok < head_len && buf[tok] == ' ') tok++;
    ver = tok;
    if (ver + 8 <= head_len && gw_starts_ci(buf + ver, head_len - ver, "HTTP/1."))
        req->http_minor = (buf[ver + 7] == '1') ? 1 : 0;

    if (gw_stricmp(req->method, "CONNECT") == 0) {
        req->shape = kGWShapeConnect;
        req->url.tls = 0;               /* raw bounce; Gateway adds no TLS */
        if (!gw_url_split_authority(buf + target, target_end - target, 443,
                                    req->url.host, sizeof(req->url.host),
                                    &req->url.port))
            return -1;
        req->url.path[0] = '\0';
        return 1;
    }

    if (gw_url_split(buf + target, target_end - target, &req->url)) {
        req->shape = kGWShapeAbsolute;
    } else if (buf[target] == '/') {
        size_t hv_len;
        const char *hv = gw_header_find(buf, head_len, "Host", &hv_len);
        if (hv == NULL) return -1;
        if (!gw_url_split_authority(hv, hv_len, 80, req->url.host,
                                    sizeof(req->url.host), &req->url.port))
            return -1;
        req->url.tls = 0;
        if (target_end - target >= sizeof(req->url.path)) return -1;
        gw_copy_n(req->url.path, sizeof(req->url.path), buf + target,
                  target_end - target);
        req->shape = kGWShapeOrigin;
    } else {
        return -1;
    }

    {
        size_t cl_len;
        const char *cl = gw_header_find(buf, head_len, "Content-Length", &cl_len);
        if (cl != NULL) {
            long v = gw_parse_dec(cl, cl_len);
            if (v >= 0) {
                req->has_content_length = 1;
                req->content_length = v;
            }
        }
    }
    return 1;
}

size_t gw_http_build_upstream(const GWRequest *req,
                              const char *client_head, size_t head_len,
                              char *out, size_t cap, int keep_alive)
{
    size_t used = 0;
    size_t off;
    char portbuf[8];

    if (!appends(out, cap, &used, req->method)) return 0;
    if (!appends(out, cap, &used, " ")) return 0;
    if (!appends(out, cap, &used, req->url.path[0] ? req->url.path : "/"))
        return 0;
    if (!appends(out, cap, &used, keep_alive ? " HTTP/1.1\r\n"
                                             : " HTTP/1.0\r\n")) return 0;

    if (!appends(out, cap, &used, "Host: ")) return 0;
    if (!appends(out, cap, &used, req->url.host)) return 0;
    if ((req->url.tls && req->url.port != 443) ||
        (!req->url.tls && req->url.port != 80)) {
        unsigned short p = req->url.port;
        int n = 0;
        char tmp[8];
        do { tmp[n++] = (char)('0' + (p % 10)); p = (unsigned short)(p / 10); }
        while (p != 0 && n < 7);
        {
            int k;
            for (k = 0; k < n; k++) portbuf[k] = tmp[n - 1 - k];
            portbuf[n] = '\0';
        }
        if (!appends(out, cap, &used, ":")) return 0;
        if (!appends(out, cap, &used, portbuf)) return 0;
    }
    if (!appends(out, cap, &used, "\r\n")) return 0;

    /* Relay the client's remaining headers, minus the hop-by-hop ones. */
    off = gw_next_line(client_head, head_len, 0);
    while (off < head_len) {
        size_t eol = off;
        size_t line_end;

        while (eol < head_len && client_head[eol] != '\n') eol++;
        line_end = eol;
        if (line_end > off && client_head[line_end - 1] == '\r') line_end--;
        if (line_end == off) break;                 /* blank line */

        if (!is_hop_by_hop(client_head + off, line_end - off)) {
            if (!append(out, cap, &used, client_head + off, line_end - off))
                return 0;
            if (!appends(out, cap, &used, "\r\n")) return 0;
        }
        off = eol < head_len ? eol + 1 : head_len;
    }

    if (req->has_content_length) {
        size_t cl_len;
        const char *cl = gw_header_find(client_head, head_len,
                                        "Content-Length", &cl_len);
        if (cl != NULL) {
            if (!appends(out, cap, &used, "Content-Length: ")) return 0;
            if (!append(out, cap, &used, cl, cl_len)) return 0;
            if (!appends(out, cap, &used, "\r\n")) return 0;
        }
    }

    /* Certainly gives us bytes, not a decompressor: refuse content codings. */
    if (!appends(out, cap, &used, "Accept-Encoding: identity\r\n")) return 0;
    if (!appends(out, cap, &used, keep_alive ? "Connection: keep-alive\r\n"
                                             : "Connection: close\r\n"))
        return 0;
    if (!appends(out, cap, &used, "\r\n")) return 0;
    return used;
}

int gw_http_parse_response(const char *buf, size_t len, GWResponse *res)
{
    size_t head_len;
    size_t v_len;
    const char *v;

    memset(res, 0, sizeof(*res));
    res->content_length = -1;

    if (!gw_find_head_end(buf, len, &head_len)) return 0;
    res->head_len = head_len;

    if (!gw_starts_ci(buf, head_len, "HTTP/1.")) return -1;
    res->http_minor = (buf[7] == '1') ? 1 : 0;
    {
        long st = gw_parse_dec(buf + 8, head_len - 8);
        if (st < 100 || st > 599) return -1;
        res->status = (int)st;
    }

    v = gw_header_find(buf, head_len, "Transfer-Encoding", &v_len);
    if (v != NULL && v_len >= 7 && gw_strnicmp(v, "chunked", 7) == 0)
        res->chunked = 1;

    v = gw_header_find(buf, head_len, "Content-Length", &v_len);
    if (v != NULL) {
        long cl = gw_parse_dec(v, v_len);
        if (cl >= 0) {
            res->has_content_length = 1;
            res->content_length = cl;
        }
    }

    v = gw_header_find(buf, head_len, "Connection", &v_len);
    if (v != NULL && v_len >= 5 && gw_strnicmp(v, "close", 5) == 0)
        res->connection_close = 1;
    if (res->http_minor == 0 && v == NULL)
        res->connection_close = 1;      /* HTTP/1.0 closes unless told not to */

    v = gw_header_find(buf, head_len, "Location", &v_len);
    if (v != NULL && v_len > 0 && v_len < sizeof(res->location)) {
        gw_copy_n(res->location, sizeof(res->location), v, v_len);
        res->has_location = 1;
    }
    return 1;
}

size_t gw_http_filter_response(const char *head, size_t head_len,
                               char *out, size_t cap,
                               const GWFilterOpts *opt)
{
    size_t used = 0;
    size_t off = 0;

    while (off < head_len) {
        size_t eol = off;
        size_t line_end;
        int    drop;

        while (eol < head_len && head[eol] != '\n') eol++;
        line_end = eol;
        if (line_end > off && head[line_end - 1] == '\r') line_end--;
        if (line_end == off) break;                 /* blank line */

        drop = header_in(kHopByHop, head + off, line_end - off) ||
               (!opt->keep_length &&
                is_named(head + off, line_end - off, "Content-Length"));

        /* The replacements are appended once, below. */
        if (opt->cache_forever &&
            (is_named(head + off, line_end - off, "Cache-Control") ||
             is_named(head + off, line_end - off, "Pragma") ||
             is_named(head + off, line_end - off, "Expires") ||
             is_named(head + off, line_end - off, "Age")))
            drop = 1;

        if (off == 0 || !drop) {
            size_t emit = line_end - off;

            /* Cut "text/html; charset=utf-8" back to "text/html". */
            if (opt->strip_charset && off != 0 &&
                is_named(head + off, emit, "Content-Type")) {
                size_t k;
                for (k = 0; k < emit; k++) {
                    if (head[off + k] == ';') {
                        while (k > 0 && (head[off + k - 1] == ' ' ||
                                         head[off + k - 1] == '\t')) k--;
                        emit = k;
                        break;
                    }
                }
            }

            if (!append(out, cap, &used, head + off, emit)) return 0;
            if (!appends(out, cap, &used, "\r\n")) return 0;
        }
        off = eol < head_len ? eol + 1 : head_len;
    }

    if (opt->cache_forever) {
        /*
         * A fixed date rather than one computed from the clock: it needs no
         * time formatting, and a Mac whose clock has drifted still gets a
         * date comfortably in the future. Both headers are sent because the
         * browsers this serves span the HTTP/1.0 and 1.1 divide.
         */
        if (!appends(out, cap, &used,
                     "Cache-Control: public, max-age=31536000\r\n"))
            return 0;
        if (!appends(out, cap, &used,
                     "Expires: Thu, 31 Dec 2037 23:59:59 GMT\r\n"))
            return 0;
    }

    if (!appends(out, cap, &used, "Connection: close\r\n")) return 0;
    if (!appends(out, cap, &used, "\r\n")) return 0;
    return used;
}

int gw_http_is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

int gw_http_should_follow(GWRedirectPolicy policy, int from_tls, int to_tls)
{
    switch (policy) {
    case kGWRedirectNever:
        return 0;
    case kGWRedirectAlways:
        return 1;
    default:
        /*
         * Follow only the hop the client could not make for itself -- which is
         * every hop that ends in TLS, because the client hop is always
         * plaintext. Everything else goes back to the client, which follows it
         * with its own cookies and knows where it ended up.
         *
         * This used to read `!from_tls && to_tls`, asking where *Gateway* was
         * rather than what the client can do, and the difference is a bug:
         * once Gateway had followed one http->https hop it was itself on TLS,
         * so a second https->https redirect failed the test and went back to
         * the browser as a `Location: https://...` the browser could not
         * fetch. Internet Explorer and Netscape 4 then opened a CONNECT
         * tunnel, tried their own 1997 handshake against a 2026 server, and
         * failed -- "an error occurred in the secure channel support", and
         * "no common encryption algorithm(s)". Two redirects was all it took;
         * lite.duckduckgo.com sends exactly two.
         *
         * from_tls is kept in the signature because `always` and `never` read
         * better beside a rule that names both ends, and because a caller
         * passing it is stating something true.
         */
        (void)from_tls;
        return to_tls;
    }
}
