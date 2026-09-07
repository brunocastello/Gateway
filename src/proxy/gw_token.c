#include "gw_token.h"

#include <string.h>
#include <stdio.h>

#include "../gw_config.h"
#include "../portable/gw_http.h"
#include "../portable/gw_log.h"
#include "../portable/gw_mailcmd.h"   /* GW_MAX_TOKEN */
#include "../portable/gw_oauth.h"

#define GW_TOKEN_BUF   8192L
#define GW_TOKEN_MAX   4096
#define GW_TOKEN_SLACK (60 * 60)            /* renew a minute early (ticks) */

typedef enum {
    kStIdle = 0,
    kStConnect,
    kStSend,
    kStRecv,
    kStDone
} GWTokenStep;

typedef struct {
    GWTokenState  state;
    GWTokenStep   step;
    GWStream      up;
    char         *req;
    size_t        reqLen, reqSent;
    char         *resp;
    size_t        respLen;
    char          host[GW_NET_HOST_MAX];    /* must outlive the async lookup */
    char          token[GW_TOKEN_MAX];
    unsigned long expiresAt;                /* ticks */
    char          error[128];
} GWTokenCtx;

static GWTokenCtx *sTok;

void GWToken_Init(void)
{
    if (sTok != NULL) return;
    sTok = (GWTokenCtx *)NewPtrClear((Size)sizeof(GWTokenCtx));
}

void GWToken_Shutdown(void)
{
    if (sTok == NULL) return;
    GWStream_Destroy(&sTok->up);
    if (sTok->req)  DisposePtr((Ptr)sTok->req);
    if (sTok->resp) DisposePtr((Ptr)sTok->resp);
    DisposePtr((Ptr)sTok);
    sTok = NULL;
}

static void token_release(GWTokenCtx *t)
{
    GWStream_Destroy(&t->up);
    if (t->req)  { DisposePtr((Ptr)t->req);  t->req = NULL; }
    if (t->resp) { DisposePtr((Ptr)t->resp); t->resp = NULL; }
    t->reqLen = t->reqSent = t->respLen = 0;
    t->step = kStIdle;
}

static void token_fail(GWTokenCtx *t, const char *why)
{
    strncpy(t->error, why, sizeof(t->error) - 1);
    t->error[sizeof(t->error) - 1] = '\0';
    t->state = kGWTokenFailed;
    t->token[0] = '\0';
    gw_log("oauth: %s", why);
    token_release(t);
}

void GWToken_Request(void)
{
    GWTokenCtx *t = sTok;
    const char *host, *path, *clientId, *secret, *refresh, *scope;
    char        body[4096];
    size_t      bodyLen;

    if (t == NULL) return;
    if (t->state == kGWTokenWorking) return;
    if (t->state == kGWTokenReady && GWNet_Ticks() < t->expiresAt) return;

    host     = GWConfig_Str("oauth_host", "login.microsoftonline.com");
    path     = GWConfig_Str("oauth_path", "/common/oauth2/v2.0/token");
    clientId = GWConfig_Str("oauth_client_id", "");
    secret   = GWConfig_Str("oauth_client_secret", "");
    refresh  = GWConfig_Str("refresh_token", "");
    scope    = GWConfig_Str("oauth_scope", "");

    if (clientId[0] == '\0' || refresh[0] == '\0') {
        token_fail(t, "prefs are missing oauth_client_id or refresh_token");
        return;
    }

    bodyLen = gw_oauth_refresh_body(clientId, secret, refresh, scope,
                                    body, sizeof(body));
    if (bodyLen == 0) {
        token_fail(t, "could not build the token request body");
        return;
    }

    t->req  = NewPtr(GW_TOKEN_BUF);
    t->resp = NewPtr(GW_TOKEN_BUF);
    if (t->req == NULL || t->resp == NULL) {
        token_fail(t, "out of memory");
        return;
    }

    t->reqLen = (size_t)snprintf(t->req, GW_TOKEN_BUF,
        "POST %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: %lu\r\n"
        "Accept: application/json\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\n"
        "\r\n%s",
        path, host, (unsigned long)bodyLen, body);
    t->reqSent = 0;
    t->respLen = 0;

    /* GWConfig_Str returns a rotating buffer and the resolver reads the name
     * later, asynchronously, so hold our own copy. */
    strncpy(t->host, host, sizeof(t->host) - 1);
    t->host[sizeof(t->host) - 1] = '\0';

    if (!GWStream_ConnectTLS(&t->up, t->host, 443)) {
        token_fail(t, "could not open a TLS connection to the token endpoint");
        return;
    }

    gw_log("oauth: refreshing the access token via %s", host);
    t->state = kGWTokenWorking;
    t->step  = kStConnect;
}

