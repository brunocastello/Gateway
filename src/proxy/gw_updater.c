#include "gw_updater.h"

#include <stdio.h>
#include <string.h>

#include "../gw_config.h"
#include "../gw_version.h"
#include "../net/gw_transport.h"
#include "../portable/gw_http.h"
#include "../portable/gw_log.h"
#include "../portable/gw_update.h"

#define GW_UPDATE_HOST    "github.com"
#define GW_UPDATE_PATH    "/brunocastello/Gateway/releases/latest"
/*
 * On hardware this logged "the response headers were too large": a real
 * capture of GitHub's 302 for this path runs ~5 KB, almost all of it one
 * Content-Security-Policy header, and 4096 left no room for the blank
 * line that ends the head. gw_http_parse_response() itself has no
 * header-count or line-length limit (tests/host/run_tests.c parses that
 * same ~5 KB capture whole), so the fix is here: a one-shot NewPtr freed
 * right after the check, generous enough for a CSP this size to grow
 * further and still fit.
 */
#define GW_UPDATE_BUF     16384L

/* The whole dial-and-answer budget: a request this small should be done well
 * inside it, and nothing owns this connection but this one check, so there
 * is no reason to wait longer than GWConn already waits for the connect
 * itself (gw_transport.h). */
#define GW_UPDATE_TIMEOUT GW_CONNECT_TIMEOUT

typedef enum {
    kStConnect = 0,
    kStSend,
    kStRecv
} GWUpdaterStep;

typedef enum {
    kUpNotStarted = 0,
    kUpWorking,
    kUpDone
} GWUpdaterState;

typedef struct {
    GWUpdaterState state;
    GWUpdaterStep  step;
    GWStream       up;
    char           req[256];
    size_t         reqLen, reqSent;
    char          *resp;
    size_t         respLen;
} GWUpdaterCtx;

static GWUpdaterCtx *sUp;

void GWUpdater_Init(void)
{
    if (sUp != NULL) return;
    sUp = (GWUpdaterCtx *)NewPtrClear((Size)sizeof(GWUpdaterCtx));
}

void GWUpdater_Shutdown(void)
{
    if (sUp == NULL) return;
    GWStream_Destroy(&sUp->up);
    if (sUp->resp) DisposePtr((Ptr)sUp->resp);
    DisposePtr((Ptr)sUp);
    sUp = NULL;
}

static void updater_done(GWUpdaterCtx *u)
{
    GWStream_Destroy(&u->up);
    if (u->resp) { DisposePtr((Ptr)u->resp); u->resp = NULL; }
    u->respLen = u->reqSent = 0;
    u->state = kUpDone;
}

/*
 * Every way this can end besides "a newer release exists" is quiet in the
 * plain log: a person with no network, or whose TLS to github.com failed,
 * did nothing wrong, and a line that reads like a fault would send them
 * looking for one. The reason still goes under log_debug for whoever is
 * looking.
 */
static void updater_fail(GWUpdaterCtx *u, const char *why)
{
    gw_logd("update check: %s", why);
    updater_done(u);
}

void GWUpdater_Request(void)
{
    GWUpdaterCtx *u = sUp;

    if (u == NULL || u->state != kUpNotStarted) return;

    if (GWConfig_Num("check_updates", 1) == 0) {
        u->state = kUpDone;              /* disabled: no request at all */
        return;
    }

    u->resp = NewPtr(GW_UPDATE_BUF);
    if (u->resp == NULL) { updater_fail(u, "out of memory"); return; }

    u->reqLen = (size_t)snprintf(u->req, sizeof(u->req),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: Gateway/%s\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n",
        GW_UPDATE_PATH, GW_UPDATE_HOST, GW_VERSION_STRING);
    u->reqSent = 0;
    u->respLen = 0;

    if (!GWStream_ConnectTLS(&u->up, GW_UPDATE_HOST, 443)) {
        updater_fail(u, "could not start a connection to github.com");
        return;
    }

    u->state = kUpWorking;
    u->step  = kStConnect;
}

