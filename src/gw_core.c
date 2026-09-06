/*
 * gw_core.c - listener ownership and the single cooperative slice.
 *
 * Compiled with Apple's Universal Interfaces on the include path because it
 * reaches Open Transport through gw_net.h. The UI never gets here directly; it
 * only sees gw_core.h.
 */

#include "gw_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gw_config.h"
#include "net/gw_net.h"
#include "portable/gw_log.h"
#include "proxy/gw_httpproxy.h"
#include "proxy/gw_mail.h"
#include "proxy/gw_token.h"

static GWListener *sHttp;
static GWListener *sImap;
static GWListener *sPop;
static GWListener *sSmtp;
static char        sStatus[128];
static int         sHttpPort, sImapPort, sPopPort, sSmtpPort;

void GW_LoadSettings(void)
{
    GWConfig_Load();
}

int GW_ShowWindowPref(void)
{
    return GWConfig_Num("show_window", 1) != 0;
}

void GW_SetShowWindowPref(int show)
{
    GWConfig_Set("show_window", show ? "1" : "0");
}

int GW_Init(void)
{
    OSStatus err;

    gw_log("Gateway starting up");

    err = GWNet_Init();
    if (err != noErr) {
        gw_log("InitOpenTransport failed (%d)", (int)err);
        GW_SetStatus("Open Transport unavailable");
        return 0;
    }

    /* Already loaded by GW_LoadSettings(), which the UI calls first so it
     * knows whether to open a window. */
    gw_log("settings: %s", GWConfig_Source());

    MacTLS_Init();
    GWProxy_Init();
    GWMail_Init();
    GWToken_Init();

    sHttpPort = (int)GWConfig_Num("http_port", 8765);
    sImapPort = (int)GWConfig_Num("imap_port", 1993);
    sPopPort  = (int)GWConfig_Num("pop_port", 1995);
    sSmtpPort = (int)GWConfig_Num("smtp_port", 1587);

    sHttp = GWListener_Open((UInt16)sHttpPort, GW_MAX_SESSIONS);
    sImap = GWListener_Open((UInt16)sImapPort, 2);
    sPop  = GWListener_Open((UInt16)sPopPort, 2);
    sSmtp = GWListener_Open((UInt16)sSmtpPort, 2);

    if (sHttp == NULL && sImap == NULL && sPop == NULL && sSmtp == NULL) {
        GW_SetStatus("no listener could be bound");
        return 0;
    }

    gw_log("provider: %s", GWConfig_Str("provider", "outlook"));
    gw_log("mail upstream: imap %s:%ld, pop %s:%ld",
           GWConfig_Str("imap_host", "outlook.office365.com"),
           GWConfig_Num("imap_upstream_port", 993),
           GWConfig_Str("pop_host", "outlook.office365.com"),
           GWConfig_Num("pop_upstream_port", 995));
    gw_log("mail upstream: smtp %s:%ld",
           GWConfig_Str("smtp_host", "smtp-mail.outlook.com"),
           GWConfig_Num("smtp_upstream_port", 587));

    GW_SetStatus("idle - proxy :%d  imap :%d  pop :%d  smtp :%d",
                 sHttpPort, sImapPort, sPopPort, sSmtpPort);
    return 1;
}

void GW_Shutdown(void)
{
    if (sHttp) { GWListener_Close(sHttp); sHttp = NULL; }
    if (sImap) { GWListener_Close(sImap); sImap = NULL; }
    if (sPop)  { GWListener_Close(sPop);  sPop  = NULL; }
    if (sSmtp) { GWListener_Close(sSmtp); sSmtp = NULL; }

    GWProxy_Shutdown();
    GWMail_Shutdown();
    GWToken_Shutdown();
    MacTLS_Shutdown();
    GWNet_Shutdown();
}

void GW_Poll(void)
{
    GWConn *c;

    if (sHttp != NULL) {
        c = GWListener_Poll(sHttp);
        if (c != NULL && !GWProxy_Accept(c)) {
            gw_log("proxy busy, dropped a connection");
            GWConn_Destroy(c);
        }
    }

    if (sImap != NULL) {
        c = GWListener_Poll(sImap);
        if (c != NULL && !GWMail_AcceptImap(c)) {
            gw_log("mail busy, dropped an IMAP connection");
            GWConn_Destroy(c);
        }
    }

    if (sPop != NULL) {
        c = GWListener_Poll(sPop);
        if (c != NULL && !GWMail_AcceptPop(c)) {
            gw_log("mail busy, dropped a POP connection");
            GWConn_Destroy(c);
        }
    }

    if (sSmtp != NULL) {
        c = GWListener_Poll(sSmtp);
        if (c != NULL && !GWMail_AcceptSmtp(c)) {
            gw_log("mail busy, dropped an SMTP connection");
            GWConn_Destroy(c);
        }
    }

    GWToken_Poll();
    GWProxy_Poll();
    GWMail_Poll();
}

int         GW_LogCount(void)        { return gw_log_count(); }
const char *GW_LogLine(int idx)      { return gw_log_line(idx); }
long        GW_LogGeneration(void)   { return gw_log_generation(); }
int         GW_HttpPort(void)        { return sHttpPort; }
int         GW_ImapPort(void)        { return sImapPort; }
int         GW_PopPort(void)         { return sPopPort; }
int         GW_SmtpPort(void)        { return sSmtpPort; }

int GW_ActiveSessions(void)
{
    return GWProxy_ActiveCount() + GWMail_ActiveCount();
}

void GW_Log(const char *fmt, ...)
{
    char    line[GW_LOG_WIDTH];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    gw_log("%s", line);
}

void GW_SetStatus(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(sStatus, sizeof(sStatus), fmt, ap);
    va_end(ap);
    sStatus[sizeof(sStatus) - 1] = '\0';
}

const char *GW_StatusLine(void)
{
    return sStatus[0] ? sStatus : "starting up";
}