static void token_finish(GWTokenCtx *t)
{
    GWResponse res;
    long       expires;

    if (gw_http_parse_response(t->resp, t->respLen, &res) != 1) {
        token_fail(t, "token endpoint sent an unparsable response");
        return;
    }
    if (res.status != 200) {
        char msg[128];
        char err[96];
        if (gw_json_string(t->resp + res.head_len, t->respLen - res.head_len,
                           "error_description", err, sizeof(err)))
            snprintf(msg, sizeof(msg), "token endpoint said %d: %s",
                     res.status, err);
        else
            snprintf(msg, sizeof(msg), "token endpoint said %d", res.status);
        token_fail(t, msg);
        return;
    }

    if (!gw_json_string(t->resp + res.head_len, t->respLen - res.head_len,
                        "access_token", t->token, sizeof(t->token))) {
        token_fail(t, "no access_token in the response");
        return;
    }

    /*
     * Providers rotate refresh tokens: the response usually carries a new one,
     * and the old one has a fixed lifetime that using it does not extend. Save
     * it, or the account quietly stops working weeks later.
     */
    {
        static char rotated[GW_MAX_TOKEN];

        if (gw_json_string(t->resp + res.head_len, t->respLen - res.head_len,
                           "refresh_token", rotated, sizeof(rotated))) {
            if (strcmp(rotated, GWConfig_Str("refresh_token", "")) != 0) {
                if (GWConfig_Set("refresh_token", rotated))
                    gw_log("oauth: saved the rotated refresh token");
            }
        }
    }

    expires = gw_json_number(t->resp + res.head_len, t->respLen - res.head_len,
                             "expires_in");
    if (expires < 60) expires = 3600;
    t->expiresAt = GWNet_Ticks() + (unsigned long)(expires * 60) - GW_TOKEN_SLACK;

    gw_log("oauth: got an access token, good for %ld s", expires);
    t->state = kGWTokenReady;
    token_release(t);
}

void GWToken_Poll(void)
{
    GWTokenCtx *t = sTok;

    if (t == NULL || t->state != kGWTokenWorking) return;

    GWStream_Pump(&t->up);

    switch (t->step) {
    case kStConnect:
        if (t->up.state == kGWStreamReady) {
            /*
             * Say so, with the version. Until this line existed there was no
             * telling a handshake that completed from one that never ran:
             * both ended at the same failure further down.
             */
            {
                unsigned long w = 0, r = 0;

                GWStream_Counters(&t->up, &w, &r);
                gw_log("oauth: connected [TLS 1.%d, handshake %lu out %lu in]",
                       GWStream_TlsVersion(&t->up) == 13 ? 3 : 2, w, r);
            }
            t->step = kStSend;
        } else if (t->up.state == kGWStreamError ||
                   t->up.state == kGWStreamClosed) {
            char why[160];
            token_fail(t, GWStream_Describe(&t->up, why, sizeof(why)));
        }
        break;

    case kStSend:
        while (t->reqSent < t->reqLen) {
            long n = GWStream_Write(&t->up, t->req + t->reqSent,
                                    t->reqLen - t->reqSent);
            if (n < 0) { token_fail(t, "TLS write failed"); return; }
            if (n == 0) return;
            t->reqSent += (size_t)n;
        }
        /*
         * How much actually left, and over how many calls. A reset that
         * arrives after the whole request went out is the peer's answer to
         * what we sent; one that arrives part-way through is the transport
         * failing underneath us, and the two need looking at in different
         * places.
         */
        gw_log("oauth: request sent, %lu bytes", (unsigned long)t->reqLen);
        t->step = kStRecv;
        break;

    case kStRecv: {
        long n;

        if (t->respLen >= (size_t)GW_TOKEN_BUF - 1) {
            token_fail(t, "token response exceeded 8K");
            return;
        }
        n = GWStream_Read(&t->up, t->resp + t->respLen,
                          (size_t)GW_TOKEN_BUF - 1 - t->respLen);
        if (n > 0) {
            t->respLen += (size_t)n;
            return;
        }
        if (n == 0) return;
        /* EOF or error: Connection: close means EOF is the end of the body. */
        if (t->respLen == 0) {
            char why[160];

            token_fail(t, GWStream_Describe(&t->up, why, sizeof(why)));
            return;
        }
        t->resp[t->respLen] = '\0';
        token_finish(t);
        break;
    }

    default:
        break;
    }
}

GWTokenState GWToken_State(void)
{
    if (sTok == NULL) return kGWTokenIdle;
    if (sTok->state == kGWTokenReady && GWNet_Ticks() >= sTok->expiresAt)
        return kGWTokenIdle;                /* expired: caller should re-request */
    return sTok->state;
}

const char *GWToken_Access(void)
{
    if (sTok == NULL || sTok->state != kGWTokenReady) return NULL;
    return sTok->token;
}

const char *GWToken_Error(void)
{
    if (sTok == NULL || sTok->error[0] == '\0') return "no error";
    return sTok->error;
}