/*
 * The head parsed: a newer release logs the one plain-log line this feature
 * exists for; anything else -- up to date, no redirect, an unparseable tag,
 * a malformed response -- is quiet, per GWUpdater_Request()'s doc comment
 * above and docs/next.md.
 */
static void updater_finish(GWUpdaterCtx *u)
{
    GWResponse       res;
    char             tag[32];
    char             asset[64];
    char             url[256];
    GWUpdatePlatform plat;

    if (gw_http_parse_response(u->resp, u->respLen, &res) != 1) {
        updater_fail(u, "the response could not be understood");
        return;
    }
    if (res.status != 302 || !res.has_location) {
        char why[64];

        snprintf(why, sizeof(why), "no redirect with a Location header "
                 "(HTTP %d)", res.status);
        updater_fail(u, why);
        return;
    }
    if (!gw_update_parse_tag(res.location, tag, sizeof(tag))) {
        updater_fail(u, "the release tag could not be parsed");
        return;
    }
    if (!gw_update_is_newer(tag, GW_VERSION_STRING)) {
        gw_logd("update check: %s is current (latest is %s)",
                GW_VERSION_STRING, tag);
        updater_done(u);
        return;
    }

#ifdef GW_MAC_OS
    plat = kGWUpdateMac;
#else
    plat = kGWUpdateWin32;
#endif

    if (!gw_update_asset_name(tag, plat, asset, sizeof(asset)) ||
        !gw_update_asset_url(tag, asset, url, sizeof(url))) {
        updater_fail(u, "the asset link did not fit");
        return;
    }

    gw_logc("G50", "Gateway %s is available: %s", tag, url);
    updater_done(u);
}

void GWUpdater_Abort(void)
{
    GWUpdaterCtx *u = sUp;

    if (u == NULL || u->state != kUpWorking) return;
    updater_done(u);
}

void GWUpdater_Poll(void)
{
    GWUpdaterCtx *u = sUp;

    if (u == NULL || u->state != kUpWorking) return;

    if (GWNet_Ticks() - u->up.startTicks > GW_UPDATE_TIMEOUT) {
        updater_fail(u, "timed out");
        return;
    }

    GWStream_Pump(&u->up);

    switch (u->step) {
    case kStConnect:
        if (u->up.state == kGWStreamReady) {
            u->step = kStSend;
        } else if (u->up.state == kGWStreamError ||
                   u->up.state == kGWStreamClosed) {
            updater_fail(u, "the connection failed");
        }
        break;

    case kStSend:
        while (u->reqSent < u->reqLen) {
            long n = GWStream_Write(&u->up, u->req + u->reqSent,
                                    u->reqLen - u->reqSent);
            if (n < 0) {
                updater_fail(u, "the connection closed before taking the "
                             "request");
                return;
            }
            if (n == 0) return;
            u->reqSent += (size_t)n;
        }
        u->step = kStRecv;
        break;

    case kStRecv: {
        long n;

        if (u->respLen >= (size_t)GW_UPDATE_BUF - 1) {
            updater_fail(u, "the response headers were too large");
            return;
        }
        n = GWStream_Read(&u->up, u->resp + u->respLen,
                          (size_t)GW_UPDATE_BUF - 1 - u->respLen);
        if (n > 0) {
            GWResponse peek;

            u->respLen += (size_t)n;
            u->resp[u->respLen] = '\0';
            if (gw_http_parse_response(u->resp, u->respLen, &peek) != 0)
                updater_finish(u);       /* head complete, or malformed */
            return;
        }
        if (n == 0) return;
        /* EOF or error: Connection: close means EOF is the end of the head
         * if nothing was read yet, or the end of whatever head arrived. */
        if (u->respLen == 0) {
            updater_fail(u, "closed the connection without answering");
            return;
        }
        updater_finish(u);
        break;
    }

    default:
        break;
    }
}
