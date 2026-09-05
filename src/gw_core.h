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
const char *GW_StatusLine(void);

int         GW_ActiveSessions(void);
int         GW_HttpPort(void);
int         GW_ImapPort(void);
int         GW_PopPort(void);
int         GW_SmtpPort(void);

#ifdef __cplusplus
}
#endif

#endif /* GW_CORE_H */
