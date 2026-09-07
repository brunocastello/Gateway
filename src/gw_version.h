/*
 * gw_version.h - the version, in one place.
 *
 * Three things have to agree and cannot see each other: the About window on
 * each platform, the 'vers' resource Rez compiles for the Finder, and the
 * VERSIONINFO block Windows shows in a file's properties. At 0.2.0 the
 * resources were bumped and the About box was not, so it went out saying 0.1.
 *
 * Rez and the resource compiler cannot read this header, so those two still
 * carry the number literally -- but they say so, and they name this file.
 */
#ifndef GW_VERSION_H
#define GW_VERSION_H

#define GW_VERSION_MAJOR  0
#define GW_VERSION_MINOR  3
#define GW_VERSION_PATCH  2

#define GW_VERSION_STRING "0.3.2"
#define GW_VERSION_LONG   "0.3.2"

#endif /* GW_VERSION_H */
