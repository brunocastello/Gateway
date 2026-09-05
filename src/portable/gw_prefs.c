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

int gw_prefs_get(const char *text, size_t len, const char *key,
                 char *out, size_t cap)
{
    size_t off = 0;
    size_t klen = strlen(key);

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
                    vs = ke + 1;
                    while (vs < line_end && (text[vs] == ' ' ||
                                             text[vs] == '\t')) vs++;
                    while (line_end > vs && (text[line_end - 1] == ' ' ||
                                             text[line_end - 1] == '\t'))
                        line_end--;
                    gw_copy_n(out, cap, text + vs, line_end - vs);
                    return out[0] != '\0';
                }
            }
        }
        off = next;
    }
    return 0;
}

long gw_prefs_get_num(const char *text, size_t len, const char *key, long def)
{
    char buf[32];
    long v;

    if (!gw_prefs_get(text, len, key, buf, sizeof(buf))) return def;
    v = gw_parse_dec(buf, strlen(buf));
    return v < 0 ? def : v;
}
