/*
 * gw_rewrite.c - see gw_rewrite.h.
 */

#include "gw_rewrite.h"

#include <string.h>

static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

size_t gw_rewrite_https(char *buf, size_t len, size_t *hold)
{
    static const char kFrom[] = "https://";   /* 8 */
    static const char kTo[]   = "http://";    /* 7 */
    const size_t flen = sizeof(kFrom) - 1;
    const size_t tlen = sizeof(kTo) - 1;
    size_t r = 0, w = 0;

    *hold = 0;
    if (buf == NULL || len == 0) return 0;

    while (r < len) {
        /*
         * Only the 'h' is worth looking at, which keeps this a single pass
         * over the body with a comparison per byte rather than a search per
         * byte. At 32 KB a chunk on a 68k-era CPU that difference is the
         * difference between noticing this and not.
         */
        if (lower_ascii(buf[r]) == 'h') {
            size_t avail = len - r;
            size_t n     = (avail < flen) ? avail : flen;
            size_t i;

            for (i = 0; i < n; i++)
                if (lower_ascii(buf[r + i]) != kFrom[i]) break;

            if (i == flen) {
                memcpy(buf + w, kTo, tlen);
                w += tlen;
                r += flen;
                continue;
            }

            /*
             * Matched every byte there was and ran out of buffer, so this may
             * yet be a match once the next read arrives. It can only happen at
             * the very end -- i == avail means r + avail == len -- so stopping
             * here loses nothing. The bytes move down to sit at the end of the
             * result and are reported as held.
             */
            if (i == avail) {
                memmove(buf + w, buf + r, avail);
                w += avail;
                *hold = avail;
                return w;
            }
        }
        buf[w++] = buf[r++];
    }

    return w;
}

int gw_rewrite_wants_type(const char *content_type)
{
    static const char *kTypes[] = {
        "text/",                        /* html, css, plain, xml */
        "application/xhtml",
        "application/javascript",
        "application/x-javascript",
        "application/ecmascript",
        NULL
    };
    size_t i;

    if (content_type == NULL || *content_type == '\0') return 0;

    for (i = 0; kTypes[i] != NULL; i++) {
        size_t n = strlen(kTypes[i]);
        size_t j;

        for (j = 0; j < n; j++)
            if (lower_ascii(content_type[j]) != kTypes[i][j]) break;
        if (j == n) return 1;
    }
    return 0;
}
