#include "gw_token.h"

#include <stdarg.h>
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

/*
 * Log a plain sentence with its code (docs/log-codes.md) and give up. The
 * sentence is kept for GWToken_Error() as well.
 */
static void token_fail(GWTokenCtx *t, const char *code, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(t->error, sizeof(t->error), fmt, ap);
    va_end(ap);
    t->state = kGWTokenFailed;
    t->token[0] = '\0';
    gw_logc(code, "mail token: %s", t->error);
    token_release(t);
}

/*
 * The connection to the token endpoint failed. Its own sentence and T code
 * when the stream holds an error, the caller's otherwise; the bracketed
 * detail under log_debug. Described before token_fail(), which destroys the
 * stream.
 */
static void token_fail_stream(GWTokenCtx *t, const char *code,
                              const char *fallback)
{
    char        why[GW_LOG_WIDTH];
    char        desc[192];
    const char *tcode = GWStream_Explain(&t->up, t->host, why, sizeof why);

    GWStream_Describe(&t->up, desc, sizeof desc);
    if (tcode != NULL)
        token_fail(t, tcode, "%s", why);
    else
        token_fail(t, code, fallback, t->host);
    gw_logd("%s", desc);
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
        token_fail(t, "M30", "the prefs file has no oauth_client_id or "
                   "refresh_token: run get-email-token.py");
        return;
    }

    bodyLen = gw_oauth_refresh_body(clientId, secret, refresh, scope,
                                    body, sizeof(body));
    if (bodyLen == 0) {
        token_fail(t, "M31", "the refresh request could not be built: a "
                   "prefs value is too long");
        return;
    }

    t->req  = NewPtr(GW_TOKEN_BUF);
    t->resp = NewPtr(GW_TOKEN_BUF);
    if (t->req == NULL || t->resp == NULL) {
        token_fail(t, "M32", "out of memory");
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
        token_fail(t, "M33", "Gateway could not start a connection to %s",
                   t->host);
        return;
    }

    gw_log("mail token: refreshing it at %s", host);
    t->state = kGWTokenWorking;
    t->step  = kStConnect;
}

static void token_finish(GWTokenCtx *t)
{
    GWResponse res;
    long       expires;

    if (gw_http_parse_response(t->resp, t->respLen, &res) != 1) {
        token_fail(t, "M38", "%s sent a response Gateway could not "
                   "understand", t->host);
        return;
    }
    if (res.status != 200) {
        char        err[32];
        char        desc[96];
        const char *body = t->resp + res.head_len;
        size_t      bodyLen = t->respLen - res.head_len;

        /*
         * invalid_grant is the refresh token itself refused -- expired,
         * revoked, or its password changed -- and has one remedy, so it gets
         * a sentence of its own. The provider's own description goes under
         * log_debug either way.
         */
        int         haveDesc;

        /* Both read before token_fail(), which disposes of t->resp. */
        haveDesc = gw_json_string(body, bodyLen, "error_description",
                                  desc, sizeof(desc));
        if (gw_json_string(body, bodyLen, "error", err, sizeof(err)) &&
            strcmp(err, "invalid_grant") == 0)
            token_fail(t, "M39", "%s refused the refresh token: run "
                       "get-email-token.py for a new one", t->host);
        else
            token_fail(t, "M40", "%s refused to refresh the token (HTTP %d)",
                       t->host, res.status);
        if (haveDesc)
            gw_logd("%s", desc);
        return;
    }

    if (!gw_json_string(t->resp + res.head_len, t->respLen - res.head_len,
                        "access_token", t->token, sizeof(t->token))) {
        token_fail(t, "M41", "%s answered without an access token", t->host);
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
                    gw_log("mail token: saved the new refresh token the "
                           "provider sent");
            }
        }
    }

    expires = gw_json_number(t->resp + res.head_len, t->respLen - res.head_len,
                             "expires_in");
    if (expires < 60) expires = 3600;
    t->expiresAt = GWNet_Ticks() + (unsigned long)(expires * 60) - GW_TOKEN_SLACK;

    gw_log("mail token: refreshed, good for %ld minutes", expires / 60);
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
            t->step = kStSend;
        } else if (t->up.state == kGWStreamError ||
                   t->up.state == kGWStreamClosed) {
            token_fail_stream(t, "M34", "%s closed the connection");
        }
        break;

    case kStSend:
        while (t->reqSent < t->reqLen) {
            long n = GWStream_Write(&t->up, t->req + t->reqSent,
                                    t->reqLen - t->reqSent);
            if (n < 0) {
                token_fail_stream(t, "M35", "%s closed the connection before "
                                  "taking the request");
                return;
            }
            if (n == 0) return;
            t->reqSent += (size_t)n;
        }
        t->step = kStRecv;
        break;

    case kStRecv: {
        long n;

        if (t->respLen >= (size_t)GW_TOKEN_BUF - 1) {
            token_fail(t, "M36", "%s sent a response larger than 8 KB",
                       t->host);
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
            token_fail_stream(t, "M37", "%s closed the connection without "
                              "answering");
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
