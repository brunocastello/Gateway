#include "gw_b64.h"

static const char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t gw_b64_encode(const void *src, size_t len, char *out, size_t cap)
{
    const unsigned char *p = (const unsigned char *)src;
    size_t need = ((len + 2) / 3) * 4;
    size_t i, o = 0;

    if (cap < need + 1) return 0;

    for (i = 0; i + 2 < len; i += 3) {
        unsigned long v = ((unsigned long)p[i] << 16) |
                          ((unsigned long)p[i + 1] << 8) | p[i + 2];
        out[o++] = kAlphabet[(v >> 18) & 0x3F];
        out[o++] = kAlphabet[(v >> 12) & 0x3F];
        out[o++] = kAlphabet[(v >> 6) & 0x3F];
        out[o++] = kAlphabet[v & 0x3F];
    }

    if (len - i == 1) {
        unsigned long v = (unsigned long)p[i] << 16;
        out[o++] = kAlphabet[(v >> 18) & 0x3F];
        out[o++] = kAlphabet[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (len - i == 2) {
        unsigned long v = ((unsigned long)p[i] << 16) |
                          ((unsigned long)p[i + 1] << 8);
        out[o++] = kAlphabet[(v >> 18) & 0x3F];
        out[o++] = kAlphabet[(v >> 12) & 0x3F];
        out[o++] = kAlphabet[(v >> 6) & 0x3F];
        out[o++] = '=';
    }

    out[o] = '\0';
    return o;
}

static int b64val(int ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

size_t gw_b64_decode(const char *src, size_t len, void *out, size_t cap)
{
    unsigned char *dst = (unsigned char *)out;
    unsigned long acc = 0;
    int bits = 0;
    size_t i, o = 0;

    for (i = 0; i < len; i++) {
        int ch = (unsigned char)src[i];
        int v;

        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
        if (ch == '=') break;

        v = b64val(ch);
        if (v < 0) return (size_t)-1;

        acc = (acc << 6) | (unsigned long)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return (size_t)-1;
            dst[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    return o;
}
