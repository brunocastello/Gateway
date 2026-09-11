/*
 * gw_plat.h - the platform services Gateway needs that are not networking.
 *
 * Two things, both files: the preferences the whole configuration lives in,
 * and the optional log file. Mac OS 9 reaches them through the Folder and File
 * Managers and Windows through the C library, and neither belongs in the code
 * that decides what to write.
 *
 * gw_plat_mac.c and gw_plat_win32.c are the implementations.
 */
#ifndef GW_PLAT_H
#define GW_PLAT_H

#include <stddef.h>

/*
 * Read the preferences file into buf. Returns the number of bytes read, or -1
 * if there is no preferences file, in which case Gateway runs on defaults.
 */
long GWPlat_ReadPrefs(char *buf, size_t cap);

/* A short description of where they came from, for the log. Never NULL. */
const char *GWPlat_PrefsSource(void);

/*
 * Replace the preferences file with len bytes of buf, truncating whatever was
 * there. Returns 1 on success, 0 on failure, and logs its own reason.
 *
 * Only reached when GWPlat_ReadPrefs() found a file: Gateway rewrites the
 * preferences to save a rotated refresh token, and inventing a file to put one
 * in would be a surprise.
 */
int GWPlat_WritePrefs(const char *buf, long len);

/*
 * Open the log file for appending, creating it and any folders it needs.
 * `want` is the log_file preference verbatim, so a platform can accept a path
 * as well as a switch. Returns 1 if the file is open.
 */
/*
 * A named file beside the preferences, read and written whole.
 *
 * Added for the local certificate authority, which is the first thing Gateway
 * keeps that is neither settings nor a log: an RSA key it generated itself and
 * the certificate over it. Whole-file rather than streaming because the thing
 * being stored is under two kilobytes and there is no reason to hold a file
 * open across the cooperative loop.
 *
 * `leaf` is a bare name -- "Gateway CA", not a path. Each platform puts it
 * where its settings already live: beside the executable on Windows, in the
 * Preferences folder on Mac OS.
 *
 * ReadFile returns the byte count, or -1 if there is no such file. WriteFile
 * returns non-zero on success.
 */
long GWPlat_ReadFile(const char *leaf, void *buf, size_t cap);
int  GWPlat_WriteFile(const char *leaf, const void *buf, long len);

int  GWPlat_OpenLog(const char *want);
void GWPlat_WriteLog(const char *line);
void GWPlat_CloseLog(void);

#endif /* GW_PLAT_H */
