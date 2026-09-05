/*
 * gw_config.h - Gateway's preference file.
 *
 * Plain C, no Mac headers, so the UI side may read settings too. The file is
 * "Gateway Prefs" in the System Preferences folder; the parsing rules live in
 * src/portable/gw_prefs.c and are exercised by the host tests.
 *
 * A minimal file for Outlook.com looks like:
 *
 *     http_port      = 8765
 *     imap_port      = 1993
 *     smtp_port      = 1587
 *     local_password = whatever-you-typed-into-Outlook-Express
 *     oauth_user     = you@outlook.com
 *     oauth_client_id = 00000000-0000-0000-0000-000000000000
 *     refresh_token  = M.C5...
 *
 * The refresh token is obtained out of band; Gateway never runs the consent
 * flow itself (CLAUDE.md non-goals).
 */
#ifndef GW_CONFIG_H
#define GW_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Read the prefs file. Safe to call again to pick up edits. */
void        GWConfig_Load(void);
int         GWConfig_Loaded(void);

const char *GWConfig_Str(const char *key, const char *def);
long        GWConfig_Num(const char *key, long def);

/* Human-readable note about where the settings came from, for the log. */
const char *GWConfig_Source(void);

#ifdef __cplusplus
}
#endif

#endif /* GW_CONFIG_H */
