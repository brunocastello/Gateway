/*
 * gw_net_win32.h - the Winsock shape of gw_transport.h's two objects.
 *
 * The counterpart of gw_net.h: everything callers speak to is in
 * gw_transport.h, and this is only what the implementation needs of itself.
 * Nothing outside src/net/ should include it.
 */
#ifndef GW_NET_WIN32_H
#define GW_NET_WIN32_H

#include <winsock.h>
#include <windows.h>

#include "gw_transport.h"
#include "../portable/gw_log.h"

#endif /* GW_NET_WIN32_H */
