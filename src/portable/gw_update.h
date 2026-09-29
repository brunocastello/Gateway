/*
 * gw_update.h - the arithmetic behind "check for updates".
 *
 * PORTABLE: no Mac or Windows headers. See gw_util.h.
 *
 * The premise (docs/next.md, "Check for updates"): an HTTP/1.0 GET of
 * https://github.com/brunocastello/Gateway/releases/latest answers 302 Found
 * with a Location header naming the latest tag -- no JSON, no
 * api.github.com, no rate limit. src/proxy/gw_updater.c makes that one
 * request per launch and hands the Location value here; everything that
 * decides what it means is portable and host-tested.
 */
#ifndef GW_UPDATE_H
#define GW_UPDATE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pull the release tag out of a Location header value, e.g.
 * ".../releases/tag/v0.3.9" -> "0.3.9". The last "/tag/" in the value is
 * used, and a leading 'v' is dropped; what follows must be digits and dots
 * only, with no leading, trailing or doubled dot. Returns 1 and writes the
 * tag into out, or 0 if the value does not look like a release tag (out is
 * then left as an empty string).
 */
int gw_update_parse_tag(const char *location, char *out, size_t cap);

/*
 * 1 when `tag` is numerically newer than `current`, comparing dotted
 * components left to right (0.3.10 > 0.3.9; a missing trailing component
 * counts as 0). Equal, older, or either string empty answers 0 -- the
 * caller then logs nothing.
 */
int gw_update_is_newer(const char *tag, const char *current);

/* Which platform's asset name to build. */
typedef enum {
    kGWUpdateMac = 0,
    kGWUpdateWin32 = 1
} GWUpdatePlatform;

/*
 * The release asset name for this platform and tag, e.g.
 * "Gateway-v0.3.10.sit" (Mac) or "Gateway-v0.3.10-windows.zip" (Windows) --
 * the primary format of each; the release also carries a .dsk / -windows.img
 * alternative, not linked here. Returns the length written, or 0 if it would
 * not fit.
 */
size_t gw_update_asset_name(const char *tag, GWUpdatePlatform plat,
                            char *out, size_t cap);

/*
 * The direct download link for `asset` under `tag`, e.g.
 * "http://github.com/brunocastello/Gateway/releases/download/v0.3.10/
 * Gateway-v0.3.10.sit". Logged as http:// rather than https: -- the browsers
 * Gateway serves reach it through Gateway's own :8765 proxy, and
 * follow_redirects=auto follows both https hops on the way there (see
 * docs/next.md). Returns the length written, or 0 if it would not fit.
 */
size_t gw_update_asset_url(const char *tag, const char *asset,
                           char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* GW_UPDATE_H */
