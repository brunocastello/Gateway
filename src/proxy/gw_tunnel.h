/*
 * gw_tunnel.h - the generic TLS tunnel ("socat") listener.
 *
 * A local application connects in plaintext; Gateway carries the far leg,
 * optionally through a forward proxy, and optionally inside TLS:
 *
 *   local_app --plain--> :tunnel_local_port --TLS--> [proxy] --> remote:port
 *
 * The canonical use is SSH: an SSH client points at the local port, Gateway
 * opens TLS to a TLS-wrapping endpoint (stunnel and friends) -- directly or
 * via an HTTP CONNECT / SOCKS5 proxy -- and splices bytes both ways. The far
 * side unwraps TLS and hands the SSH stream onward. SSH is the use case, not
 * the protocol: the tunnel forwards whatever bytes the local port receives.
 *
 * Platform independent (gw_transport.h names no operating system). The proxy
 * handshake grammar lives in src/portable/gw_fwd.c so the host tests can
 * exercise it; this file is only the state machine and the buffers.
 */
#ifndef GW_TUNNEL_H
#define GW_TUNNEL_H

#include "../net/gw_transport.h"

/* Long-lived sessions, not short requests: four is plenty. */
#define GW_MAX_TUNNEL_SESSIONS 4

void GWTunnel_Init(void);
void GWTunnel_Shutdown(void);

/*
 * Take ownership of an accepted client connection. Returns 0 when every slot
 * is busy or the prefs do not describe a usable tunnel, in which case the
 * caller must dispose of the connection.
 */
int  GWTunnel_Accept(GWConn *c);

/* True when a session slot is free; poll the listener only when it is. */
int  GWTunnel_CanAccept(void);

/* One cooperative slice across all live sessions. */
void GWTunnel_Poll(void);

int  GWTunnel_ActiveCount(void);

#endif /* GW_TUNNEL_H */
