/*
 * gw_wayback.h - Module 3: serving the old web from the Internet Archive.
 *
 * PORTABLE: no Mac or Windows headers. See gw_util.h.
 *
 * Requests arriving on the Wayback listener are rewritten to point at the
 * Internet Archive at a configured date, unless the host is on the allow-list
 * of sites to fetch live. The design, and the reasoning behind the choices
 * here, is in docs/module3-wayback.md.
 *
 * Prior art: WaybackProxy (github.com/richardg867/WaybackProxy), which is
 * GPL-3 and was not consulted as source. This is written from the Wayback
 * Machine's public URL scheme, and keeps that project's settings-page field
 * names so existing bookmarks continue to work.
 */
#ifndef GW_WAYBACK_H
#define GW_WAYBACK_H

#include <stddef.h>

#include "gw_url.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A Wayback timestamp is 14 digits: YYYYMMDDhhmmss. */
#define GW_WB_STAMP     15
#define GW_WB_HOST      "web.archive.org"

/* The settings the page edits, all of them global to the proxy by design. */
typedef struct {
    char date[GW_WB_STAMP];     /* YYYY, YYYYMM or YYYYMMDD */
    long tolerance;             /* days after date to accept; 0 = no limit */
    int  geocities;
    int  quick_images;
    int  ct_encoding;
} GWWaybackSettings;

/*
 * Build the archive request path for an origin URL:
 *
 *   /web/<date>id_/http://host[:port]/path
 *
 * The "id_" modifier is what makes this tractable: the archive returns the
 * bytes the origin server sent, with no toolbar, no injected script and no
 * rewritten links, so nothing in the body needs editing. See
 * docs/module3-wayback.md for the measurements behind that choice.
 *
 * Returns the length written, or 0 if it would not fit.
 */
size_t gw_wayback_path(const char *date, const GWUrl *origin,
                       char *out, size_t cap);

/*
 * Take an archive URL apart. Accepts both the absolute form
 * ("https://web.archive.org/web/<ts>id_/http://...") and the rooted form the
 * archive uses in Location headers ("/web/<ts>/http://...").
 *
 * Fills stamp with the 14-digit snapshot timestamp and original with the URL
 * the snapshot is of. Returns 1 when the input was an archive URL.
 */
int gw_wayback_parse(const char *url, size_t len,
                     char *stamp, size_t stamp_cap,
                     char *original, size_t original_cap);

/*
 * Day number for a Wayback timestamp or a partial date. Accepts YYYY, YYYYMM,
 * YYYYMMDD and full 14-digit stamps; a missing month or day counts as the
 * first. Returns -1 when the input is not a date.
 */
long gw_wayback_daynum(const char *stamp);

/*
 * Whether a snapshot is close enough to the date that was asked for. A
 * tolerance of 0 accepts anything. Snapshots *before* the target are always
 * accepted -- asking for 2001 and receiving 1999 is the archive saying that is
 * the best it has.
 */
int gw_wayback_in_tolerance(const char *target, const char *snapshot,
                            long tolerance);

/* GeoCities moved to OoCities. Returns 1 when out was rewritten. */
int gw_wayback_geocities_host(const char *host, char *out, size_t cap);

/*
 * Render the settings page. Plain HTML only: the clients are Internet Explorer
 * 4, Netscape 3 and iCab. Returns bytes written, or 0 if it would not fit.
 */
size_t gw_wayback_settings_page(const GWWaybackSettings *s,
                                char *out, size_t cap);

/*
 * Apply a settings-page query string. Checkboxes are present as "name=on" and
 * absent when unchecked, so absence means false -- the one part of this that
 * is easy to get wrong.
 *
 * When targetUrl was filled in, it is written to target and 1 is returned,
 * meaning the caller should save the settings and redirect there.
 */
int gw_wayback_apply_query(const char *query, size_t len,
                           GWWaybackSettings *s,
                           char *target, size_t target_cap);

#ifdef __cplusplus
}
#endif

#endif /* GW_WAYBACK_H */
