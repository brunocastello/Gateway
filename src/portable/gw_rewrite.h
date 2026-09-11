/*
 * gw_rewrite.h - turning https:// into http:// on the way past.
 *
 * Why this exists at all: a 1997 browser cannot speak to a 2026 server, and
 * when it meets an https:// link it does not ask Gateway for the page, it opens
 * a CONNECT tunnel and tries its own handshake. Gateway is a byte pipe at that
 * point and the handshake fails -- Internet Explorer says "an error occurred in
 * the secure channel support", Netscape 4 says "no common encryption
 * algorithm(s)". Neither message mentions a proxy and neither is Gateway's
 * fault, but the page is still broken.
 *
 * So the links are rewritten before the browser ever sees them. It asks for
 * http://, Gateway does the TLS, and the browser never learns there was any.
 * The plaintext hop is the loopback interface of the machine Gateway is running
 * on, so nothing is exposed that was not already on that machine.
 *
 * What this does not reach: a URL the user types by hand (the browser has
 * decided before there is any content to rewrite), https:// built up by string
 * concatenation in JavaScript, percent- or backslash-escaped forms, and
 * Secure-flagged cookies, which the browser will now decline to send.
 */
#ifndef GW_REWRITE_H
#define GW_REWRITE_H

#include <stddef.h>

/*
 * The most bytes that may have to be carried to the next chunk: "https:/" is
 * seven characters that are still on their way to being a match.
 */
#define GW_REWRITE_HOLD 7

/*
 * Rewrite in place, shortening. Returns the new length.
 *
 * `*hold` comes back as the number of bytes at the END of the result that are
 * an incomplete "https://" and must not be sent yet -- the caller keeps them
 * and puts them in front of the next chunk. At the end of the body there is no
 * next chunk, so whatever is held is by definition not a match and can go out
 * as it stands.
 *
 * Matching is case-insensitive, because HTML written by hand is not consistent
 * about it. The replacement is always lower case, which every client parses.
 */
size_t gw_rewrite_https(char *buf, size_t len, size_t *hold);

/*
 * Whether a Content-Type is something whose bytes are worth reading for links.
 * HTML, CSS and JavaScript, plus the XHTML the era's stricter pages use.
 * Deliberately not JSON, and never anything binary: a JPEG that happens to
 * contain the bytes "https://" would be corrupted by one byte and the failure
 * would look like a decoder bug.
 */
int gw_rewrite_wants_type(const char *content_type);

#endif /* GW_REWRITE_H */
