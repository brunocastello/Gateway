#include "gw_chunked.h"

#include <string.h>

void gw_chunked_init(GWChunked *c)
{
    memset(c, 0, sizeof(*c));
    c->state = kGWChunkSize;
    c->remaining = 0;
    c->seen_digit = 0;
}

static int hexval(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

long gw_chunked_feed(GWChunked *c, const char *in, size_t in_len,
                     char *out, size_t out_cap, size_t *out_len)
{
    size_t i = 0;
    size_t produced = 0;

    *out_len = 0;
    if (c->state == kGWChunkError) return -1;

    while (i < in_len && c->state != kGWChunkDone) {
        unsigned char ch = (unsigned char)in[i];

        switch (c->state) {
        case kGWChunkSize: {
            int hv = hexval(ch);
            if (hv >= 0 && !c->cr) {
                if (c->remaining > 0x00FFFFFFL) {   /* 16 MiB chunk: refuse */
                    c->state = kGWChunkError;
                    return -1;
                }
                c->remaining = c->remaining * 16 + hv;
                c->seen_digit = 1;
                i++;
                break;
            }
            if (ch == '\r') { c->cr = 1; i++; break; }
            if (ch == '\n') {
                if (!c->seen_digit) { c->state = kGWChunkError; return -1; }
                c->cr = 0;
                c->seen_digit = 0;
                if (c->remaining == 0) {
                    c->state = kGWChunkTrailer;
                    /* A trailer section that starts with a blank line ends
                     * immediately, which is the common no-trailer case. */
                    c->trailer_blank = 1;
                } else {
                    c->state = kGWChunkData;
                }
                i++;
                break;
            }
            /* A chunk extension (";name=value") starts here. Setting cr
             * stops the digit scanner, since extension text may itself
             * contain hex characters. */
            c->cr = 1;
            i++;
            break;
        }

        case kGWChunkData: {
            size_t avail = in_len - i;
            size_t room = out_cap - produced;
            size_t n = (size_t)c->remaining;

            if (room == 0) goto out;
            if (n > avail) n = avail;
            if (n > room) n = room;
            memcpy(out + produced, in + i, n);
            produced += n;
            i += n;
            c->remaining -= (long)n;
            if (c->remaining == 0) c->state = kGWChunkDataCRLF;
            break;
        }

        case kGWChunkDataCRLF:
            if (ch == '\n') { c->state = kGWChunkSize; c->remaining = 0; }
            i++;
            break;

        case kGWChunkTrailer:
            if (ch == '\n') {
                if (c->trailer_blank) { c->state = kGWChunkDone; i++; break; }
                c->trailer_blank = 1;
            } else if (ch != '\r') {
                c->trailer_blank = 0;
            }
            i++;
            break;

        default:
            goto out;
        }
    }

out:
    *out_len = produced;
    return (long)i;
}

int gw_chunked_done(const GWChunked *c)
{
    return c->state == kGWChunkDone;
}
