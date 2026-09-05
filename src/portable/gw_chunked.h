/*
 * gw_chunked.h - incremental chunked transfer-coding decoder.
 *
 * PORTABLE: no Mac or Windows headers. See gw_util.h.
 *
 * Gateway always speaks HTTP/1.0 with Connection: close to the client, so a
 * chunked origin response has to be decoded on the way through. The decoder
 * is fed whatever arrived from the socket and emits plain body bytes; it never
 * needs the whole body in memory.
 */
#ifndef GW_CHUNKED_H
#define GW_CHUNKED_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    kGWChunkSize = 0,       /* reading the hex size line          */
    kGWChunkData,           /* copying chunk-data                 */
    kGWChunkDataCRLF,       /* eating the CRLF after chunk-data   */
    kGWChunkTrailer,        /* reading trailers after the 0 chunk */
    kGWChunkDone,
    kGWChunkError
} GWChunkState;

typedef struct {
    GWChunkState state;
    long         remaining;     /* bytes left in the current chunk */
    int          seen_digit;
    int          cr;            /* saw a CR while scanning a line  */
    int          trailer_blank; /* consecutive empty trailer lines */
} GWChunked;

void gw_chunked_init(GWChunked *c);

/*
 * Feed in_len bytes. Decoded body bytes are appended to out (capacity
 * out_cap); *out_len receives how many were produced. Returns the number of
 * input bytes consumed, or -1 on a malformed stream. A short return means the
 * output buffer filled up - call again with the remaining input.
 */
long gw_chunked_feed(GWChunked *c, const char *in, size_t in_len,
                     char *out, size_t out_cap, size_t *out_len);

int gw_chunked_done(const GWChunked *c);

#ifdef __cplusplus
}
#endif

#endif /* GW_CHUNKED_H */
