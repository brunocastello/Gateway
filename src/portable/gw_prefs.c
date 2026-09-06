#include "gw_prefs.h"
#include "gw_util.h"

#include <string.h>

/*
 * Find the end of the current line and the start of the next one.
 *
 * Three line endings have to work here. A prefs file typed on the Mac itself
 * ends lines with CR, one moved over from a modern machine ends them with LF,
 * and one that has been through a Windows editor uses CRLF. Splitting on LF
 * alone made a Mac-authored file look like a single line, which silently
 * turned every setting into its default.
 */
static void gw_prefs_line(const char *text, size_t len, size_t off,
                          size_t *line_end, size_t *next)
{
    size_t i = off;

    while (i < len && text[i] != '\n' && text[i] != '\r') i++;
    *line_end = i;

    if (i < len && text[i] == '\r' && i + 1 < len && text[i + 1] == '\n')
        *next = i + 2;                      /* CRLF counts as one ending */
    else if (i < len)
        *next = i + 1;
    else
        *next = len;
}

int gw_prefs_get_nth(const char *text, size_t len, const char *key, int n,
                     char *out, size_t cap)
{
    size_t off = 0;
    size_t klen = strlen(key);
    int    seen = 0;

    if (cap) out[0] = '\0';

    while (off < len) {
        size_t line_end, next, i, vs;

        gw_prefs_line(text, len, off, &line_end, &next);

        /* trim leading blanks */
        i = off;
        while (i < line_end && (text[i] == ' ' || text[i] == '\t')) i++;

        if (i < line_end && text[i] != '#' && text[i] != ';') {
            size_t ke = i;
            while (ke < line_end && text[ke] != '=' && text[ke] != ':') ke++;
            if (ke < line_end) {
                size_t kend = ke;
                while (kend > i && (text[kend - 1] == ' ' ||
                                    text[kend - 1] == '\t')) kend--;
                if (kend - i == klen && gw_strnicmp(text + i, key, klen) == 0) {
                    if (seen++ == n) {
                        size_t end = line_end;
                        vs = ke + 1;
                        while (vs < end && (text[vs] == ' ' ||
                                            text[vs] == '\t')) vs++;
                        while (end > vs && (text[end - 1] == ' ' ||
                                            text[end - 1] == '\t')) end--;
                        gw_copy_n(out, cap, text + vs, end - vs);
                        return out[0] != '\0';
                    }
                }
            }
        }
        off = next;
    }
    return 0;
}

int gw_prefs_get(const char *text, size_t len, const char *key,
                 char *out, size_t cap)
{
    return gw_prefs_get_nth(text, len, key, 0, out, cap);
}

long gw_prefs_get_num(const char *text, size_t len, const char *key, long def)
{
    char buf[32];
    long v;

    if (!gw_prefs_get(text, len, key, buf, sizeof(buf))) return def;
    v = gw_parse_dec(buf, strlen(buf));
    return v < 0 ? def : v;
}

/* The line ending the file already uses, so an edit does not mix conventions. */
static const char *gw_prefs_eol(const char *text, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (text[i] == '\r')
            return (i + 1 < len && text[i + 1] == '\n') ? "\r\n" : "\r";
        if (text[i] == '\n')
            return "\n";
    }
    return "\r";           /* a new file on this machine: Mac convention */
}

size_t gw_prefs_set(const char *text, size_t len, const char *key,
                    const char *value, char *out, size_t cap)
{
    size_t off = 0;
    size_t used = 0;
    size_t klen = strlen(key);
    size_t vlen = strlen(value);
    const char *eol = gw_prefs_eol(text, len);
    size_t eol_len = strlen(eol);
    int replaced = 0;

    while (off < len) {
        size_t line_end, next, i;
        int is_match = 0;

        gw_prefs_line(text, len, off, &line_end, &next);

        i = off;
        while (i < line_end && (text[i] == ' ' || text[i] == '\t')) i++;

        if (i < line_end && text[i] != '#' && text[i] != ';') {
            size_t ke = i;
            while (ke < line_end && text[ke] != '=' && text[ke] != ':') ke++;
            if (ke < line_end) {
                size_t kend = ke;
                while (kend > i && (text[kend - 1] == ' ' ||
                                    text[kend - 1] == '\t')) kend--;
                if (kend - i == klen && gw_strnicmp(text + i, key, klen) == 0)
                    is_match = 1;
            }
        }

        if (is_match && !replaced) {
            /* Rewrite in place, keeping the key exactly as the user typed it. */
            size_t ke = i;
            while (ke < line_end && text[ke] != '=' && text[ke] != ':') ke++;
            if (used + (ke - off) + 2 + vlen + eol_len > cap) return 0;
            memcpy(out + used, text + off, ke - off);
            used += ke - off;
            out[used++] = text[ke];         /* the separator they used */
            out[used++] = ' ';
            memcpy(out + used, value, vlen);
            used += vlen;
            memcpy(out + used, eol, eol_len);
            used += eol_len;
            replaced = 1;
        } else {
            size_t n = next - off;
            if (used + n > cap) return 0;
            memcpy(out + used, text + off, n);
            used += n;
        }
        off = next;
    }

    if (!replaced) {
        if (used > 0 && out[used - 1] != '\n' && out[used - 1] != '\r') {
            if (used + eol_len > cap) return 0;
            memcpy(out + used, eol, eol_len);
            used += eol_len;
        }
        if (used + klen + 3 + vlen + eol_len > cap) return 0;
        memcpy(out + used, key, klen);
        used += klen;
        out[used++] = ' ';
        out[used++] = '=';
        out[used++] = ' ';
        memcpy(out + used, value, vlen);
        used += vlen;
        memcpy(out + used, eol, eol_len);
        used += eol_len;
    }
    return used;
}
