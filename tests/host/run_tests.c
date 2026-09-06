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
#include "gw_prefs.h"
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
        check(gw_http_should_follow(kGWRedirectAuto, 1, 1) == 0,
              "auto passes https to https back to the client");

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

    printf("\n%d checks, %d failures\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
