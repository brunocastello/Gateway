/*
 * gw_pac.c - see gw_pac.h.
 *
 * The script is written for the oldest engine that will read it, which is
 * Netscape 3's. That rules out more than it sounds like: no `var` outside a
 * function, no arrays, no `for`, no `===`, no string methods beyond what
 * FindProxyForURL is handed. What is left -- `if`, `return`, `||`, and the
 * helper functions every implementation of the format provides -- is enough,
 * and a flat chain of tests is also the easiest thing to read when someone
 * opens the URL to see what their browser was told.
 */

#include "gw_pac.h"

#include <stdio.h>
#include <string.h>

#include "gw_url.h"    /* GW_MAX_HOST */
#include "gw_util.h"

/* Appends, returning 0 if it would not fit. */
static int add(char *out, size_t cap, size_t *used, const char *s)
{
    size_t n = strlen(s);

    if (*used + n + 1 > cap) return 0;
    memcpy(out + *used, s, n);
    *used += n;
    out[*used] = '\0';
    return 1;
}

/*
 * Whether a pattern can go into a JavaScript string literal as it stands.
 *
 * Everything a host glob needs is here and nothing else is, so there is no
 * escaping to get wrong: a quote or a backslash in a pattern would end the
 * literal and break the whole script, turning one bad line in the prefs into
 * a browser with no proxy configuration at all. A pattern that fails this is
 * dropped, which costs one host; letting it through costs every host.
 */
static int pattern_is_safe(const char *p)
{
    size_t i;

    if (p == NULL || p[0] == '\0') return 0;
    for (i = 0; p[i] != '\0'; i++) {
        char c = p[i];

        if (c >= 'a' && c <= 'z') continue;
        if (c >= 'A' && c <= 'Z') continue;
        if (c >= '0' && c <= '9') continue;
        if (c == '.' || c == '-' || c == '_' || c == '*' || c == '?') continue;
        return 0;
    }
    return 1;
}

int gw_pac_is_request(const char *path)
{
    size_t n;

    if (path == NULL) return 0;
    if (*path != '/') return 0;

    /* Up to the query string; a browser may append one to defeat a cache. */
    for (n = 0; path[n] != '\0' && path[n] != '?' && path[n] != '#'; n++)
        ;

    if (n == 10 && gw_strnicmp(path, "/proxy.pac", 10) == 0) return 1;
    if (n ==  9 && gw_strnicmp(path, "/wpad.dat",  9) == 0) return 1;
    return 0;
}

size_t gw_pac_build(const char *authority, int live_port, int archive_port,
                    GWPacNextHost next_host, char *out, size_t cap)
{
    char   line[640];
    char   pattern[256];
    char   live[GW_MAX_HOST + 32];
    size_t used = 0;
    int    i;

    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';
    if (!pattern_is_safe(authority)) return 0;
    if (live_port <= 0) return 0;

    snprintf(live, sizeof(live), "PROXY %s:%d", authority, live_port);

    /*
     * Say which of the two this is. Someone with a bookmark for each needs to
     * be able to open one and tell, and the difference is otherwise a port
     * number buried forty lines down.
     */
    if (!add(out, cap, &used,
            "// Gateway proxy auto-configuration.\n"
            "//\n"))
        return 0;
    if (!add(out, cap, &used, archive_port > 0
            ? "// The archive: pages as they were, except the hosts listed\n"
              "// below, which are fetched from the live web.\n"
            : "// The live web: every host, as it is now.\n"))
        return 0;
    if (!add(out, cap, &used,
            "//\n"
            "// Generated for the address this was fetched from, so the\n"
            "// proxies named below are ones this browser can reach.\n"
            "\n"
            "function FindProxyForURL(url, host)\n"
            "{\n"))
        return 0;

    /*
     * Gateway itself, and anything that is not a dotted name, go direct.
     *
     * The first is not optional. Without it a browser asked to re-fetch this
     * script -- which they do, on a timer -- would fetch it through the proxy
     * it describes, and Gateway would be proxying to itself. The second is
     * the ordinary courtesy of not sending `http://fileserver/` out to the
     * internet.
     */
    snprintf(line, sizeof(line),
             "    // Gateway itself: never through Gateway.\n"
             "    if (host == \"%s\") return \"DIRECT\";\n"
             "    if (isPlainHostName(host)) return \"DIRECT\";\n"
             "    if (shExpMatch(host, \"127.*\")) return \"DIRECT\";\n"
             "    if (shExpMatch(host, \"localhost\")) return \"DIRECT\";\n"
             "\n", authority);
    if (!add(out, cap, &used, line)) return 0;

    /*
     * With no archive listener there is nothing to route around, so the
     * allow-list is left out rather than written as a list of hosts that all
     * resolve to the same answer as everything else.
     */
    if (archive_port > 0 && next_host != NULL) {
        int any = 0;

        for (i = 0; i < 128; i++) {
            if (!next_host(i, pattern, sizeof(pattern))) break;
            if (!pattern_is_safe(pattern)) continue;
            if (!any) {
                /*
                 * An allow-listed host is one the archive should not answer
                 * for. What it needs from Gateway after that depends on the
                 * scheme, and the script is handed the whole URL, so it can
                 * tell: plain http is something the browser can already do,
                 * and sending it through a proxy to be handed back unchanged
                 * buys nothing. https is the opposite -- left to itself a
                 * browser this old attempts a 2026 handshake and fails,
                 * which is the entire reason this program exists -- so that
                 * one still goes to the live listener.
                 *
                 * Decided once, above the list, so each entry stays one line
                 * and the two answers cannot drift apart.
                 */
                if (!add(out, cap, &used,
                        "    // The live web, by wayback_live in the prefs:\n"
                        "    // direct when the browser needs nothing from\n"
                        "    // Gateway, through it when the TLS does.\n"
                        "    var live;\n"
                        "    if (shExpMatch(url, \"http://*\")) live = \"DIRECT\";\n"))
                    return 0;
                snprintf(line, sizeof(line),
                         "    else live = \"%s\";\n\n", live);
                if (!add(out, cap, &used, line)) return 0;
                any = 1;
            }
            /*
             * A plain host name covers its subdomains, exactly as
             * gw_host_matches() does for the proxy itself -- shExpMatch has
             * no such rule, so the script has to say both. A script that
             * routed differently from the proxy it configures would be worse
             * than no script at all.
             */
            if (strchr(pattern, '*') == NULL && strchr(pattern, '?') == NULL)
                snprintf(line, sizeof(line),
                         "    if (shExpMatch(host, \"%s\") ||\n"
                         "        shExpMatch(host, \"*.%s\")) return live;\n",
                         pattern, pattern);
            else
                snprintf(line, sizeof(line),
                         "    if (shExpMatch(host, \"%s\")) return live;\n",
                         pattern);
            if (!add(out, cap, &used, line)) return 0;
        }
        if (any && !add(out, cap, &used, "\n")) return 0;

        snprintf(line, sizeof(line),
                 "    // Everything else, from the archive.\n"
                 "    return \"PROXY %s:%d\";\n"
                 "}\n", authority, archive_port);
    } else {
        snprintf(line, sizeof(line),
                 "    // Everything else.\n"
                 "    return \"%s\";\n"
                 "}\n", live);
    }
    if (!add(out, cap, &used, line)) return 0;

    return used;
}
