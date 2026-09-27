/*
 * gw_fwd.h - forward-proxy handshake grammar for the tunnel module.
 *
 * PORTABLE: no Mac or Windows headers (CLAUDE.md rule 8). The tunnel speaks
 * to an upstream HTTP CONNECT or SOCKS5 proxy before handing the socket to
 * Certainly, and every byte of those handshakes is built and parsed here so
 * the host tests can exercise it with a plain Linux cc.
 *
 * Nothing here touches a socket. The caller moves bytes; these functions only
 * shape and recognise them.
 */
#ifndef GW_FWD_H
#define GW_FWD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_FWD_NONE   0
#define GW_FWD_HTTP   1
#define GW_FWD_SOCKS5 2

/*
 * "none" -> GW_FWD_NONE, "http"/"connect" -> GW_FWD_HTTP,
 * "socks5"/"socks" -> GW_FWD_SOCKS5. Case-insensitive. -1 when unknown.
 */
int gw_fwd_kind(const char *name);

/*
 * Shape "CONNECT host:port HTTP/1.0" plus Host and an optional
 * "Proxy-Authorization: Basic" line carrying already-encoded credentials.
 * b64 may be NULL for no auth. Returns the bytes written excluding the NUL,
 * or 0 when the request would not fit in cap.
 */
size_t gw_fwd_connect_req(const char *host, unsigned port, const char *b64,
                          char *out, size_t cap);

/*
 * The same request without the Host line, byte-for-byte what socat sends:
 * request line, optional Proxy-Authorization, blank line. See
 * tunnel_host_header; one proxy stalled the Host-carrying form while
 * passing this one. HTTP/1.0 does not require Host -- the authority is
 * already in the request line.
 */
size_t gw_fwd_connect_req_bare(const char *host, unsigned port,
                               const char *b64, char *out, size_t cap);

/*
 * Read a proxy's reply to the CONNECT request. Returns 1 once the full head
 * has arrived (status in *code, head length in *head_len), 0 when more bytes
 * are needed, -1 when the reply is not an HTTP status line at all.
 */
int gw_fwd_connect_reply(const char *buf, size_t len,
                         size_t *head_len, long *code);

/* The no-auth greeting: 05 01 00. Returns bytes written, 0 when no room. */
size_t gw_fwd_socks_greet(unsigned char *out, size_t cap);

/*
 * The connection request: 05 01 00 03 <len> host <port-hi> <port-lo>.
 * Host names only (no ATYP switch): an IPv4 literal travels as a name and
 * the proxy resolves it, which is also what keeps this free of inet_addr.
 * Returns bytes written, 0 when the host is empty/too long or out is small.
 */
size_t gw_fwd_socks_connect(const char *host, unsigned port,
                            unsigned char *out, size_t cap);

/*
 * Read the 2-byte greeting reply. 1: the server accepted no-auth.
 * 0: need more bytes. -1: refused (including 0xFF, no acceptable methods).
 */
int gw_fwd_socks_greet_reply(const unsigned char *buf, size_t len);

/*
 * Read the connection reply. 1: granted (REP 0). 0: need more bytes.
 * -1: refused, or a reply this parser does not understand. On a grant,
 * *used (when not NULL) is the reply's length: anything past it in buf is
 * the far end's first bytes, arrived in the same segment.
 */
int gw_fwd_socks_conn_reply(const unsigned char *buf, size_t len,
                            size_t *used);

#ifdef __cplusplus
}
#endif

#endif /* GW_FWD_H */
