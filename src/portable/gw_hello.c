/*
 * gw_hello.c - see gw_hello.h.
 */
#include "gw_hello.h"

#include <stdio.h>

/* Append one entry's hex bytes, space-separated from whatever came before.
 * Returns 0 and leaves out untouched (past *used) if it would not fit,
 * which is how the caller stops rather than truncating an entry. */
static int append_entry(char *out, size_t cap, size_t *used,
                        const unsigned char *b, size_t n)
{
    char   tmp[8];
    size_t tn = 0, k, sep = (*used > 0) ? 1 : 0;
    int    w;

    for (k = 0; k < n; k++) {
        w = snprintf(tmp + tn, sizeof(tmp) - tn, "%02x", b[k]);
        if (w < 0 || (size_t)w >= sizeof(tmp) - tn) return 0;
        tn += (size_t)w;
    }
    if (*used + sep + tn >= cap) return 0;
    if (sep) out[(*used)++] = ' ';
    for (k = 0; k < tn; k++) out[(*used)++] = tmp[k];
    out[*used] = '\0';
    return 1;
}

size_t gw_hello_ciphers(const unsigned char *hello, size_t len,
                        char *out, size_t cap, int *truncated)
{
    size_t used = 0, count = 0;

    if (out != NULL && cap > 0) out[0] = '\0';
    if (hello == NULL || len == 0 || out == NULL || cap == 0) return 0;

    if (hello[0] & 0x80) {
        /*
         * SSLv2-compatible hello: 2-byte length header, then msg-type(1),
         * version(2), cipher-spec-length(2), session-id-length(2),
         * challenge-length(2) -- 11 bytes -- then the cipher specs.
         */
        size_t spec_len, avail, off, taken;

        if (len < 7) {                       /* not even the three lengths */
            if (truncated != NULL) *truncated = 1;
            return 0;
        }
        spec_len = ((size_t)hello[5] << 8) | hello[6];
        if (len < 11) {
            if (truncated != NULL) *truncated = 1;
            return 0;
        }
        avail = len - 11;
        taken = spec_len < avail ? spec_len : avail;
        if (spec_len > avail && truncated != NULL) *truncated = 1;

        for (off = 0; off + 3 <= taken; off += 3) {
            if (!append_entry(out, cap, &used, hello + 11 + off, 3)) {
                if (truncated != NULL) *truncated = 1;
                break;
            }
            count++;
        }
        return count;
    }

    if (hello[0] == 0x16) {
        /*
         * Native SSL 3.0 / TLS record: 5-byte record header, 4-byte
         * handshake header, 2-byte client version, 32-byte random, then a
         * 1-byte session-id length and that many bytes, then a 2-byte
         * cipher-suite-list length and the suites.
         */
        size_t pos, sid_len, cs_len, avail, off, taken;

        pos = 5 + 4 + 2 + 32;                /* up to the session ID length */
        if (len <= pos) {
            if (truncated != NULL) *truncated = 1;
            return 0;
        }
        sid_len = hello[pos];
        pos += 1 + sid_len;
        if (len < pos + 2) {
            if (truncated != NULL) *truncated = 1;
            return 0;
        }
        cs_len = ((size_t)hello[pos] << 8) | hello[pos + 1];
        pos += 2;

        avail = len > pos ? len - pos : 0;
        taken = cs_len < avail ? cs_len : avail;
        if (cs_len > avail && truncated != NULL) *truncated = 1;

        for (off = 0; off + 2 <= taken; off += 2) {
            if (!append_entry(out, cap, &used, hello + pos + off, 2)) {
                if (truncated != NULL) *truncated = 1;
                break;
            }
            count++;
        }
        return count;
    }

    return 0;
}
