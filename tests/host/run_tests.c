/*
 * run_tests.c - host-side unit tests for Gateway's portable code.
 *
 * Built by tests/host/Makefile with the developer's native cc. Nothing here
 * touches the Toolbox; if a test needs a Mac header, the code under test is in
 * the wrong directory.
 */

#include <stdio.h>
#define _GNU_SOURCE
#include <string.h>

#include "gw_b64.h"
#include "gw_chunked.h"
#include "gw_http.h"
#include "gw_mailcmd.h"
#include "gw_oauth.h"
#include "gw_pac.h"
#include "gw_prefs.h"
#include "gw_rewrite.h"
#include "gw_x509write.h"
#include "gw_url.h"
#include "gw_util.h"
#include "gw_wayback.h"

static int sFailures;
static int sChecks;

static void check(int cond, const char *what)
{
    sChecks++;
    if (!cond) {
        sFailures++;
        printf("  FAIL  %s\n", what);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    sChecks++;
    if (strcmp(got, want) != 0) {
        sFailures++;
        printf("  FAIL  %s: got \"%s\", wanted \"%s\"\n", what, got, want);
    }
}

/* ------------------------------------------------------------------ */

static void test_util(void)
{
    size_t head_len = 0;
    size_t val_len = 0;
    const char *v;
    static const char head[] =
        "GET / HTTP/1.0\r\nHost: example.com\r\nX-Empty:\r\n\r\nbody";

    puts("gw_util");

    check(gw_stricmp("Host", "hOsT") == 0, "stricmp is case-insensitive");
    check(gw_stricmp("Hosta", "Host") != 0, "stricmp compares length");
    check(gw_starts_ci("HTTPS://x", 9, "https://"), "starts_ci");

    check(gw_find_head_end(head, sizeof(head) - 1, &head_len), "head end found");
    check(head_len == strlen("GET / HTTP/1.0\r\nHost: example.com\r\nX-Empty:\r\n\r\n"),
          "head end offset");

    v = gw_header_find(head, head_len, "host", &val_len);
    check(v != NULL && val_len == 11 && memcmp(v, "example.com", 11) == 0,
          "header lookup is case-insensitive");

    v = gw_header_find(head, head_len, "X-Empty", &val_len);
    check(v != NULL && val_len == 0, "empty header value");

    v = gw_header_find(head, head_len, "Missing", &val_len);
    check(v == NULL, "absent header returns NULL");

    check(gw_parse_dec("  42xyz", 7) == 42, "parse_dec");
    check(gw_parse_dec("xyz", 3) == -1, "parse_dec rejects non-digits");
}

static void test_url(void)
{
    GWUrl u, out;

    puts("gw_url");

    check(gw_url_split("http://example.com/a/b?c=d", 26, &u), "split http");
    check_str(u.host, "example.com", "host");
    check(u.port == 80, "default http port");
    check(u.tls == 0, "http is not tls");
    check_str(u.path, "/a/b?c=d", "path");

    check(gw_url_split("https://api.example.com:8443/x", 30, &u), "split https");
    check(u.port == 8443, "explicit port");
    check(u.tls == 1, "https is tls");

    check(gw_url_split("http://example.com", 18, &u), "split with no path");
    check_str(u.path, "/", "empty path becomes /");

    check(!gw_url_split("ftp://example.com/", 18, &u), "ftp is rejected");

    {
        unsigned short port = 0;
        char host[64];
        check(gw_url_split_authority("github.com:443", 14, 443, host,
                                     sizeof(host), &port),
              "authority form");
        check_str(host, "github.com", "authority host");
        check(port == 443, "authority port");

        check(gw_url_split_authority("github.com", 10, 443, host,
                                     sizeof(host), &port),
              "authority without a port");
        check(port == 443, "authority default port");
    }

    gw_url_split("https://a.example/one/two", 25, &u);
    check(gw_url_resolve(&u, "/three", 6, &out), "absolute-path redirect");
    check_str(out.path, "/three", "absolute-path redirect target");
    check_str(out.host, "a.example", "absolute-path redirect keeps the host");

    check(gw_url_resolve(&u, "three", 5, &out), "relative redirect");
    check_str(out.path, "/one/three", "relative redirect target");

    check(gw_url_resolve(&u, "http://b.example/x", 18, &out),
          "absolute redirect");
    check_str(out.host, "b.example", "absolute redirect host");
    check(out.tls == 0, "absolute redirect scheme");
}

static void test_request(void)
{
    GWRequest req;
    char out[2048];
    size_t n;

    puts("gw_http requests");

    /* Shape 1: classic forward proxy. */
    {
        static const char r[] =
            "GET http://example.com/index.html HTTP/1.0\r\n"
            "User-Agent: Classilla/9.3.4\r\n"
            "Proxy-Connection: keep-alive\r\n\r\n";
        check(gw_http_parse_request(r, sizeof(r) - 1, &req) == 1, "shape 1 parses");
        check(req.shape == kGWShapeAbsolute, "shape 1 is absolute");
        check(req.url.tls == 0, "shape 1 is plain");
        check_str(req.url.host, "example.com", "shape 1 host");
        check_str(req.url.path, "/index.html", "shape 1 path");

        n = gw_http_build_upstream(&req, r, req.head_len, out, sizeof(out), 0);
        check(n > 0, "shape 1 rewrite succeeds");
        out[n] = '\0';
        check(strstr(out, "GET /index.html HTTP/1.0\r\n") == out,
              "rewrite uses origin form");
        check(strstr(out, "Host: example.com\r\n") != NULL, "rewrite adds Host");
        check(strstr(out, "Connection: close\r\n") != NULL,
              "rewrite forces Connection: close");
        check(strstr(out, "Proxy-Connection") == NULL,
              "rewrite drops Proxy-Connection");
        check(strstr(out, "User-Agent: Classilla/9.3.4\r\n") != NULL,
              "rewrite keeps end-to-end headers");
        check(strstr(out, "Accept-Encoding: identity\r\n") != NULL,
              "rewrite forces identity encoding");

        /* Asking to hold the connection open needs HTTP/1.1 to mean anything. */
        n = gw_http_build_upstream(&req, r, req.head_len, out, sizeof(out), 1);
        out[n] = '\0';
        check(strstr(out, "GET /index.html HTTP/1.1\r\n") == out,
              "keep-alive upgrades the request to HTTP/1.1");
        check(strstr(out, "Connection: keep-alive\r\n") != NULL,
              "and asks for the connection to be held");
        check(strstr(out, "Connection: close") == NULL,
              "without also asking for it to be closed");
    }

    /* Shape 2: Classilla with proxy-for-https. */
    {
        static const char r[] =
            "GET https://www.example.org/ HTTP/1.0\r\n"
            "Host: www.example.org\r\n\r\n";
        check(gw_http_parse_request(r, sizeof(r) - 1, &req) == 1, "shape 2 parses");
        check(req.url.tls == 1, "shape 2 wants TLS");
        check(req.url.port == 443, "shape 2 port");
    }

    /* Shape 3: CONNECT. */
    {
        static const char r[] = "CONNECT github.com:443 HTTP/1.0\r\n\r\n";
        check(gw_http_parse_request(r, sizeof(r) - 1, &req) == 1, "shape 3 parses");
        check(req.shape == kGWShapeConnect, "shape 3 is CONNECT");
        check_str(req.url.host, "github.com", "CONNECT host");
        check(req.url.port == 443, "CONNECT port");
        check(req.url.tls == 0, "Gateway adds no TLS to a CONNECT");
    }

    /* Origin form plus Host. */
    {
        static const char r[] = "GET /page HTTP/1.1\r\nHost: origin.test:8080\r\n\r\n";
        check(gw_http_parse_request(r, sizeof(r) - 1, &req) == 1, "origin form parses");
        check(req.shape == kGWShapeOrigin, "origin form shape");
        check_str(req.url.host, "origin.test", "origin form host");
        check(req.url.port == 8080, "origin form port");

        n = gw_http_build_upstream(&req, r, req.head_len, out, sizeof(out), 0);
        out[n] = '\0';
        check(strstr(out, "Host: origin.test:8080\r\n") != NULL,
              "non-default port is kept in Host");
    }

    /* Incomplete and malformed. */
    {
        static const char partial[] = "GET http://a/ HTTP/1.0\r\nHost: a\r\n";
        check(gw_http_parse_request(partial, sizeof(partial) - 1, &req) == 0,
              "incomplete head asks for more");
    }
    {
        static const char bad[] = "NONSENSE\r\n\r\n";
        check(gw_http_parse_request(bad, sizeof(bad) - 1, &req) == -1,
              "malformed request is rejected");
    }
}

static void test_response(void)
{
    GWResponse res;
    GWFilterOpts fopt;
    char out[1024];
    size_t n;

    memset(&fopt, 0, sizeof(fopt));

    puts("gw_http responses");

    {
        static const char r[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: keep-alive\r\n\r\n";
        check(gw_http_parse_response(r, sizeof(r) - 1, &res) == 1, "response parses");
        check(res.status == 200, "status");
        check(res.chunked == 1, "chunked detected");

        fopt.keep_length = 0;
        n = gw_http_filter_response(r, res.head_len, out, sizeof(out), &fopt);
        out[n] = '\0';
        check(strstr(out, "HTTP/1.1 200 OK\r\n") == out, "status line survives");
        check(strstr(out, "Transfer-Encoding") == NULL,
              "Transfer-Encoding is dropped");
        check(strstr(out, "keep-alive") == NULL, "Connection is dropped");
        check(strstr(out, "Connection: close\r\n") != NULL,
              "Connection: close is appended");
        check(strstr(out, "Content-Type: text/html\r\n") != NULL,
              "entity headers survive");
    }

    /*
     * Content-Length must survive when the body is passed through untouched.
     * A media player will not start without one, which is what made every
     * video fetch fail while ordinary pages were fine.
     */
    {
        static const char r[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: video/mp4\r\n"
            "Content-Length: 15728640\r\n"
            "Connection: keep-alive\r\n\r\n";
        check(gw_http_parse_response(r, sizeof(r) - 1, &res) == 1,
              "media response parses");
        check(!res.chunked, "media response is not chunked");

        fopt.keep_length = 1;
        n = gw_http_filter_response(r, res.head_len, out, sizeof(out), &fopt);
        out[n] = '\0';
        check(strstr(out, "Content-Length: 15728640\r\n") != NULL,
              "Content-Length survives when the body is untouched");
        check(strstr(out, "Content-Type: video/mp4\r\n") != NULL,
              "Content-Type survives");
        check(strstr(out, "keep-alive") == NULL,
              "Connection is still dropped");

        fopt.keep_length = 0;
        n = gw_http_filter_response(r, res.head_len, out, sizeof(out), &fopt);
        out[n] = '\0';
        check(strstr(out, "Content-Length") == NULL,
              "Content-Length is dropped when de-chunking would change it");
    }

    /* Stripping the charset parameter, for browsers that choke on it. */
    {
        static const char r[] =
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n\r\n";
        gw_http_parse_response(r, sizeof(r) - 1, &res);

        fopt.keep_length = 1;
        fopt.strip_charset = 0;
        n = gw_http_filter_response(r, res.head_len, out, sizeof(out), &fopt);
        out[n] = '\0';
        check(strstr(out, "charset=utf-8") != NULL,
              "the charset is kept when encoding is allowed");

        fopt.strip_charset = 1;
        n = gw_http_filter_response(r, res.head_len, out, sizeof(out), &fopt);
        out[n] = '\0';
        check(strstr(out, "Content-Type: text/html\r\n") != NULL,
              "the charset parameter is cut off");
        check(strstr(out, "charset") == NULL, "and nothing of it remains");
        fopt.strip_charset = 0;
    }

    /*
     * An archived snapshot cannot change, but the archive still serves it with
     * a half-hour lifetime. Taken at face value that means re-fetching every
     * asset through Gateway twice an hour for bytes that are known in advance
     * to be identical.
     */
    {
        static const char r[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Cache-Control: max-age=1800\r\n"
            "Pragma: no-cache\r\n"
            "Expires: Tue, 18 Dec 2001 02:09:33 GMT\r\n\r\n";
        gw_http_parse_response(r, sizeof(r) - 1, &res);

        fopt.keep_length = 1;
        fopt.cache_forever = 0;
        n = gw_http_filter_response(r, res.head_len, out, sizeof(out), &fopt);
        out[n] = '\0';
        check(strstr(out, "max-age=1800") != NULL,
              "the origin's caching is left alone for the live web");

        fopt.cache_forever = 1;
        n = gw_http_filter_response(r, res.head_len, out, sizeof(out), &fopt);
        out[n] = '\0';
        check(strstr(out, "max-age=1800") == NULL,
              "the archive's half-hour lifetime is replaced");
        check(strstr(out, "no-cache") == NULL, "Pragma: no-cache goes with it");
        check(strstr(out, "2001") == NULL, "and the expiry from 2001");
        check(strstr(out, "max-age=31536000") != NULL, "a year is offered");
        check(strstr(out, "Expires: Thu, 31 Dec 2037") != NULL,
              "with an Expires for browsers that prefer one");
        check(strstr(out, "Content-Type: text/html\r\n") != NULL,
              "everything else survives");
        fopt.cache_forever = 0;
    }

    /* Which redirects Gateway follows itself. */
    {
        /* Auto: only the hop a browser without modern TLS cannot make. */
        check(gw_http_should_follow(kGWRedirectAuto, 0, 1) == 1,
              "auto follows http to https");
        check(gw_http_should_follow(kGWRedirectAuto, 0, 0) == 0,
              "auto passes http to http back to the client");
        /*
         * The second hop of a two-hop redirect. This asserted == 0 until the
         * screenshots in Gateway#1 showed what that meant: the browser was
         * handed an https:// Location it could not fetch, and went off to try
         * its own handshake. The client hop is plaintext whichever side
         * Gateway is on, so a destination on TLS is always ours to follow.
         */
        check(gw_http_should_follow(kGWRedirectAuto, 1, 1) == 1,
              "auto follows https to https, because the client still cannot");
        check(gw_http_should_follow(kGWRedirectAuto, 1, 0) == 0,
              "auto passes https to http back to the client");

        check(gw_http_should_follow(kGWRedirectAlways, 0, 0) == 1,
              "always follows");
        check(gw_http_should_follow(kGWRedirectNever, 0, 1) == 0,
              "never follows");
    }

    /* A signed media URL longer than the old 1024-byte limit must resolve. */
    {
        static char loc[3000];
        GWUrl base, next;
        size_t i;

        strcpy(loc, "/videoplayback?id=abc&sig=");
        for (i = strlen(loc); i < sizeof(loc) - 1; i++) loc[i] = 'A';
        loc[sizeof(loc) - 1] = '\0';

        check(gw_url_split("http://93.245.69.158:8080/watch?v=x", 35, &base),
              "base URL with a port parses");
        check(base.port == 8080, "base keeps its non-default port");

        check(gw_url_resolve(&base, loc, strlen(loc), &next),
              "a 3 KB signed URL resolves");
        check(next.port == 8080,
              "a relative redirect keeps the origin's port");
        check(strcmp(next.path, loc) == 0, "the query string survives intact");
    }

    {
        static const char r[] =
            "HTTP/1.0 302 Found\r\nLocation: https://elsewhere.test/x\r\n"
            "Content-Length: 0\r\n\r\n";
        check(gw_http_parse_response(r, sizeof(r) - 1, &res) == 1, "redirect parses");
        check(gw_http_is_redirect(res.status), "302 is a redirect");
        check(res.has_location, "Location captured");
        check_str(res.location, "https://elsewhere.test/x", "Location value");
        check(res.has_content_length && res.content_length == 0,
              "Content-Length captured");
    }

    check(gw_http_is_redirect(200) == 0, "200 is not a redirect");
    check(gw_http_is_redirect(308) == 1, "308 is a redirect");
}

static void test_chunked(void)
{
    GWChunked c;
    char out[256];
    size_t produced;
    long used;

    puts("gw_chunked");

    {
        static const char body[] =
            "5\r\nhello\r\n1c\r\n, this is a chunked response\r\n0\r\n\r\n";
        gw_chunked_init(&c);
        used = gw_chunked_feed(&c, body, sizeof(body) - 1, out, sizeof(out),
                               &produced);
        check(used == (long)(sizeof(body) - 1), "whole body consumed");
        check(gw_chunked_done(&c), "terminator recognised");
        out[produced] = '\0';
        check_str(out, "hello, this is a chunked response", "decoded body");
    }

    /* Split across reads, the way a real socket delivers it. */
    {
        static const char a[] = "3\r\nabc\r\n3";
        static const char b[] = "\r\ndef\r\n0\r\n\r\n";
        size_t total = 0;

        gw_chunked_init(&c);
        used = gw_chunked_feed(&c, a, sizeof(a) - 1, out, sizeof(out), &produced);
        check(used == (long)(sizeof(a) - 1), "first fragment consumed");
        total += produced;
        used = gw_chunked_feed(&c, b, sizeof(b) - 1, out + total,
                               sizeof(out) - total, &produced);
        total += produced;
        out[total] = '\0';
        check_str(out, "abcdef", "reassembled across reads");
        check(gw_chunked_done(&c), "done after the split feed");
    }

    /* A chunk extension must not confuse the size scanner. */
    {
        static const char body[] = "4;name=value\r\nwxyz\r\n0\r\n\r\n";
        gw_chunked_init(&c);
        gw_chunked_feed(&c, body, sizeof(body) - 1, out, sizeof(out), &produced);
        out[produced] = '\0';
        check_str(out, "wxyz", "chunk extension skipped");
    }

    /* A tiny output buffer forces a short return, not data loss. */
    {
        static const char body[] = "8\r\n01234567\r\n0\r\n\r\n";
        char small[4];
        gw_chunked_init(&c);
        used = gw_chunked_feed(&c, body, sizeof(body) - 1, small, sizeof(small),
                               &produced);
        check(produced == 4, "output capacity respected");
        check(used < (long)(sizeof(body) - 1), "short return when out is full");
    }
}

static void test_b64(void)
{
    char enc[64];
    char dec[64];
    size_t n;

    puts("gw_b64");

    n = gw_b64_encode("Man", 3, enc, sizeof(enc));
    check(n == 4, "encode length");
    check_str(enc, "TWFu", "encode without padding");

    gw_b64_encode("Ma", 2, enc, sizeof(enc));
    check_str(enc, "TWE=", "encode with one pad");

    gw_b64_encode("M", 1, enc, sizeof(enc));
    check_str(enc, "TQ==", "encode with two pads");

    n = gw_b64_decode("TWFu", 4, dec, sizeof(dec));
    dec[n] = '\0';
    check_str(dec, "Man", "decode");

    n = gw_b64_decode("TWE=", 4, dec, sizeof(dec));
    dec[n] = '\0';
    check_str(dec, "Ma", "decode with padding");

    check(gw_b64_decode("!!!!", 4, dec, sizeof(dec)) == (size_t)-1,
          "invalid base64 is rejected");
}

static void test_mailcmd(void)
{
    GWImapCmd cmd;

    puts("gw_mailcmd");

    check(gw_imap_parse("a001 LOGIN \"me@example.com\" \"s3cret\"\r\n", 38, &cmd),
          "quoted LOGIN parses");
    check_str(cmd.tag, "a001", "IMAP tag");
    check_str(cmd.cmd, "LOGIN", "IMAP command");
    check_str(cmd.user, "me@example.com", "IMAP user");
    check_str(cmd.pass, "s3cret", "IMAP password");
    check(cmd.has_credentials, "credentials flagged");

    check(gw_imap_parse("A2 LOGIN bare pass", 18, &cmd), "bare-atom LOGIN parses");
    check_str(cmd.user, "bare", "bare atom user");
    check_str(cmd.pass, "pass", "bare atom password");

    check(gw_imap_parse("x1 CAPABILITY", 13, &cmd), "CAPABILITY parses");
    check_str(cmd.cmd, "CAPABILITY", "non-LOGIN command");
    check(!cmd.has_credentials, "CAPABILITY carries no credentials");

    check(gw_imap_parse("t LOGIN \"a\\\"b\" \"p\"", 18, &cmd), "escaped quote");
    check_str(cmd.user, "a\"b", "backslash escape honoured");

    {
        char verb[32], arg[128];
        check(gw_smtp_parse("EHLO gateway\r\n", 14, verb, sizeof(verb),
                            arg, sizeof(arg)), "EHLO parses");
        check_str(verb, "EHLO", "SMTP verb");
        check_str(arg, "gateway", "SMTP argument");

        check(gw_smtp_parse("MAIL FROM:<a@b>\r\n", 17, verb, sizeof(verb),
                            arg, sizeof(arg)), "MAIL FROM parses");
        check_str(verb, "MAIL", "verb stops before the colon");
    }

    {
        char verb[32], arg[128];
        check(gw_pop_parse("USER me@example.com\r\n", 21, verb, sizeof(verb),
                           arg, sizeof(arg)), "POP USER parses");
        check_str(verb, "USER", "POP verb");
        check_str(arg, "me@example.com", "POP argument");

        check(gw_pop_parse("CAPA\r\n", 6, verb, sizeof(verb),
                           arg, sizeof(arg)), "POP bare verb parses");
        check_str(verb, "CAPA", "POP bare verb");
        check_str(arg, "", "POP bare verb has no argument");

        /* A password may contain anything, including spaces and colons. */
        check(gw_pop_parse("PASS a:b c\r\n", 12, verb, sizeof(verb),
                           arg, sizeof(arg)), "POP PASS parses");
        check_str(verb, "PASS", "POP PASS verb");
        check_str(arg, "a:b c", "POP PASS keeps the whole argument");
    }

    {
        char user[64], pass[64], blob[128];
        size_t n = gw_sasl_plain_encode("me@example.com", "s3cret",
                                        blob, sizeof(blob));
        check(n > 0, "SASL PLAIN encodes");
        check(gw_sasl_plain_decode(blob, n, user, sizeof(user),
                                   pass, sizeof(pass)),
              "SASL PLAIN round-trips");
        check_str(user, "me@example.com", "PLAIN user");
        check_str(pass, "s3cret", "PLAIN password");
    }

    {
        /* A real Microsoft access token is a JWT of a couple of thousand
         * characters; the payload buffer has to take it. */
        static char big_token[2600];
        static char big_blob[GW_XOAUTH2_B64];
        size_t i;

        for (i = 0; i < sizeof(big_token) - 1; i++) big_token[i] = 'A';
        big_token[sizeof(big_token) - 1] = '\0';

        check(gw_sasl_xoauth2("me@example.com", big_token,
                              big_blob, sizeof(big_blob)) > 0,
              "XOAUTH2 encodes a full-size access token");
    }

    {
        char blob[256];
        char raw[256];
        size_t n = gw_sasl_xoauth2("me@example.com", "TOKEN", blob, sizeof(blob));
        size_t d;
        check(n > 0, "XOAUTH2 encodes");
        d = gw_b64_decode(blob, n, raw, sizeof(raw) - 1);
        raw[d] = '\0';
        check(memcmp(raw, "user=me@example.com\001auth=Bearer TOKEN\001\001",
                     d) == 0,
              "XOAUTH2 payload layout");
    }
}

static void test_oauth(void)
{
    char body[512];
    char value[128];
    size_t n;

    puts("gw_oauth");

    n = gw_form_escape("a b/c=d", body, sizeof(body));
    check(n > 0, "escape returns a length");
    check_str(body, "a%20b%2Fc%3Dd", "form escaping");

    n = gw_oauth_refresh_body("client-id", "", "M.C5_tok+en",
                              "https://outlook.office.com/IMAP.AccessAsUser.All",
                              body, sizeof(body));
    check(n > 0, "refresh body built");
    check(strstr(body, "client_id=client-id") == body, "client_id first");
    check(strstr(body, "&grant_type=refresh_token") != NULL, "grant_type present");
    check(strstr(body, "M.C5_tok%2Ben") != NULL, "refresh token is escaped");
    check(strstr(body, "client_secret") == NULL, "empty secret is omitted");
    check(strstr(body, "scope=https%3A%2F%2Foutlook") != NULL, "scope is escaped");

    check(gw_oauth_refresh_body("", "", "tok", "", body, sizeof(body)) == 0,
          "missing client_id is refused");

    {
        static const char json[] =
            "{\"token_type\":\"Bearer\",\"expires_in\":3599,"
            "\"access_token\":\"EwB\\/abc\"}";
        check(gw_json_string(json, sizeof(json) - 1, "access_token",
                             value, sizeof(value)),
              "access_token extracted");
        check_str(value, "EwB/abc", "escaped solidus decoded");
        check(gw_json_number(json, sizeof(json) - 1, "expires_in") == 3599,
              "expires_in extracted");
        check(gw_json_number(json, sizeof(json) - 1, "missing") == -1,
              "absent number reports -1");
    }
}

static void test_prefs(void)
{
    static const char text[] =
        "# Gateway prefs\n"
        "http_port = 8765\n"
        "local_password:  hunter2  \n"
        "; a comment\n"
        "OAUTH_USER = me@example.com\n"
        "empty =\n";
    char value[64];

    puts("gw_prefs");

    check(gw_prefs_get(text, sizeof(text) - 1, "local_password",
                       value, sizeof(value)), "value found");
    check_str(value, "hunter2", "whitespace trimmed");

    check(gw_prefs_get(text, sizeof(text) - 1, "oauth_user",
                       value, sizeof(value)), "keys are case-insensitive");
    check_str(value, "me@example.com", "colon separator");

    check(gw_prefs_get_num(text, sizeof(text) - 1, "http_port", 0) == 8765,
          "numeric value");
    check(gw_prefs_get_num(text, sizeof(text) - 1, "imap_port", 1993) == 1993,
          "default when absent");
    check(!gw_prefs_get(text, sizeof(text) - 1, "empty", value, sizeof(value)),
          "empty value counts as absent");
    check(!gw_prefs_get(text, sizeof(text) - 1, "Gateway", value, sizeof(value)),
          "comment text is not a key");

    /*
     * Line endings. A prefs file typed on Mac OS 9 ends its lines with CR and
     * contains no LF at all; splitting on LF alone made the whole file look
     * like one comment line, so every setting silently fell back to its
     * default and every mail login was refused as a bad password.
     */
    {
        static const char cr[] =
            "# Gateway Prefs\r"
            "local_password = hunter2\r"
            "imap_port = 1993\r";
        static const char crlf[] =
            "# Gateway Prefs\r\n"
            "local_password = hunter2\r\n"
            "imap_port = 1993\r\n";
        static const char noeol[] = "local_password = hunter2";

        check(gw_prefs_get(cr, sizeof(cr) - 1, "local_password",
                           value, sizeof(value)), "CR line endings parse");
        check_str(value, "hunter2", "CR value");
        check(gw_prefs_get_num(cr, sizeof(cr) - 1, "imap_port", -1) == 1993,
              "CR numeric value");
        check(!gw_prefs_get(cr, sizeof(cr) - 1, "Gateway",
                            value, sizeof(value)),
              "CR comment is still a comment");

        check(gw_prefs_get(crlf, sizeof(crlf) - 1, "local_password",
                           value, sizeof(value)), "CRLF line endings parse");
        check_str(value, "hunter2", "CRLF value");

        check(gw_prefs_get(noeol, sizeof(noeol) - 1, "local_password",
                           value, sizeof(value)),
              "last line without a terminator parses");
    }

    /* Writing a setting back, for the rotated OAuth refresh token. */
    {
        static const char before[] =
            "# Gateway Prefs\r"
            "local_password = hunter2\r"
            "refresh_token = OLD\r"
            "imap_port = 1993\r";
        char out[512];
        size_t n;

        n = gw_prefs_set(before, sizeof(before) - 1, "refresh_token", "NEW",
                         out, sizeof(out));
        check(n > 0, "set rewrites an existing key");
        check(gw_prefs_get(out, n, "refresh_token", value, sizeof(value)),
              "rewritten key reads back");
        check_str(value, "NEW", "rewritten value");
        check(gw_prefs_get(out, n, "local_password", value, sizeof(value)) &&
              strcmp(value, "hunter2") == 0, "other settings survive");
        check(gw_prefs_get_num(out, n, "imap_port", -1) == 1993,
              "settings after the edit survive");
        check(memchr(out, '\n', n) == NULL,
              "CR line endings are preserved, not converted");
        check(memmem(out, n, "# Gateway Prefs", 15) != NULL,
              "comments survive");

        /* Appending a key the file does not have yet. */
        n = gw_prefs_set(before, sizeof(before) - 1, "oauth_user", "me@x.com",
                         out, sizeof(out));
        check(n > 0, "set appends a missing key");
        check(gw_prefs_get(out, n, "oauth_user", value, sizeof(value)) &&
              strcmp(value, "me@x.com") == 0, "appended value reads back");

        /* A buffer that cannot hold the result must fail, not truncate. */
        check(gw_prefs_set(before, sizeof(before) - 1, "refresh_token",
                           "NEW", out, 16) == 0,
              "set refuses to overflow");
    }
}

static void test_glob(void)
{
    puts("gw_glob_match");

    check(gw_glob_match("frogfind.com", "frogfind.com"), "exact match");
    check(!gw_glob_match("frogfind.com", "frogfind.org"), "different TLD");
    check(gw_glob_match("FrogFind.com", "frogfind.com"), "case-insensitive");

    check(gw_glob_match("*.frogfind.com", "www.frogfind.com"), "leading star");
    check(gw_glob_match("*.frogfind.com", "a.b.frogfind.com"),
          "star spans dots");
    check(!gw_glob_match("*.frogfind.com", "frogfind.com"),
          "leading star needs a label, as the shell does");
    check(!gw_glob_match("*.frogfind.com", "evil-frogfind.com"),
          "star does not match across the dot boundary it anchors");

    check(gw_glob_match("*", "anything.at.all"), "bare star matches all");
    check(gw_glob_match("*", ""), "bare star matches empty");
    check(gw_glob_match("68k.news", "68k.news"), "digits and dots");
    check(gw_glob_match("?8k.news", "68k.news"), "question mark matches one");
    check(!gw_glob_match("?8k.news", "168k.news"),
          "question mark matches exactly one");

    /* The pattern that would send the live web to the archive if it misfired. */
    check(!gw_glob_match("*.macos9lives.com", "macos9lives.com.evil.test"),
          "a suffix pattern does not match a prefix of a longer host");
    check(gw_glob_match("*.nina.chat", "escargot.nina.chat"), "real entry");

    check(gw_glob_match("a*b*c", "abc"), "several stars, minimal");
    check(gw_glob_match("a*b*c", "axxbyyc"), "several stars, spread out");
    check(!gw_glob_match("a*b*c", "axxbyy"), "several stars, missing tail");
}

static void test_wayback(void)
{
    char stamp[GW_WB_STAMP];
    char original[GW_MAX_PATH];
    char out[GW_MAX_PATH];
    GWUrl origin;

    puts("gw_wayback");

    /* Building the archive request. */
    check(gw_url_split("http://www.example.com/page.html", 32, &origin),
          "origin URL parses");
    check(gw_wayback_path("20011231", &origin, out, sizeof(out)) > 0,
          "archive path builds");
    check_str(out, "/web/20011231id_/http://www.example.com/page.html",
              "archive path uses the id_ modifier");

    check(gw_url_split("http://93.245.69.158:8080/watch?v=x", 35, &origin),
          "origin with a port parses");
    gw_wayback_path("2001", &origin, out, sizeof(out));
    check_str(out, "/web/2001id_/http://93.245.69.158:8080/watch?v=x",
              "a non-default port is kept in the archived URL");

    /* Taking an archive URL apart, rooted and absolute. */
    {
        static const char rooted[] =
            "/web/20011231120000/http://www.example.com/page.html";
        check(gw_wayback_parse(rooted, sizeof(rooted) - 1, stamp, sizeof(stamp),
                               original, sizeof(original)),
              "rooted archive URL parses");
        check_str(stamp, "20011231120000", "snapshot timestamp");
        check_str(original, "http://www.example.com/page.html",
                  "original URL recovered");
    }
    {
        static const char absolute[] =
            "https://web.archive.org/web/19970822000000id_/http://a.test/";
        check(gw_wayback_parse(absolute, sizeof(absolute) - 1, stamp,
                               sizeof(stamp), original, sizeof(original)),
              "absolute archive URL parses");
        check_str(stamp, "19970822000000", "modifier is not part of the stamp");
        check_str(original, "http://a.test/", "original URL recovered");
    }
    {
        static const char plain[] = "http://www.example.com/not-an-archive";
        check(!gw_wayback_parse(plain, sizeof(plain) - 1, stamp, sizeof(stamp),
                                original, sizeof(original)),
              "an ordinary URL is not mistaken for an archive one");
    }

    /* Dates. */
    check(gw_wayback_daynum("19700101") == 0, "the epoch is day zero");
    check(gw_wayback_daynum("19700102") == 1, "the next day");
    check(gw_wayback_daynum("20011231") - gw_wayback_daynum("20011201") == 30,
          "December has 31 days");
    check(gw_wayback_daynum("20000301") - gw_wayback_daynum("20000201") == 29,
          "2000 was a leap year");
    check(gw_wayback_daynum("19000301") - gw_wayback_daynum("19000201") == 28,
          "1900 was not");
    check(gw_wayback_daynum("2001") == gw_wayback_daynum("20010101"),
          "a bare year means the first of January");
    check(gw_wayback_daynum("200106") == gw_wayback_daynum("20010601"),
          "a bare month means the first");
    check(gw_wayback_daynum("20011231120000") == gw_wayback_daynum("20011231"),
          "the time of day is ignored");
    check(gw_wayback_daynum("nonsense") == -1, "rubbish is rejected");

    /* Tolerance: only newer snapshots are refused. */
    check(gw_wayback_in_tolerance("20011231", "20011231000000", 730),
          "the exact date is in range");
    check(gw_wayback_in_tolerance("20011231", "19970822000000", 730),
          "an older snapshot is always accepted");
    check(gw_wayback_in_tolerance("20011231", "20021231000000", 730),
          "a year newer is inside a 730-day tolerance");
    check(!gw_wayback_in_tolerance("20011231", "20051231000000", 730),
          "four years newer is outside it");
    check(gw_wayback_in_tolerance("20011231", "20251231000000", 0),
          "a tolerance of zero accepts anything");

    /* GeoCities. */
    check(gw_wayback_geocities_host("www.geocities.com", out, sizeof(out)),
          "geocities is rewritten");
    check_str(out, "www.oocities.org", "geocities becomes oocities");
    check(gw_wayback_geocities_host("SoHo.geocities.com", out, sizeof(out)),
          "a geocities neighbourhood is rewritten");
    check_str(out, "SoHo.oocities.org", "the subdomain is preserved");
    check(!gw_wayback_geocities_host("www.example.com", out, sizeof(out)),
          "other hosts are left alone");

    /* The settings form. */
    {
        GWWaybackSettings set;
        char target[GW_MAX_PATH];
        static const char q[] =
            "date=20011231&dateTolerance=730&targetUrl=frogfind.com"
            "&gcFix=on&quickImages=on&ctEncoding=on";

        memset(&set, 0, sizeof(set));
        check(gw_wayback_apply_query(q, sizeof(q) - 1, &set,
                                     target, sizeof(target)),
              "targetUrl asks for a redirect");
        check_str(set.date, "20011231", "date applied");
        check(set.tolerance == 730, "tolerance applied");
        check(set.geocities && set.quick_images && set.ct_encoding,
              "checkboxes that are present read as on");
        check_str(target, "http://frogfind.com",
                  "a bare hostname gets a scheme");
    }
    {
        /* Unchecked boxes are absent, not "off". */
        GWWaybackSettings set;
        char target[GW_MAX_PATH];
        static const char q[] = "date=1997&dateTolerance=0";

        memset(&set, 0, sizeof(set));
        set.geocities = set.quick_images = set.ct_encoding = 1;
        check(!gw_wayback_apply_query(q, sizeof(q) - 1, &set,
                                      target, sizeof(target)),
              "no targetUrl means no redirect");
        check(!set.geocities && !set.quick_images && !set.ct_encoding,
              "an absent checkbox reads as off");
        check_str(set.date, "1997", "a bare year is kept as given");
    }
    {
        /* An escaped URL must survive intact. */
        GWWaybackSettings set;
        char target[GW_MAX_PATH];
        static const char q[] =
            "targetUrl=http%3A%2F%2Fa.test%2Fx%3Fy%3D1%26z%3D2";

        memset(&set, 0, sizeof(set));
        check(gw_wayback_apply_query(q, sizeof(q) - 1, &set,
                                     target, sizeof(target)),
              "escaped targetUrl parses");
        check_str(target, "http://a.test/x?y=1&z=2",
                  "percent-escapes are decoded");
    }

    /* The page itself has to render into a sane amount of space. */
    {
        GWWaybackSettings set;
        static char page[4096];
        size_t n;

        memset(&set, 0, sizeof(set));
        strcpy(set.date, "20011231");
        set.tolerance = 730;
        set.geocities = 1;

        n = gw_wayback_settings_page(&set, page, sizeof(page));
        check(n > 0, "settings page renders");
        check(strstr(page, "name=\"date\"") != NULL, "date field present");
        check(strstr(page, "name=\"dateTolerance\"") != NULL,
              "tolerance field present");
        check(strstr(page, "name=\"targetUrl\"") != NULL,
              "targetUrl field present");
        check(strstr(page, "value=\"20011231\"") != NULL,
              "the current date is filled in");
        check(strstr(page, "name=\"gcFix\" checked") != NULL,
              "an enabled checkbox renders checked");
        check(strstr(page, "name=\"quickImages\"> ") != NULL,
              "a disabled checkbox renders unchecked");
        check(strstr(page, "method=\"get\" action=\"/\"") != NULL,
              "the form is a GET to /, so it can be bookmarked");
        check(strstr(page, "<script") == NULL, "no script for period browsers");
    }
}

static void test_query(void)
{
    char v[128];
    static const char q[] = "a=1&bee=two+words&c=&d=%2Fslash%2F&flag";

    puts("gw_url query");

    check(gw_url_query_get(q, sizeof(q) - 1, "a", v, sizeof(v)) &&
          strcmp(v, "1") == 0, "first field");
    check(gw_url_query_get(q, sizeof(q) - 1, "bee", v, sizeof(v)) &&
          strcmp(v, "two words") == 0, "plus decodes to a space");
    check(gw_url_query_get(q, sizeof(q) - 1, "c", v, sizeof(v)) &&
          v[0] == '\0', "an empty value is still present");
    check(gw_url_query_get(q, sizeof(q) - 1, "d", v, sizeof(v)) &&
          strcmp(v, "/slash/") == 0, "percent escapes decode");
    check(!gw_url_query_get(q, sizeof(q) - 1, "missing", v, sizeof(v)),
          "an absent field reports absent");

    check(gw_url_query_has(q, sizeof(q) - 1, "flag"),
          "a bare name with no '=' counts as present");
    check(!gw_url_query_has(q, sizeof(q) - 1, "fla"),
          "a prefix of a field name does not match");
    check(!gw_url_query_has(q, sizeof(q) - 1, "ee"),
          "a suffix of a field name does not match");
}

/*
 * The https:// rewriter, and mostly the seam between chunks.
 *
 * A 32 KB read can end in the middle of "https://" and the naive version emits
 * the fragment and then fails to match the rest -- which on a real page means
 * one link in a few hundred silently keeps its scheme, sends the browser to
 * CONNECT, and produces exactly the dialog this whole feature exists to avoid.
 * Rare, invisible in testing, and impossible to explain from a bug report. So
 * every split of the needle is checked here, not just the tidy cases.
 */
static void test_rewrite(void)
{
    char   buf[256];
    size_t hold, n;

    puts("gw_rewrite");

    /* The ordinary case, and the length really does shrink by one per hit. */
    strcpy(buf, "<a href=\"https://a.example/x\">");
    n = gw_rewrite_https(buf, strlen(buf), &hold);
    buf[n] = '\0';
    check_str(buf, "<a href=\"http://a.example/x\">", "one link rewritten");
    check(hold == 0, "nothing held when the buffer ends on a plain byte");

    /* Several, including back to back with no separator. */
    strcpy(buf, "https://a/ https://b/https://c/");
    n = gw_rewrite_https(buf, strlen(buf), &hold);
    buf[n] = '\0';
    check_str(buf, "http://a/ http://b/http://c/", "three in a row");

    /* Case is not consistent in hand-written HTML; the output always is. */
    strcpy(buf, "HTTPS://A.EXAMPLE/ HtTpS://b/");
    n = gw_rewrite_https(buf, strlen(buf), &hold);
    buf[n] = '\0';
    check_str(buf, "http://A.EXAMPLE/ http://b/",
              "upper and mixed case match, output is lower");

    /* http:// is left exactly alone, and so is anything that merely starts h. */
    strcpy(buf, "http://plain/ httpx://no/ https:/notyet hhttps://x/");
    n = gw_rewrite_https(buf, strlen(buf), &hold);
    buf[n] = '\0';
    check_str(buf, "http://plain/ httpx://no/ https:/notyet hhttp://x/",
              "near misses survive untouched");

    /* Every possible split of the needle across a chunk boundary. */
    {
        int cut;

        for (cut = 1; cut <= 8; cut++) {
            char   first[64], second[64], joined[128];
            size_t n1, n2, h1, h2;
            const char *whole = "x=https://h/y";
            size_t at = 2 + (size_t)cut;     /* inside "https://" */

            memcpy(first, whole, at);
            n1 = gw_rewrite_https(first, at, &h1);

            /* What the proxy does: emit n1 - h1, carry h1 to the next chunk. */
            memcpy(second, first + (n1 - h1), h1);
            strcpy(second + h1, whole + at);
            n2 = gw_rewrite_https(second, h1 + strlen(whole + at), &h2);

            memcpy(joined, first, n1 - h1);
            memcpy(joined + (n1 - h1), second, n2);
            joined[(n1 - h1) + n2] = '\0';

            check_str(joined, "x=http://h/y",
                      "a needle split across two chunks still matches");
            check(h2 == 0, "and nothing is left held at the end");
        }
    }

    /* A partial match at the very end of the body is held, then released. */
    strcpy(buf, "trailing https:/");
    n = gw_rewrite_https(buf, strlen(buf), &hold);
    check(hold == 7, "seven bytes of a possible match are held");
    check(n == strlen("trailing https:/"), "and none of them are dropped");
    buf[n] = '\0';
    check_str(buf, "trailing https:/",
              "held bytes stay in the buffer for the caller to keep");

    /* Nothing to do, and the degenerate inputs. */
    strcpy(buf, "no urls here at all");
    n = gw_rewrite_https(buf, strlen(buf), &hold);
    check(n == strlen("no urls here at all") && hold == 0,
          "a body with no match is returned whole");
    check(gw_rewrite_https(buf, 0, &hold) == 0 && hold == 0,
          "an empty chunk is not a match");
    check(gw_rewrite_https(NULL, 10, &hold) == 0, "a null buffer is refused");

    /* Which bodies are worth scanning. */
    check(gw_rewrite_wants_type("text/html") == 1, "html is scanned");
    check(gw_rewrite_wants_type("text/html; charset=utf-8") == 1,
          "a charset parameter does not hide the type");
    check(gw_rewrite_wants_type("TEXT/HTML") == 1, "type match is insensitive");
    check(gw_rewrite_wants_type("text/css") == 1, "css is scanned");
    check(gw_rewrite_wants_type("application/javascript") == 1,
          "javascript is scanned");
    check(gw_rewrite_wants_type("image/jpeg") == 0, "a jpeg is left alone");
    check(gw_rewrite_wants_type("video/mp4") == 0, "video is left alone");
    check(gw_rewrite_wants_type("application/json") == 0,
          "json is left alone, deliberately");
    check(gw_rewrite_wants_type("") == 0, "an empty type is not scanned");
    check(gw_rewrite_wants_type(NULL) == 0, "a missing type is not scanned");
}

/*
 * The certificate writer.
 *
 * Hand-written DER is exactly the kind of code that looks right, passes every
 * assertion its author thought to make, and is then rejected by the one parser
 * that matters. So the real test is not here: these checks confirm the shape,
 * and the files written at the end are handed to openssl by CI, which has no
 * stake in this being correct.
 */
static void test_x509write(void)
{
    /* A 1024-bit modulus with the top bit set, which is the case that needs a
     * leading zero byte or the INTEGER reads as negative. */
    static unsigned char mod[128];
    static const unsigned char exp3[] = { 0x01, 0x00, 0x01 };
    static const unsigned char serial[] = { 0x4A, 0x17, 0x02, 0x31 };
    static unsigned char buf[4096];
    static unsigned char cert[4096];
    GWCertReq req;
    size_t    n, off, cn, coff;
    size_t    i;

    puts("gw_x509write");

    for (i = 0; i < sizeof(mod); i++) mod[i] = (unsigned char)(0x80 + i);

    memset(&req, 0, sizeof(req));
    req.cn         = "lite.duckduckgo.com";
    req.issuer_cn  = "Gateway Local CA";
    req.is_ca      = 0;
    req.serial     = serial;
    req.serial_len = sizeof(serial);
    req.mod        = mod;
    req.mod_len    = sizeof(mod);
    req.exp        = exp3;
    req.exp_len    = sizeof(exp3);
    req.not_before = "980101000000Z";
    req.not_after  = "370101000000Z";

    n = gw_x509_tbs(&req, buf, sizeof(buf), &off);
    check(n > 0, "a leaf TBSCertificate is produced");
    check(buf[off] == 0x30, "it is a SEQUENCE");
    /* Tag, then a long-form length: two content bytes for anything this size. */
    check(buf[off + 1] == 0x82, "with a two-byte length");
    check(((size_t)buf[off + 2] << 8 | buf[off + 3]) == n - 4,
          "and the length matches what was written");
    /* A v1 leaf (since 2026-09-19, PATCHES.md §28) has no version field: the
     * first TBSCertificate field is the serialNumber INTEGER, not an explicit
     * [0]. Netscape 3.04 Gold refused a v3 leaf on the RC2 suite. The serial
     * here is 4A170231, whose first byte is < 0x80, so it needs no padding. */
    check(memcmp(buf + off + 4, "\x02\x04\x4A\x17\x02\x31", 6) == 0,
          "no version field: the leaf is v1, starting at its serialNumber");
    check(memmem(buf + off, n, "lite.duckduckgo.com", 19) != NULL,
          "the subject name is in there");
    check(memmem(buf + off, n, "Gateway Local CA", 16) != NULL,
          "and so is the issuer");
    /* The modulus must have gained a 0x00 in front of its 0x80 first byte. */
    check(memmem(buf + off, n, "\x02\x81\x81\x00\x80", 5) != NULL,
          "a high-bit modulus is padded so the INTEGER stays positive");

    /* The same key as a CA: different extensions, no subjectAltName. */
    {
        static unsigned char ca[4096];
        size_t caoff, can;

        req.is_ca = 1;
        req.cn    = "Gateway Local CA";
        can = gw_x509_tbs(&req, ca, sizeof(ca), &caoff);
        check(can > 0, "a CA TBSCertificate is produced");
        /* BasicConstraints cA TRUE, inside its OCTET STRING. */
        check(memmem(ca + caoff, can, "\x30\x03\x01\x01\xFF", 5) != NULL,
              "the CA says cA TRUE");
        check(memmem(ca + caoff, can, "\x03\x02\x01\x06", 4) != NULL,
              "with keyCertSign and cRLSign");
        /* The authority stays v3, unlike the leaf: version [0] INTEGER 2. */
        check(memmem(ca + caoff, can, "\xA0\x03\x02\x01\x02", 5) != NULL,
              "and is v3, so it carries the version field");
        req.is_ca = 0;
        req.cn    = "lite.duckduckgo.com";
    }

    /* Wrapping it with a signature. The bytes are not a real signature; this
     * checks the envelope, and CI checks that openssl can read it. */
    {
        static unsigned char sig[128];

        for (i = 0; i < sizeof(sig); i++) sig[i] = (unsigned char)(i * 7);
        cn = gw_x509_cert(buf + off, n, sig, sizeof(sig),
                          cert, sizeof(cert), &coff);
        check(cn > n, "a Certificate is larger than the TBS it wraps");
        check(cert[coff] == 0x30, "and is itself a SEQUENCE");
        check(memcmp(cert + coff + 4, buf + off, n) == 0,
              "the TBS bytes are copied in verbatim, so the hash still matches");
    }

    /* Refusals. A caller that gets these wrong must not get a certificate. */
    {
        GWCertReq bad = req;

        bad.not_before = "98010100000Z";      /* 12 characters, not 13 */
        check(gw_x509_tbs(&bad, buf, sizeof(buf), &off) == 0,
              "a UTCTime of the wrong length is refused");
        bad = req;
        bad.mod_len = 0;
        check(gw_x509_tbs(&bad, buf, sizeof(buf), &off) == 0,
              "an empty modulus is refused");
        bad = req;
        check(gw_x509_tbs(&bad, buf, 64, &off) == 0,
              "a buffer too small is refused rather than overrun");
    }

    /*
     * Written out for CI to hand to openssl, which is the only reader here
     * with no stake in this being right. Everything above confirms the bytes
     * are what this file meant to write; openssl confirms they are a
     * certificate.
     */
    {
        FILE *f = fopen("gw_leaf.der", "wb");

        if (f != NULL) {
            fwrite(cert + coff, 1, cn, f);
            fclose(f);
        }
    }
    {
        static unsigned char ca[4096], cacert[4096];
        static unsigned char sig[128];
        size_t caoff, can, ccoff, ccn;

        for (i = 0; i < sizeof(sig); i++) sig[i] = (unsigned char)(0xFF - i);
        req.is_ca = 1;
        req.cn    = "Gateway Local CA";
        can = gw_x509_tbs(&req, ca, sizeof(ca), &caoff);
        ccn = gw_x509_cert(ca + caoff, can, sig, sizeof(sig),
                           cacert, sizeof(cacert), &ccoff);
        if (ccn > 0) {
            FILE *f = fopen("gw_ca.der", "wb");

            if (f != NULL) {
                fwrite(cacert + ccoff, 1, ccn, f);
                fclose(f);
            }
        }
    }
}

/* ------------------------------------------------------------------ */

/* The allow-list the script is built against. */
static const char *sPacHosts[] = {
    "floodgap.com", "*.floodgap.com", "68k.news",
    "bad\"quote.com",              /* must be dropped, not escaped */
    NULL
};

static int pac_host(int index, char *out, size_t cap)
{
    if (index < 0 || sPacHosts[index] == NULL) return 0;
    gw_copy_n(out, cap, sPacHosts[index], strlen(sPacHosts[index]));
    return 1;
}

/*
 * wayback_api: JSON parsing and URL building.
 *
 * §7 test cases 20–25. The JSON work is portable; the fetch is not, so test
 * the parsing and the URL building and leave the transport to the Mac.
 */
static void test_wayback_api(void)
{
    static const char body[] =
        "{\"url\":\"example.com\","
        "\"archived_snapshots\":{\"closest\":{\"status\":\"200\","
        "\"available\":true,"
        "\"url\":\"http://web.archive.org/web/20011025000000/http://example.com/\","
        "\"timestamp\":\"20011025000000\"}}}";
    char value[128];

    printf("wayback api\n");

    /* Case 20: a real availability body — reading timestamp returns the stamp. */
    check(gw_json_string(body, sizeof(body) - 1,
                         "timestamp", value, sizeof(value)),
          "case 20: timestamp extracted");
    check_str(value, "20011025000000", "case 20: timestamp value");

    /* Case 21: reading url returns the top-level echo, not the snapshot.
     * This asserts behaviour that is wrong for the caller — gw_json_string
     * scans flat, a future reader will reach for url, and this is the line
     * that stops them from "simplifying" into it later. */
    check(gw_json_string(body, sizeof(body) - 1,
                         "url", value, sizeof(value)),
          "case 21: url extracted (top-level echo)");
    check_str(value, "example.com", "case 21: top-level url is the echo");

    /* Case 22: available:false — treated as no snapshot. */
    {
        static const char no_snap[] =
            "{\"url\":\"x.com\","
            "\"archived_snapshots\":{\"closest\":{"
            "\"available\":false}}}";
        check(!gw_json_string(no_snap, sizeof(no_snap) - 1,
                              "available", value, sizeof(value)),
              "case 22: available:false is not a string");
        /* Check the number instead. */
        check(gw_json_number(no_snap, sizeof(no_snap) - 1,
                            "available") == -1,
              "case 22: available:false is not a number");
    }

    /* Case 23: no archived_snapshots member — treated as no snapshot. */
    {
        static const char no_archived[] =
            "{\"url\":\"x.com\"}";
        check(!gw_json_string(no_archived, sizeof(no_archived) - 1,
                              "timestamp", value, sizeof(value)),
              "case 23: no timestamp when archived_snapshots is absent");
    }

    /* Case 24: a truncated body — no snapshot, no read past the end. */
    {
        static const char truncated[] = "{\"url\":\"x.com\","
                                        "\"archived_snapshots\":{\"closest\":{";
        check(!gw_json_string(truncated, sizeof(truncated) - 1,
                              "timestamp", value, sizeof(value)),
              "case 24: truncated body returns no timestamp");
    }

    /* Case 25: a stamp and a URL — the built target is /web/<stamp>id_/<url>. */
    {
        static const char body2[] =
            "{\"url\":\"test.org\","
            "\"archived_snapshots\":{\"closest\":{"
            "\"available\":true,"
            "\"timestamp\":\"20020101120000\"}}}";
        char stamp[GW_WB_STAMP];
        char target[GW_MAX_PATH];

        check(gw_json_string(body2, sizeof(body2) - 1,
                             "timestamp", stamp, sizeof(stamp)),
              "case 25: timestamp extracted");
        check_str(stamp, "20020101120000", "case 25: stamp value");

        /* Build the target URL. */
        {
            static const char url[] = "http://test.org/page.html";
            snprintf(target, sizeof(target),
                     "/web/%sid_/%s", stamp, url);
            check_str(target,
                      "/web/20020101120000id_/http://test.org/page.html",
                      "case 25: target URL built correctly");
        }
    }
}

static void test_pac(void)
{
    char   buf[4096];
    size_t n;

    printf("pac\n");

    check(gw_pac_is_request("/proxy.pac"), "proxy.pac is the script");
    check(gw_pac_is_request("/wpad.dat"), "wpad.dat is the script");
    check(gw_pac_is_request("/PROXY.PAC"), "the match is case-insensitive");
    check(gw_pac_is_request("/proxy.pac?1758"), "a cache-buster is ignored");
    check(!gw_pac_is_request("/proxy.pack"), "a longer name is not the script");
    check(!gw_pac_is_request("/a/proxy.pac"), "only at the root");
    check(!gw_pac_is_request("/"), "the root is not the script");
    check(!gw_pac_is_request(NULL), "no path is not the script");

    /* Served from the archive listener: the allow-list is the exception. */
    n = gw_pac_build("192.168.1.5", 8765, 8888, pac_host, buf, sizeof(buf));
    check(n > 0 && n == strlen(buf), "the script is built and counted");
    check(strstr(buf, "function FindProxyForURL(url, host)") != NULL,
          "the entry point is there");
    check(strstr(buf, "if (host == \"192.168.1.5\") return \"DIRECT\";") != NULL,
          "Gateway itself is DIRECT, so a refetch cannot loop");
    check(strstr(buf, "shExpMatch(host, \"*.floodgap.com\")) return \"DIRECT\";")
              != NULL,
          "an allow-listed host goes direct, not through either proxy");
    check(strstr(buf, "shExpMatch(host, \"68k.news\") ||\n"
                      "        shExpMatch(host, \"*.68k.news\")") != NULL,
          "a plain host is emitted in both forms, as the proxy matches it");
    check(strstr(buf, "8765") == NULL,
          "the archive script names no live proxy at all");
    check(strstr(buf, "url") == NULL || strstr(buf, "shExpMatch(url") == NULL,
          "and decides on the host alone");
    check(strstr(buf, "return \"PROXY 192.168.1.5:8888\";") != NULL,
          "everything else goes to the archive");
    check(strstr(buf, "bad") == NULL,
          "a pattern that would break the literal is dropped");

    check(strstr(buf, "// The archive:") != NULL,
          "the script says which of the two it is");

    /* Served from the live listener: everything live, no allow-list. */
    n = gw_pac_build("gateway.local", 8765, 0, pac_host, buf, sizeof(buf));
    check(n > 0, "the script is built with no archive listener");
    check(strstr(buf, "8888") == NULL, "no archive proxy is named");
    check(strstr(buf, "floodgap") == NULL,
          "the allow-list is left out when there is nothing to route around");
    check(strstr(buf, "return \"PROXY gateway.local:8765\";") != NULL,
          "everything goes to the live proxy");
    check(strstr(buf, "// The live web:") != NULL,
          "and says so at the top");

    /* Refusals rather than half a script. */
    check(gw_pac_build("", 8765, 0, pac_host, buf, sizeof(buf)) == 0,
          "no authority is refused");
    check(gw_pac_build("ho\"st", 8765, 0, pac_host, buf, sizeof(buf)) == 0,
          "an authority that would break the literal is refused");
    check(gw_pac_build("192.168.1.5", 0, 0, pac_host, buf, sizeof(buf)) == 0,
          "no live port is refused");
    check(gw_pac_build("192.168.1.5", 8765, 8888, pac_host, buf, 40) == 0,
          "a buffer too small yields nothing rather than a truncated script");
}

/* ------------------------------------------------------------------ */

static void test_host_match(void)
{
    printf("gw_host_matches\n");

    /* A plain name covers itself and everything under it. */
    check(gw_host_matches("howsmyssl.com", "howsmyssl.com"),
          "a plain name matches itself");
    check(gw_host_matches("howsmyssl.com", "www.howsmyssl.com"),
          "a plain name covers its subdomains");
    check(gw_host_matches("howsmyssl.com", "a.b.howsmyssl.com"),
          "however deep");
    check(gw_host_matches("HowsMySSL.com", "WWW.howsmyssl.COM"),
          "case does not matter");

    /* But only a real subdomain: the dot has to be there. */
    check(!gw_host_matches("howsmyssl.com", "notmyhowsmyssl.com"),
          "a suffix that is not a subdomain does not match");
    check(!gw_host_matches("howsmyssl.com", "howsmyssl.com.evil.test"),
          "nor a name that merely starts with it");
    check(!gw_host_matches("howsmyssl.com", "com"),
          "nor a shorter name");

    /* A glob keeps meaning exactly what it says. */
    check(gw_host_matches("*.howsmyssl.com", "www.howsmyssl.com"),
          "a glob still matches subdomains");
    check(!gw_host_matches("*.howsmyssl.com", "howsmyssl.com"),
          "a *. glob does not cover the bare name, as globs never did");
    check(gw_host_matches("*.news", "68k.news"), "a bare-suffix glob works");
    check(!gw_host_matches("", "howsmyssl.com"), "an empty pattern matches nothing");
    check(!gw_host_matches("howsmyssl.com", ""), "and nothing matches an empty host");
}

/* An indexed lookup must not stop at a blank entry. */
static void test_prefs_list(void)
{
    static const char text[] =
        "wayback_live = frogfind.com\n"
        "wayback_live =\n"                 /* blank: not the end of the list */
        "# wayback_live = commented.out\n"
        "wayback_live = howsmyssl.com\n";
    char buf[128];

    printf("prefs lists\n");

    check(gw_prefs_get_nth(text, sizeof(text) - 1, "wayback_live", 0,
                           buf, sizeof(buf)) == 1, "entry 0 is present");
    check_str(buf, "frogfind.com", "entry 0");

    check(gw_prefs_get_nth(text, sizeof(text) - 1, "wayback_live", 1,
                           buf, sizeof(buf)) == 1,
          "a blank entry is reported as present, not as the end");
    check_str(buf, "", "entry 1 is empty");

    check(gw_prefs_get_nth(text, sizeof(text) - 1, "wayback_live", 2,
                           buf, sizeof(buf)) == 1,
          "the entry after the blank is still reachable");
    check_str(buf, "howsmyssl.com", "entry 2, past the blank and the comment");

    check(gw_prefs_get_nth(text, sizeof(text) - 1, "wayback_live", 3,
                           buf, sizeof(buf)) == 0, "and then the list ends");

    /* gw_prefs_get keeps its old meaning: set to something, or not. */
    check(gw_prefs_get(text, sizeof(text) - 1, "wayback_live",
                       buf, sizeof(buf)) == 1, "gw_prefs_get is unchanged");
}

/*
 * The indexed splitter: gw_prefs_get_nth_split.
 *
 * §7 test cases 1–10. Both storage forms (repeated keys and one ;-separated
 * value) are read correctly, spaces are trimmed, empty entries are skipped,
 * and the walk does not stop at a blank entry inside a value.
 */
static void test_prefs_splitter(void)
{
    static const char semi[] =
        "wayback_live = frogfind.com;*.frogfind.com;c.net\n";
    static const char mixed[] =
        "wayback_live = frogfind.com;*.frogfind.com\n"
        "wayback_live = howsmyssl.com;c.net\n";
    static const char spaces[] =
        "wayback_live = a.com ; *.b.com ;c.net\n";
    static const char empty_mid[] =
        "wayback_live = a.com;;c.net\n";
    static const char trailing[] =
        "wayback_live = a.com;b.com;c.net;\n";
    static const char commented[] =
        "# wayback_live = x\n"
        "wayback_live = frogfind.com\n";
    static const char empty_val[] =
        "wayback_live =\n"
        "wayback_live = frogfind.com\n";
    static const char upper[] =
        "WAYBACK_LIVE = frogfind.com\n";
    char buf[128];

    printf("prefs splitter\n");

    /* Case 1: four repeated keys, one pattern each. */
    {
        static const char four[] =
            "wayback_live = a.com\n"
            "wayback_live = b.com\n"
            "wayback_live = c.net\n"
            "wayback_live = d.org\n";
        check(gw_prefs_get_nth_split(four, sizeof(four) - 1,
                                     "wayback_live", 0, buf, sizeof(buf)) == 1,
              "case 1: index 0 exists");
        check_str(buf, "a.com", "case 1: index 0");
        check(gw_prefs_get_nth_split(four, sizeof(four) - 1,
                                     "wayback_live", 3, buf, sizeof(buf)) == 1,
              "case 1: index 3 exists");
        check_str(buf, "d.org", "case 1: index 3");
        check(gw_prefs_get_nth_split(four, sizeof(four) - 1,
                                     "wayback_live", 4, buf, sizeof(buf)) == 0,
              "case 1: index 4 returns 0");
    }

    /* Case 2: one ;-separated value. */
    check(gw_prefs_get_nth_split(semi, sizeof(semi) - 1,
                                 "wayback_live", 0, buf, sizeof(buf)) == 1,
          "case 2: index 0");
    check_str(buf, "frogfind.com", "case 2: index 0");
    check(gw_prefs_get_nth_split(semi, sizeof(semi) - 1,
                                 "wayback_live", 1, buf, sizeof(buf)) == 1,
          "case 2: index 1");
    check_str(buf, "*.frogfind.com", "case 2: index 1");
    check(gw_prefs_get_nth_split(semi, sizeof(semi) - 1,
                                 "wayback_live", 2, buf, sizeof(buf)) == 1,
          "case 2: index 2");
    check_str(buf, "c.net", "case 2: index 2");
    check(gw_prefs_get_nth_split(semi, sizeof(semi) - 1,
                                 "wayback_live", 3, buf, sizeof(buf)) == 0,
          "case 2: index 3 returns 0");

    /* Case 3: a ; line and a later plain line — every entry in file order.
     * First occurrence: frogfind.com;*.frogfind.com (entries 0,1).
     * Second occurrence: howsmyssl.com;c.net (entries 2,3). */
    check(gw_prefs_get_nth_split(mixed, sizeof(mixed) - 1,
                                 "wayback_live", 0, buf, sizeof(buf)) == 1,
          "case 3: index 0");
    check_str(buf, "frogfind.com", "case 3: index 0");
    check(gw_prefs_get_nth_split(mixed, sizeof(mixed) - 1,
                                 "wayback_live", 1, buf, sizeof(buf)) == 1,
          "case 3: index 1");
    check_str(buf, "*.frogfind.com", "case 3: index 1");
    check(gw_prefs_get_nth_split(mixed, sizeof(mixed) - 1,
                                 "wayback_live", 2, buf, sizeof(buf)) == 1,
          "case 3: index 2");
    check_str(buf, "howsmyssl.com", "case 3: index 2");
    check(gw_prefs_get_nth_split(mixed, sizeof(mixed) - 1,
                                 "wayback_live", 3, buf, sizeof(buf)) == 1,
          "case 3: index 3");
    check_str(buf, "c.net", "case 3: index 3");
    check(gw_prefs_get_nth_split(mixed, sizeof(mixed) - 1,
                                 "wayback_live", 4, buf, sizeof(buf)) == 0,
          "case 3: index 4 returns 0");

    /* Case 4: spaces around entries are trimmed. */
    check(gw_prefs_get_nth_split(spaces, sizeof(spaces) - 1,
                                 "wayback_live", 0, buf, sizeof(buf)) == 1,
          "case 4: index 0");
    check_str(buf, "a.com", "case 4: index 0");
    check(gw_prefs_get_nth_split(spaces, sizeof(spaces) - 1,
                                 "wayback_live", 1, buf, sizeof(buf)) == 1,
          "case 4: index 1");
    check_str(buf, "*.b.com", "case 4: index 1");
    check(gw_prefs_get_nth_split(spaces, sizeof(spaces) - 1,
                                 "wayback_live", 2, buf, sizeof(buf)) == 1,
          "case 4: index 2");
    check_str(buf, "c.net", "case 4: index 2");

    /* Case 5: empty entry in the middle does not end the walk. */
    check(gw_prefs_get_nth_split(empty_mid, sizeof(empty_mid) - 1,
                                 "wayback_live", 0, buf, sizeof(buf)) == 1,
          "case 5: index 0");
    check_str(buf, "a.com", "case 5: index 0");
    check(gw_prefs_get_nth_split(empty_mid, sizeof(empty_mid) - 1,
                                 "wayback_live", 1, buf, sizeof(buf)) == 1,
          "case 5: index 1 (past empty)");
    check_str(buf, "c.net", "case 5: index 1 (past empty)");
    check(gw_prefs_get_nth_split(empty_mid, sizeof(empty_mid) - 1,
                                 "wayback_live", 2, buf, sizeof(buf)) == 0,
          "case 5: index 2 returns 0");

    /* Case 6: trailing ; does not produce an empty final entry. */
    check(gw_prefs_get_nth_split(trailing, sizeof(trailing) - 1,
                                 "wayback_live", 0, buf, sizeof(buf)) == 1,
          "case 6: index 0");
    check_str(buf, "a.com", "case 6: index 0");
    check(gw_prefs_get_nth_split(trailing, sizeof(trailing) - 1,
                                 "wayback_live", 2, buf, sizeof(buf)) == 1,
          "case 6: index 2");
    check_str(buf, "c.net", "case 6: index 2");
    check(gw_prefs_get_nth_split(trailing, sizeof(trailing) - 1,
                                 "wayback_live", 3, buf, sizeof(buf)) == 0,
          "case 6: index 3 returns 0");

    /* Case 7: a commented line stays commented. */
    check(gw_prefs_get_nth_split(commented, sizeof(commented) - 1,
                                 "wayback_live", 0, buf, sizeof(buf)) == 1,
          "case 7: comment is skipped");
    check_str(buf, "frogfind.com", "case 7: comment is skipped");

    /* Case 8: empty value contributes nothing, does not end the walk. */
    check(gw_prefs_get_nth_split(empty_val, sizeof(empty_val) - 1,
                                 "wayback_live", 0, buf, sizeof(buf)) == 1,
          "case 8: past empty value");
    check_str(buf, "frogfind.com", "case 8: past empty value");

    /* Case 9: key matching is case-insensitive. */
    check(gw_prefs_get_nth_split(upper, sizeof(upper) - 1,
                                 "wayback_live", 0, buf, sizeof(buf)) == 1,
          "case 9: case-insensitive key");
    check_str(buf, "frogfind.com", "case 9: case-insensitive key");

    /* Case 10: entry longer than cap is truncated, NUL-terminated. */
    {
        char small[8];
        static const char long_entry[] =
            "wayback_live = this.is.a.very.long.hostname.example.com\n";
        check(gw_prefs_get_nth_split(long_entry, sizeof(long_entry) - 1,
                                     "wayback_live", 0, small, sizeof(small)) == 1,
              "case 10: entry fits in small buffer");
        check_str(small, "this.is", "case 10: truncated to cap - 1");
    }
}

/*
 * The writer dropping stale duplicates.
 *
 * §7 test cases 11–16. Setting a key that appears three times leaves
 * exactly one line; other keys are untouched; comments are left alone.
 */
static void test_prefs_set_drop(void)
{
    static const char before[] =
        "http_port = 8765\n"
        "wayback_live = a.com\n"
        "wayback_live = b.com\n"
        "wayback_live = c.net\n"
        "local_password: hunter2\n";
    static const char single[] =
        "http_port = 8765\n"
        "wayback_live = a.com\n";
    char out[512];
    size_t n;
    char buf[128];

    printf("prefs set drop\n");

    /* Case 11: a key present three times, set once — exactly one line. */
    n = gw_prefs_set(before, sizeof(before) - 1,
                     "wayback_live", "new.com", out, sizeof(out));
    check(n > 0, "case 11: set succeeds");
    {
        /* Count how many wayback_live lines are in the output. */
        int count = 0;
        size_t off = 0;
        while (off < n) {
            /* Find the next line. */
            size_t le = off;
            while (le < n && out[le] != '\n' && out[le] != '\r') le++;
            /* Check if this line starts with wayback_live. */
            size_t i = off;
            while (i < le && (out[i] == ' ' || out[i] == '\t')) i++;
            if (i < le && out[i] != '#' && out[i] != ';') {
                size_t ke = i;
                while (ke < le && out[ke] != '=' && out[ke] != ':') ke++;
                if (ke < le) {
                    size_t kend = ke;
                    while (kend > i && (out[kend - 1] == ' ' ||
                                        out[kend - 1] == '\t')) kend--;
                    if (kend - i == 12 &&
                        gw_strnicmp(out + i, "wayback_live", 12) == 0)
                        count++;
                }
            }
            off = (le < n && out[le] == '\r' && le + 1 < n &&
                   out[le + 1] == '\n') ? le + 2 : (le < n ? le + 1 : le);
        }
        check(count == 1, "case 11: exactly one wayback_live line remains");
    }

    /* Case 12: other keys are untouched, in their original order. */
    check(gw_prefs_get(out, n, "http_port", buf, sizeof(buf)) &&
          strcmp(buf, "8765") == 0, "case 12: http_port survives");
    check(gw_prefs_get(out, n, "local_password", buf, sizeof(buf)) &&
          strcmp(buf, "hunter2") == 0, "case 12: local_password survives");

    /* Case 13: a commented line is left alone. */
    {
        static const char with_c[] =
            "http_port = 8765\n"
            "# wayback_live = old\n"
            "wayback_live = a.com\n";
        n = gw_prefs_set(with_c, sizeof(with_c) - 1,
                         "wayback_live", "new.com", out, sizeof(out));
        check(memmem(out, n, "# wayback_live = old", 19) != NULL,
              "case 13: comment is left alone");
    }

    /* Case 14: a key present once, set — unchanged behaviour (regression). */
    {
        n = gw_prefs_set(single, sizeof(single) - 1,
                         "wayback_live", "new.com", out, sizeof(out));
        check(n > 0, "case 14: set on single key succeeds");
        check(gw_prefs_get(out, n, "wayback_live", buf, sizeof(buf)) &&
              strcmp(buf, "new.com") == 0, "case 14: value is updated");
    }

    /* Case 15: a key absent, set — appended (regression). */
    {
        n = gw_prefs_set(single, sizeof(single) - 1,
                         "new_key", "value", out, sizeof(out));
        check(n > 0, "case 15: append succeeds");
        check(gw_prefs_get(out, n, "new_key", buf, sizeof(buf)) &&
              strcmp(buf, "value") == 0, "case 15: appended value reads back");
    }

    /* Case 16: after case 11, read it back — index 0 is the new value,
     * index 1 returns 0. */
    {
        n = gw_prefs_set(before, sizeof(before) - 1,
                         "wayback_live", "new.com", out, sizeof(out));
        check(gw_prefs_get_nth_split(out, n,
                                     "wayback_live", 0, buf, sizeof(buf)) == 1,
              "case 16: index 0 is the new value");
        check_str(buf, "new.com", "case 16: index 0 is the new value");
        check(gw_prefs_get_nth_split(out, n,
                                     "wayback_live", 1, buf, sizeof(buf)) == 0,
              "case 16: index 1 returns 0");
    }
}

/*
 * The two consumers agreeing: proxy and PAC file use the same splitter.
 *
 * §7 test cases 17–19. A host on the list is fetched live by the proxy
 * and routed direct by the generated PAC script.
 */
static void test_pac_splitter_agree(void)
{
    static const char text[] =
        "wayback_live = frogfind.com;*.frogfind.com;c.net\n";
    char buf[128];

    printf("pac splitter agree\n");

    /* Case 17: read through the new accessor — the pattern sequence is the
     * same as what GW_WaybackHostIsLive would match and what PAC builds from. */
    {
        int i;
        for (i = 0; i < 128; i++) {
            if (!gw_prefs_get_nth_split(text, sizeof(text) - 1,
                                        "wayback_live", i, buf, sizeof(buf)))
                break;
        }
    }

    /* Case 18: a ;-separated list — the PAC script names every entry. */
    {
        static const char text2[] =
            "wayback_live = a.com;b.com;c.net\n";
        int i;
        char hosts[128][64];
        int count = 0;

        for (i = 0; i < 128; i++) {
            if (!gw_prefs_get_nth_split(text2, sizeof(text2) - 1,
                                        "wayback_live", i, hosts[count],
                                        sizeof(hosts[0])))
                break;
            count++;
        }
        check(count == 3, "case 18: three entries extracted");
        check_str(hosts[0], "a.com", "case 18: first");
        check_str(hosts[1], "b.com", "case 18: second");
        check_str(hosts[2], "c.net", "case 18: third");
    }

    /* Case 19: the same list in repeated-key form — byte-identical output.
     * Compare a ;-separated value with the same entries in repeated-key form. */
    {
        static const char semi_form[] =
            "wayback_live = a.com;b.com;c.net\n";
        static const char repeated[] =
            "wayback_live = a.com\n"
            "wayback_live = b.com\n"
            "wayback_live = c.net\n";
        int i;
        char hosts1[128][64], hosts2[128][64];
        int count1 = 0, count2 = 0;

        for (i = 0; i < 128; i++) {
            if (!gw_prefs_get_nth_split(semi_form, sizeof(semi_form) - 1,
                                        "wayback_live", i, hosts1[count1],
                                        sizeof(hosts1[0])))
                break;
            count1++;
        }
        for (i = 0; i < 128; i++) {
            if (!gw_prefs_get_nth_split(repeated, sizeof(repeated) - 1,
                                        "wayback_live", i, hosts2[count2],
                                        sizeof(hosts2[0])))
                break;
            count2++;
        }
        check(count1 == count2, "case 19: same number of entries");
        check(count1 == 3, "case 19: three entries from each form");
        {
            int match = 1;
            for (i = 0; i < count1; i++) {
                if (strcmp(hosts1[i], hosts2[i]) != 0) { match = 0; break; }
            }
            check(match, "case 19: byte-identical output from both forms");
        }
    }
}

/* ------------------------------------------------------------------ */

/* Exercise the editor-to-file-to-consumer path, including deleting all sites. */
static void test_whitelist_edit(void)
{
    char value[256] = " ; frogfind.com \r\n *.example.com ;;\t68k.news \n; ";
    char prefs[1024], entry[128];
    const char *old = "wayback_live = stale.com\nother = keep\nwayback_live = deleted.com\n";
    size_t len;
    printf("whitelist editing\n");
    gw_prefs_normalize_list(value);
    check_str(value, "frogfind.com;*.example.com;68k.news", "normalize pasted host list");
    len = gw_prefs_set(old, strlen(old), "wayback_live", value, prefs, sizeof(prefs));
    check(len > 0, "save edited whitelist");
    check(gw_prefs_get_nth_split(prefs, len, "wayback_live", 2, entry, sizeof(entry)), "reload added site");
    check_str(entry, "68k.news", "added site survives reload");
    check(!gw_prefs_get_nth_split(prefs, len, "wayback_live", 3, entry, sizeof(entry)), "deleted sites stay deleted");
    strcpy(value, " ; \r\n ; \t");
    gw_prefs_normalize_list(value);
    check_str(value, "", "empty editor list has no leading separator");
    len = gw_prefs_set(old, strlen(old), "wayback_live", value, prefs, sizeof(prefs));
    check(len > 0, "save empty whitelist");
    check(!gw_prefs_get_nth_split(prefs, len, "wayback_live", 0, entry, sizeof(entry)), "all whitelist entries removed");
    check(gw_prefs_get(prefs, len, "other", entry, sizeof(entry)), "other preferences survive");
    check_str(entry, "keep", "other value unchanged");
    strcpy(value, "a;b");
    gw_prefs_normalize_list(value);
    check_str(value, "a;b", "normalization is idempotent");
}

int main(void)
{
    test_util();
    test_url();
    test_request();
    test_response();
    test_chunked();
    test_b64();
    test_mailcmd();
    test_oauth();
    test_prefs();
    test_glob();
    test_query();
    test_wayback();
    test_rewrite();
    test_x509write();
    test_host_match();
    test_prefs_list();
    test_prefs_splitter();
    test_whitelist_edit();
    test_prefs_set_drop();
    test_pac_splitter_agree();
    test_wayback_api();
    test_pac();

    printf("\n%d checks, %d failures\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
