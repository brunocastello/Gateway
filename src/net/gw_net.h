/*
 * gw_net.h - the Open Transport implementation of gw_transport.h.
 *
 * NOT PORTABLE, and deliberately narrow now. Everything the modules above
 * actually speak to has moved to gw_transport.h, which names no operating
 * system; what is left here is the Open Transport shape of the two connection
 * objects and the one call that only makes sense on this platform.
 *
 * This header pulls in <OpenTransport.h>, which exists only in Apple's
 * Universal Interfaces -- the Multiversal Interfaces that Retro68 ships have
 * no Open Transport at all (CLAUDE.md rule 3). Only translation units compiled
 * with the vendored Universal CIncludes on their include path may include it.
 * src/main.cpp must not: it talks to the core through the plain-C gw_core.h,
 * and src/proxy/ now goes through gw_transport.h.
 */
#ifndef GW_NET_H
#define GW_NET_H

#include <OpenTransport.h>
#include <OpenTptInternet.h>

#include <certainly.h>

#include "gw_transport.h"

struct GWConn {
    EndpointRef       ep;
    InetSvcRef        svc;
    InetHostInfo      hinfo;
    InetAddress       remote;
    UInt16            port;
    GWConnState       state;

    /* Written from the notifier, which runs at interrupt time: the notifier
     * only ever sets flags, and GWConn_Pump() does the real work at app time. */
    volatile OSStatus err;
    volatile Boolean  fConnect;
    volatile Boolean  fData;
    volatile Boolean  fOrdRel;
    volatile Boolean  fDisconnect;
    volatile Boolean  fDns;
    volatile Boolean  fPassCon;

    unsigned long     startTicks;
    Boolean           ordRelConsumed;
    Boolean           remoteEOF;
    Boolean           sentFIN;
    char              host[GW_NET_HOST_MAX];
};

struct GWListener {
    EndpointRef       ep;
    UInt16            port;
    Boolean           open;
    volatile Boolean  fListen;
    volatile Boolean  fAcceptDone;
    volatile OSStatus err;

    GWConn           *pending;              /* endpoint an accept is landing on */
    TCall             call;
    InetAddress       callAddr;
};

#endif /* GW_NET_H */
