#include "gw_update.h"

#include <stdio.h>
#include <string.h>

int gw_update_parse_tag(const char *location, char *out, size_t cap)
{
    const char *marker, *last, *tag;
    size_t      n;
    int         dots;

    if (out != NULL && cap > 0) out[0] = '\0';
    if (location == NULL || out == NULL || cap == 0) return 0;

    /* The last "/tag/" in the value, so an earlier path segment cannot be
     * mistaken for one. */
    last = NULL;
    for (marker = location; (marker = strstr(marker, "/tag/")) != NULL;
         marker++)
        last = marker;
    if (last == NULL) return 0;

    tag = last + 5;
    if (*tag == 'v' || *tag == 'V') tag++;

    n = 0;
    dots = 0;
    while (tag[n] != '\0' && n + 1 < cap) {
        char c = tag[n];

        if (c >= '0' && c <= '9') {
            out[n] = c;
        } else if (c == '.') {
            if (n == 0 || out[n - 1] == '.') break;   /* no leading/doubled dot */
            out[n] = c;
            dots++;
        } else {
            break;
        }
        n++;
    }
    if (n == 0 || dots == 0 || out[n - 1] == '.') { out[0] = '\0'; return 0; }
    out[n] = '\0';
    return 1;
}

/*
 * One dotted component. Advances *p past it (and the following dot, if any)
 * and returns 1, or leaves *p at the string's end and returns 0 when there is
 * no digit to read -- either the string ran out or it holds something that
 * is not a version, and either way there is nothing more this side can say.
 */
static int update_component(const char **p, long *out)
{
    const char *s = *p;
    long        v = 0;
    int         n = 0;

    while (s[n] >= '0' && s[n] <= '9') {
        v = v * 10 + (s[n] - '0');
        n++;
    }
    if (n == 0) {
        *p = s + strlen(s);
        *out = 0;
        return 0;
    }
    s += n;
    if (*s == '.') s++;
    *p = s;
    *out = v;
    return 1;
}

int gw_update_is_newer(const char *tag, const char *current)
{
    const char *a, *b;

    if (tag == NULL || current == NULL || tag[0] == '\0' ||
        current[0] == '\0')
        return 0;

    a = tag;
    b = current;
    while (*a != '\0' || *b != '\0') {
        long va, vb;

        update_component(&a, &va);
        update_component(&b, &vb);
        if (va != vb) return va > vb;
    }
    return 0;                                   /* equal through the end */
}

size_t gw_update_asset_name(const char *tag, GWUpdatePlatform plat,
                            char *out, size_t cap)
{
    int n;

    if (out != NULL && cap > 0) out[0] = '\0';
    if (tag == NULL || out == NULL || cap == 0) return 0;

    if (plat == kGWUpdateWin32)
        n = snprintf(out, cap, "Gateway-v%s-windows.zip", tag);
    else
        n = snprintf(out, cap, "Gateway-v%s.sit", tag);

    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return 0; }
    return (size_t)n;
}

size_t gw_update_asset_url(const char *tag, const char *asset,
                           char *out, size_t cap)
{
    int n;

    if (out != NULL && cap > 0) out[0] = '\0';
    if (tag == NULL || asset == NULL || out == NULL || cap == 0) return 0;

    n = snprintf(out, cap,
        "http://github.com/brunocastello/Gateway/releases/download/v%s/%s",
        tag, asset);
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return 0; }
    return (size_t)n;
}
