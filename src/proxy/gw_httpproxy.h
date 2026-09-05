/*
 * gw_httpproxy.h - Module 1: the HTTP proxy on :8765.
 *
 * NOT PORTABLE (pulls in gw_net.h, hence Open Transport). The request and
 * response grammar lives in src/portable/gw_http.c so it can be unit tested on
 * the host; this file is only the state machine and the buffers.
 */
#ifndef GW_HTTPPROXY_H
#define GW_HTTPPROXY_H

#include "../net/gw_net.h"

/* CLAUDE.md rule 6: start with four concurrent splices. */
#define GW_MAX_SESSIONS 4

void GWProxy_Init(void);
void GWProxy_Shutdown(void);

/* Take ownership of an accepted client connection. Returns 0 when every slot
 * is busy, in which case the caller must dispose of the connection. */
int  GWProxy_Accept(GWConn *c);

/* One cooperative slice across all live sessions. */
void GWProxy_Poll(void);

int  GWProxy_ActiveCount(void);

#endif /* GW_HTTPPROXY_H */
