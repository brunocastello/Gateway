/*
 * gw_core.h - the boundary between the Toolbox UI and the network core.
 *
 * PORTABLE BY DESIGN: plain C declarations only, no Mac headers. src/main.cpp
 * is compiled against the Multiversal Interfaces and must never see Open
 * Transport (CLAUDE.md rule 3), so this header is the whole of its view into
 * the proxy. Everything behind it is compiled with Apple's Universal
 * Interfaces on the include path.
 */
#ifndef GW_CORE_H
#define GW_CORE_H

#include "portable/gw_wayback.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 when at least one listener came up. */
int         GW_Init(void);
void        GW_Shutdown(void);

/* One cooperative slice. Call once per pass of the WaitNextEvent loop. */
void        GW_Poll(void);

/* Log ring, for the window. */
int         GW_LogCount(void);
const char *GW_LogLine(int idx);
long        GW_LogGeneration(void);

/* The Phase 1 debug line: negotiated TLS version and last HTTP status. */
void        GW_SetStatus(const char *fmt, ...);

/* Append a line to the log the window shows. */
void        GW_Log(const char *fmt, ...);
const char *GW_StatusLine(void);

int         GW_ActiveSessions(void);

/* 0 auto, 1 always, 2 never. See GWRedirectPolicy in gw_http.h. */
int         GW_RedirectPolicy(void);

/* Ceiling on a relayed response body, in bytes; 0 means no limit. */
long        GW_MaxBodyBytes(void);

/*
 * Hard ceiling on concurrent HTTP splices. The max_sessions setting is
 * clamped to this, because each session costs roughly 110 KB and the
 * application partition is 8 MB (docs/inventory.md).
 */
#define GW_SESSION_LIMIT 16

/* How many splices to run at once, from max_sessions. */
int         GW_MaxSessions(void);

/* How many upstream connections may be opening at once (max_connects). */
int         GW_MaxConnects(void);

/* ---- Module 3: the Wayback listener ---------------------------------- */

int         GW_WaybackPort(void);

/*
 * The one set of Wayback settings, shared by every client. Deliberately
 * global: the workflow is to set an era from the settings page, browse, then
 * set it again -- see docs/module3-wayback.md section 7a.
 */
GWWaybackSettings *GW_WaybackSettings(void);

/* Persist date and tolerance; the checkboxes stay session-only. */
void        GW_WaybackSave(void);

/* 1 when the host is on the allow-list and should be fetched live. */
int         GW_WaybackHostIsLive(const char *host);

/* 1 when the settings page should answer on the Wayback listener. */
int         GW_WaybackServesSettings(void);

/* 1 when archived responses should be made long-lived for the browser cache. */
int         GW_WaybackCaches(void);

/*
 * Read the prefs before the Toolbox side decides what to put on screen.
 * Separate from GW_Init because the window has to be created knowing this.
 */
void        GW_LoadSettings(void);

/*
 * 1 when Gateway should show its window and appear in the Application menu,
 * 0 when it should run as a faceless background application. From the
 * show_window setting; defaults to showing.
 */
int         GW_ShowWindowPref(void);

/* Remember the choice, so the next launch starts the way this one ended. */
void        GW_SetShowWindowPref(int show);

int         GW_HttpPort(void);
int         GW_ImapPort(void);
int         GW_PopPort(void);
int         GW_SmtpPort(void);

#ifdef __cplusplus
}
#endif

#endif /* GW_CORE_H */
