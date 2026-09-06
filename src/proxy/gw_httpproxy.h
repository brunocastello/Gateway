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

/*
 * How many splices run at once is a preference, not a constant: see
 * GW_MaxSessions() in gw_core.h. CLAUDE.md rule 6 said to start at four, and
 * four turned out to be too few -- a single page of a video site opens more
 * than that in parallel and the surplus was refused, which showed up in the
 * log as "proxy busy, dropped a connection".
 */

void GWProxy_Init(void);
void GWProxy_Shutdown(void);

/*
 * Take ownership of an accepted client connection. Returns 0 when every slot
 * is busy, in which case the caller must dispose of the connection.
 *
 * wayback says which listener it arrived on. That flag is the whole of
 * Module 3's entry point: a session marked with it has its requests rewritten
 * to the Internet Archive, and one without it is an ordinary live-web proxy
 * session. Gateway's own outbound connections -- the OAuth refresh, the mail
 * upstreams -- never come through here at all, which is what keeps them off
 * the archive. See docs/module3-wayback.md section 6.
 */
int  GWProxy_Accept(GWConn *c, int wayback);

/* One cooperative slice across all live sessions. */
void GWProxy_Poll(void);

/*
 * True when a session slot is free. Poll the proxy listeners only when it is:
 * see the comment on the definition for why backpressure beats refusing.
 */
int  GWProxy_CanAccept(void);

int  GWProxy_ActiveCount(void);

#endif /* GW_HTTPPROXY_H */
