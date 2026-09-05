#include "gw_prefs.h"
#include "gw_util.h"

#include <string.h>

int gw_prefs_get(const char *text, size_t len, const char *key,
                 char *out, size_t cap)
{
    size_t off = 0;
    size_t klen = strlen(key);

    if (cap) out[0] = '\0';

    while (off < len) {
        size_t eol = off;
        size_t line_end, i, vs;

        while (eol < len && text[eol] != '\n') eol++;
        line_end = eol;
        if (line_end > off && text[line_end - 1] == '\r') line_end--;

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
        off = eol < len ? eol + 1 : len;
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
