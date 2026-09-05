#include "gw_util.h"

#include <string.h>

int gw_lower(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

int gw_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        int d = gw_lower((unsigned char)*a) - gw_lower((unsigned char)*b);
        if (d) return d;
        a++; b++;
    }
    return gw_lower((unsigned char)*a) - gw_lower((unsigned char)*b);
}

int gw_strnicmp(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int d = gw_lower((unsigned char)a[i]) - gw_lower((unsigned char)b[i]);
        if (d) return d;
        if (a[i] == '\0') return 0;
    }
    return 0;
}

int gw_starts_ci(const char *s, size_t len, const char *prefix)
{
    size_t plen = strlen(prefix);
    if (len < plen) return 0;
    return gw_strnicmp(s, prefix, plen) == 0;
}

size_t gw_copy_n(char *dst, size_t cap, const char *src, size_t len)
{
    if (cap == 0) return 0;
    if (len > cap - 1) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return len;
}

int gw_find_head_end(const char *buf, size_t len, size_t *head_len)
{
    size_t i;

    for (i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            i + 3 < len && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            *head_len = i + 4;
            return 1;
        }
        if (buf[i] == '\n' && buf[i + 1] == '\n') {   /* tolerate bare LF */
            *head_len = i + 2;
            return 1;
        }
    }
    return 0;
}

size_t gw_next_line(const char *buf, size_t len, size_t off)
{
    while (off < len && buf[off] != '\n') off++;
    if (off < len) off++;
    return off;
}

const char *gw_header_find(const char *head, size_t head_len,
                           const char *name, size_t *val_len)
{
    size_t off = gw_next_line(head, head_len, 0);   /* skip the start line */
    size_t nlen = strlen(name);

    while (off < head_len) {
        size_t eol = off;
        size_t line_end;

        while (eol < head_len && head[eol] != '\n') eol++;
        line_end = eol;
        if (line_end > off && head[line_end - 1] == '\r') line_end--;

        if (line_end == off) break;                 /* blank line: end of block */

        if (line_end - off > nlen &&
            gw_strnicmp(head + off, name, nlen) == 0 &&
            head[off + nlen] == ':') {
            size_t v = off + nlen + 1;
            while (v < line_end && (head[v] == ' ' || head[v] == '\t')) v++;
            while (line_end > v &&
                   (head[line_end - 1] == ' ' || head[line_end - 1] == '\t'))
                line_end--;
            *val_len = line_end - v;
            return head + v;
        }

        off = eol < head_len ? eol + 1 : head_len;
    }
    return NULL;
}

long gw_parse_dec(const char *s, size_t len)
{
    long v = 0;
    size_t i = 0;
    int any = 0;

    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    for (; i < len && s[i] >= '0' && s[i] <= '9'; i++) {
        if (v > 200000000L) return -1;              /* absurd; refuse */
        v = v * 10 + (s[i] - '0');
        any = 1;
    }
    return any ? v : -1;
}
