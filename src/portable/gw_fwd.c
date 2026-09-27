/*
 * gw_fwd.c - forward-proxy handshake grammar. See gw_fwd.h.
 *
 * PORTABLE: plain C99 plus gw_util.h. No system headers beyond stdio/string.
 */

#include "gw_fwd.h"

#include <stdio.h>
#include <string.h>

#include "gw_util.h"

int gw_fwd_kind(const char *name)
{
    if (name == NULL) return -1;
    if (gw_stricmp(name, "none") == 0 ||
        gw_stricmp(name, "direct") == 0 ||
        name[0] == '\0') return GW_FWD_NONE;
    if (gw_stricmp(name, "http") == 0 ||
        gw_stricmp(name, "connect") == 0) return GW_FWD_HTTP;
    if (gw_stricmp(name, "socks5") == 0 ||
        gw_stricmp(name, "socks") == 0) return GW_FWD_SOCKS5;
    return -1;
}

static size_t gw_fwd_connect_req_impl(const char *host, unsigned port,
                                      const char *b64, char *out, size_t cap,
                                      int with_host)
{
    int n;

    if (host == NULL || host[0] == '\0' || port == 0 || port > 65535)
        return 0;
    if (out == NULL || cap == 0) return 0;

    if (b64 != NULL && b64[0] != '\0') {
        if (with_host)
            n = snprintf(out, cap,
                         "CONNECT %s:%u HTTP/1.0\r\n"
                         "Host: %s:%u\r\n"
                         "Proxy-Authorization: Basic %s\r\n"
                         "\r\n",
                         host, port, host, port, b64);
        else
            n = snprintf(out, cap,
                         "CONNECT %s:%u HTTP/1.0\r\n"
                         "Proxy-Authorization: Basic %s\r\n"
                         "\r\n",
                         host, port, b64);
    } else {
        if (with_host)
            n = snprintf(out, cap,
                         "CONNECT %s:%u HTTP/1.0\r\n"
                         "Host: %s:%u\r\n"
                         "\r\n",
                         host, port, host, port);
        else
            n = snprintf(out, cap,
                         "CONNECT %s:%u HTTP/1.0\r\n"
                         "\r\n",
                         host, port);
    }

    /*
     * snprintf returns what would have been written: n >= cap means the
     * request was cut off, which the proxy would read as a different
     * request. Report it rather than send it.
     */
    if (n <= 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

size_t gw_fwd_connect_req(const char *host, unsigned port, const char *b64,
                          char *out, size_t cap)
{
    return gw_fwd_connect_req_impl(host, port, b64, out, cap, 1);
}

size_t gw_fwd_connect_req_bare(const char *host, unsigned port,
                               const char *b64, char *out, size_t cap)
{
    return gw_fwd_connect_req_impl(host, port, b64, out, cap, 0);
}

int gw_fwd_connect_reply(const char *buf, size_t len,
                         size_t *head_len, long *code)
{
    size_t hlen = 0;
    size_t eol, i;
    long   status;

    if (buf == NULL || len == 0) return 0;
    if (!gw_find_head_end(buf, len, &hlen)) return 0;   /* still arriving */
    if (head_len != NULL) *head_len = hlen;

    /*
     * The status line is the first line. It must read HTTP/x.y SP code;
     * anything else is not a proxy reply (a SOCKS server answering an HTTP
     * greeting is the classic way to get here).
     */
    if (!gw_starts_ci(buf, len, "HTTP/")) return -1;
    eol = len;
    for (i = 0; i < hlen; i++) {
        if (buf[i] == '\n') { eol = i; break; }
    }
    for (i = 0; i < eol; i++) {
        if (buf[i] == ' ') {
            status = gw_parse_dec(buf + i + 1, eol - i - 1);
            if (status < 100 || status > 599) return -1;
            if (code != NULL) *code = status;
            return 1;
        }
    }
    return -1;
}

size_t gw_fwd_socks_greet(unsigned char *out, size_t cap)
{
    if (out == NULL || cap < 3) return 0;
    out[0] = 0x05;
    out[1] = 0x01;          /* one method */
    out[2] = 0x00;          /* no authentication */
    return 3;
}

size_t gw_fwd_socks_connect(const char *host, unsigned port,
                            unsigned char *out, size_t cap)
{
    size_t hlen;

    if (host == NULL || port == 0 || port > 65535) return 0;
    hlen = strlen(host);
    if (hlen == 0 || hlen > 255) return 0;
    if (out == NULL || cap < 7 + hlen) return 0;

    out[0] = 0x05;
    out[1] = 0x01;          /* CONNECT */
    out[2] = 0x00;          /* reserved */
    out[3] = 0x03;          /* domain name follows */
    out[4] = (unsigned char)hlen;
    memcpy(out + 5, host, hlen);
    out[5 + hlen] = (unsigned char)((port >> 8) & 0xFF);
    out[6 + hlen] = (unsigned char)(port & 0xFF);
    return 7 + hlen;
}

int gw_fwd_socks_greet_reply(const unsigned char *buf, size_t len)
{
    if (buf == NULL || len < 2) return 0;
    if (buf[0] != 0x05) return -1;
    if (buf[1] == 0x00) return 1;
    return -1;              /* 0xFF: no acceptable methods, or anything else */
}

int gw_fwd_socks_conn_reply(const unsigned char *buf, size_t len)
{
    size_t need;

    if (buf == NULL || len < 4) return 0;
    if (buf[0] != 0x05) return -1;

    /*
     * The bound address that follows is of no interest -- the tunnel speaks
     * through the proxy, never to that address -- but its length depends on
     * the type, so read that first to know how many bytes make a reply.
     */
    switch (buf[3]) {
    case 0x01: need = 10; break;                        /* IPv4 */
    case 0x03:
        if (len < 5) return 0;
        need = (size_t)5 + buf[4] + 2;                  /* domain */
        break;
    case 0x04: need = 22; break;                        /* IPv6 */
    default:   return -1;
    }
    if (len < need) return 0;
    return buf[1] == 0x00 ? 1 : -1;
}
