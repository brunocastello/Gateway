/*
 * gw_hello.h - the cipher list a ClientHello offered, decoded for the log.
 *
 * PORTABLE: no Mac or Windows headers.
 *
 * `log_debug`'s S03-S05 lines (docs/log-codes.md) already say the hello's
 * framing, version and chosen suite, plus a hex dump of its opening bytes
 * (MacTLS_ServerHelloHex() / GWStream_ServerHelloHex()). That dump is one
 * line and cannot fit a whole cipher list, which is often the only useful
 * clue when a browser gives up on our certificate -- the suite it chose is
 * not the story, the suites it offered are. This decodes that list from the
 * same captured bytes, for one more log_debug-only line (PATCHES.md §37).
 *
 * Two framings, both already handled elsewhere in Certainly for the same
 * bytes:
 *
 *   - An SSLv2-compatible hello (leading byte's high bit set): a 2-byte
 *     length header, msg-type, version, three 2-byte lengths (cipher specs,
 *     session ID, challenge), then the cipher specs themselves, 3 bytes
 *     each.
 *   - A native SSL 3.0 / TLS record (0x16 0x03 ..): a 5-byte record header,
 *     a 4-byte handshake header, 2-byte client version, 32-byte random, a
 *     1-byte session ID length and that many bytes, a 2-byte cipher-suite
 *     list length, then the suites, 2 bytes each.
 */
#ifndef GW_HELLO_H
#define GW_HELLO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode the cipher list from hello[0..len), writing it as hex, space-
 * separated -- three bytes per entry for an SSLv2 hello, two for a native
 * one -- into out (always NUL-terminated if cap > 0; entries that would not
 * fit are simply left off rather than truncated mid-entry).
 *
 * Returns the number of complete entries written. 0 means either framing
 * was not recognised, the hello was too short to reach the list, or the
 * list itself was empty.
 *
 * *truncated is set to 1 when the captured bytes end before the hello's own
 * length fields say they should -- whether that cuts the list itself short,
 * or cuts off before its length can even be read -- so the caller can say so
 * rather than silently showing a partial list as the whole one, or showing
 * nothing at all with no explanation. Left untouched (so the caller should
 * pre-clear it) when the framing was not recognised.
 */
size_t gw_hello_ciphers(const unsigned char *hello, size_t len,
                        char *out, size_t cap, int *truncated);

#ifdef __cplusplus
}
#endif

#endif /* GW_HELLO_H */
