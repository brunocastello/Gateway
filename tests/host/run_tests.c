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

        n = gw_http_build_upstream(&req, r, req.head_len, out, sizeof(out));
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

        n = gw_http_build_upstream(&req, r, req.head_len, out, sizeof(out));
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
    char out[1024];
    size_t n;

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

        n = gw_http_filter_response(r, res.head_len, out, sizeof(out));
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

    printf("\n%d checks, %d failures\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
