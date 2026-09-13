/*
 * gw_pac.h - the proxy auto-configuration file Gateway serves for itself.
 *
 * A browser that supports automatic configuration is given one URL and works
 * the rest out per host, which is the only way to express the thing Gateway
 * cannot otherwise say: that some hosts belong on the archive listener and
 * some on the live one. Every browser this program targets has it -- Internet
 * Explorer 4 and up, Netscape 3 and up, Classilla, iCab -- under "Use
 * automatic configuration script" or "Automatic proxy configuration".
 *
 * This is auto-*configuration*, not auto-*detection*: WPAD needs DHCP option
 * 252 or a `wpad` DNS name, neither of which Gateway can hand out. The user
 * pastes the URL once.
 *
 * CLAUDE.md rule 8: portable, so the generator is exercised by the host tests.
 */
#ifndef GW_PAC_H
#define GW_PAC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fetches allow-list pattern number `index` into `out`, returning 0 when
 * there are no more. The patterns are globs against a host name, the same
 * ones GW_WaybackHostIsLive() matches, and they reach the script as
 * shExpMatch() arguments unchanged -- the two syntaxes agree on `*` and `?`,
 * which is the whole of what a host pattern uses.
 *
 * A callback rather than an array because the patterns live in the
 * preferences, and reading those is not portable.
 */
typedef int (*GWPacNextHost)(int index, char *out, size_t cap);

/*
 * Write the script. Returns the number of bytes written, or 0 if it would not
 * fit -- in which case `out` holds nothing worth sending.
 *
 *   authority     the host the browser used to reach Gateway, taken from the
 *                 Host header of the request for the script itself, so the
 *                 addresses in it are ones that browser can reach. An IP or a
 *                 name, without a port.
 *   live_port     the proxy listener for the live web.
 *   archive_port  the Wayback listener, or 0 when it is not running, in which
 *                 case everything is routed to live_port and the allow-list
 *                 does not appear at all.
 *   next_host     the allow-list, or NULL for none.
 */
size_t gw_pac_build(const char *authority, int live_port, int archive_port,
                    GWPacNextHost next_host, char *out, size_t cap);

/*
 * 1 when `path` is a request for the script. Matches /proxy.pac and
 * /wpad.dat, with or without a query string: the first is what a person
 * types, the second is the name WPAD uses, and answering both costs one
 * comparison.
 */
int gw_pac_is_request(const char *path);

/* What the script has to be served as. Internet Explorer and Netscape both
 * want this exact type and neither accepts text/plain. */
#define GW_PAC_CONTENT_TYPE "application/x-ns-proxy-autoconfig"

#ifdef __cplusplus
}
#endif

#endif /* GW_PAC_H */
