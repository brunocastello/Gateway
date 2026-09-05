/*
 * gw_mail.c - Module 2.
 *
 * Each session is line-oriented until both hops are authenticated, then it
 * becomes a plain byte splice. Gateway only ever inserts itself into the login
 * exchange: the client authenticates against the local password from prefs,
 * Gateway authenticates upstream with XOAUTH2, and everything after that
 * (SELECT, FETCH, DATA, ...) passes through untouched.
 */

#include "gw_mail.h"

#include <Memory.h>
#include <string.h>
#include <stdio.h>

#include "gw_token.h"
#include "../gw_config.h"
#include "../portable/gw_b64.h"
#include "../portable/gw_log.h"
#include "../portable/gw_mailcmd.h"
#include "../portable/gw_util.h"

#define GW_MAIL_BUF     4096L
#define GW_MAIL_LINE    2048
#define GW_MAIL_IDLE    (600 * 60)          /* ticks: ten minutes */

typedef enum { kMailNone = 0, kMailImap, kMailPop, kMailSmtp } GWMailKind;

typedef enum {
    kMSFree = 0,
    kMSGreet,
    kMSCommand,
    kMSToken,
    kMSUpConnect,
    kMSUpGreet,
    kMSUpEhlo,
    kMSUpStartTLS,      /* sent STARTTLS, waiting for the server's 220   */
    kMSUpHandshake,     /* Certainly is negotiating on the same socket   */
    kMSUpAuth,
    kMSSplice,
    kMSFlushClose,
    kMSDone
} GWMailState;

typedef struct {
    GWMailKind    kind;
    GWMailState   state;
    long          id;

    GWStream      cli, up;

    char         *cbuf;  size_t cLen;       /* from client   */
    char         *ubuf;  size_t uLen;       /* from upstream */
    char         *oq;    size_t oLen, oSent;/* to client     */
    char         *pq;    size_t pLen, pSent;/* to upstream   */

    char          tag[GW_MAX_TAG];          /* IMAP tag owed a reply */
    char          user[GW_MAX_USER];
    int           loginPhase;               /* SMTP AUTH LOGIN sub-state */
    int           wantStartTLS;             /* upstream needs a STARTTLS upgrade */
    int           tlsUp;                    /* the upgrade has happened  */
    char          upHost[GW_NET_HOST_MAX];  /* kept for SNI on the upgrade */
    unsigned long lastActivity;
} GWMailSession;

static GWMailSession *sMail;
static long           sNextId;

/* ------------------------------------------------------------------ */
/* Buffer plumbing                                                     */
/* ------------------------------------------------------------------ */

static int q_push(char *q, size_t cap, size_t *len, size_t *sent,
                  const void *data, size_t n)
{
    if (*sent > 0) {
        memmove(q, q + *sent, *len - *sent);
        *len -= *sent;
        *sent = 0;
    }
    if (*len + n > cap) return 0;
    memcpy(q + *len, data, n);
    *len += n;
    return 1;
}

/* 1: drained, 0: partially written, -1: the peer is gone. */
static int q_flush(GWStream *s, char *q, size_t *len, size_t *sent)
{
    while (*sent < *len) {
        long n = GWStream_Write(s, q + *sent, *len - *sent);
        if (n < 0) return -1;
        if (n == 0) return 0;
        *sent += (size_t)n;
    }
    *len = 0;
    *sent = 0;
    return 1;
}

static int say_client(GWMailSession *s, const char *text)
{
    return q_push(s->oq, (size_t)GW_MAIL_BUF, &s->oLen, &s->oSent,
                  text, strlen(text));
}

static int say_up(GWMailSession *s, const char *text)
{
    return q_push(s->pq, (size_t)GW_MAIL_BUF, &s->pLen, &s->pSent,
                  text, strlen(text));
}

/* Pull the first complete CRLF-terminated line out of buf. Returns 1 when one
 * was taken; out holds the line without its terminator. */
static int take_line(char *buf, size_t *len, char *out, size_t cap)
{
    size_t i;

    for (i = 0; i < *len; i++) {
        if (buf[i] == '\n') {
            size_t line = i;
            if (line > 0 && buf[line - 1] == '\r') line--;
            gw_copy_n(out, cap, buf, line);
            memmove(buf, buf + i + 1, *len - i - 1);
            *len -= i + 1;
            return 1;
        }
    }
    return 0;
}

/* Read from a stream into a line buffer. Returns the GWStream_Read code. */
static long fill(GWStream *s, char *buf, size_t *len, size_t cap)
{
    long n;

    if (*len >= cap) return 0;
    n = GWStream_Read(s, buf + *len, cap - *len);
    if (n > 0) *len += (size_t)n;
    return n;
}

/*
 * Phrase a failure the way the client's protocol expects it: IMAP wants an
 * untagged BYE, POP3 wants -ERR, SMTP wants a 4xx line.
 */
static const char *mail_error_text(GWMailSession *s, const char *what)
{
    static char sText[256];

    switch (s->kind) {
    case kMailImap:
        snprintf(sText, sizeof(sText), "* BYE %s\r\n", what);
        break;
    case kMailPop:
        snprintf(sText, sizeof(sText), "-ERR %s\r\n", what);
        break;
    default:
        snprintf(sText, sizeof(sText), "421 4.4.1 %s\r\n", what);
        break;
    }
    return sText;
}

static void mail_fail(GWMailSession *s, const char *clientText,
                      const char *reason)
{
    gw_log("mail #%ld %s", s->id, reason);
    say_client(s, clientText);
    GWStream_Destroy(&s->up);
    s->state = kMSFlushClose;
}

/* ------------------------------------------------------------------ */
/* Local authentication                                                */
/* ------------------------------------------------------------------ */

/*
 * Returns 1 on a match, 0 on a mismatch, -1 when prefs carry no password at
 * all. The three-way answer exists so the log can tell "you typed the wrong
 * password" apart from "Gateway never read your prefs file", which look
 * identical to the mail client and are the two things that actually go wrong.
 */
static int local_password_check(const char *pass)
{
    const char *want = GWConfig_Str("local_password", "");

    if (want[0] == '\0') return -1;
    return strcmp(want, pass) == 0 ? 1 : 0;
}

static void log_auth_failure(long id, const char *proto, int verdict)
{
    if (verdict < 0)
        gw_log("mail #%ld %s refused: prefs have no local_password", id, proto);
    else
        gw_log("mail #%ld %s refused: password did not match prefs", id, proto);
}

static void begin_upstream(GWMailSession *s)
{
    GWToken_Request();
    s->state = kMSToken;
}

/* ------------------------------------------------------------------ */
/* IMAP                                                                */
/* ------------------------------------------------------------------ */

static void imap_command(GWMailSession *s, const char *line, size_t len)
{
    GWImapCmd cmd;
    char      reply[512];

    if (!gw_imap_parse(line, len, &cmd)) {
        say_client(s, "* BAD Gateway could not parse that\r\n");
        return;
    }

    if (gw_stricmp(cmd.cmd, "CAPABILITY") == 0) {
        snprintf(reply, sizeof(reply),
                 "* CAPABILITY IMAP4rev1\r\n%s OK CAPABILITY completed\r\n",
                 cmd.tag);
        say_client(s, reply);
        return;
    }
    if (gw_stricmp(cmd.cmd, "NOOP") == 0) {
        snprintf(reply, sizeof(reply), "%s OK NOOP completed\r\n", cmd.tag);
        say_client(s, reply);
        return;
    }
    if (gw_stricmp(cmd.cmd, "LOGOUT") == 0) {
        snprintf(reply, sizeof(reply),
                 "* BYE Gateway signing off\r\n%s OK LOGOUT completed\r\n",
                 cmd.tag);
        say_client(s, reply);
        s->state = kMSFlushClose;
        return;
    }
    if (gw_stricmp(cmd.cmd, "LOGIN") == 0) {
        int verdict = cmd.has_credentials ? local_password_check(cmd.pass) : 0;

        if (verdict != 1) {
            snprintf(reply, sizeof(reply),
                     "%s NO Gateway rejected that password\r\n", cmd.tag);
            say_client(s, reply);
            log_auth_failure(s->id, "IMAP", verdict);
            return;
        }
        strncpy(s->tag, cmd.tag, sizeof(s->tag) - 1);
        strncpy(s->user, cmd.user, sizeof(s->user) - 1);
        gw_log("mail #%ld IMAP login accepted for %s", s->id, cmd.user);
        begin_upstream(s);
        return;
    }

    snprintf(reply, sizeof(reply), "%s BAD Log in first\r\n", cmd.tag);
    say_client(s, reply);
}

/* ------------------------------------------------------------------ */
/* POP3                                                                */
/* ------------------------------------------------------------------ */

static void pop_command(GWMailSession *s, const char *line, size_t len)
{
    char verb[32], arg[512];

    if (!gw_pop_parse(line, len, verb, sizeof(verb), arg, sizeof(arg))) {
        say_client(s, "-ERR Gateway could not parse that\r\n");
        return;
    }

    if (gw_stricmp(verb, "USER") == 0) {
        strncpy(s->user, arg, sizeof(s->user) - 1);
        say_client(s, "+OK\r\n");
        return;
    }

    if (gw_stricmp(verb, "PASS") == 0) {
        int verdict = local_password_check(arg);

        if (verdict != 1) {
            say_client(s, "-ERR Gateway rejected that password\r\n");
            log_auth_failure(s->id, "POP", verdict);
            return;
        }
        gw_log("mail #%ld POP login accepted for %s", s->id, s->user);
        begin_upstream(s);
        return;
    }

    if (gw_stricmp(verb, "CAPA") == 0) {
        /* Deliberately minimal: Gateway does the real authentication with
         * the upstream server, so the client only ever needs USER/PASS. */
        say_client(s, "+OK Capability list follows\r\nUSER\r\n.\r\n");
        return;
    }
    if (gw_stricmp(verb, "NOOP") == 0) { say_client(s, "+OK\r\n"); return; }
    if (gw_stricmp(verb, "QUIT") == 0) {
        say_client(s, "+OK Gateway signing off\r\n");
        s->state = kMSFlushClose;
        return;
    }
    if (gw_stricmp(verb, "APOP") == 0) {
        /* APOP hashes a server-issued timestamp, which Gateway never
         * issued, so there is nothing to verify against. */
        say_client(s, "-ERR APOP is not supported; use USER and PASS\r\n");
        return;
    }

    say_client(s, "-ERR Log in first\r\n");
}

/* ------------------------------------------------------------------ */
/* SMTP                                                                */
/* ------------------------------------------------------------------ */

static void smtp_auth_success(GWMailSession *s)
{
    gw_log("mail #%ld SMTP login accepted", s->id);
    begin_upstream(s);
}

static void smtp_command(GWMailSession *s, const char *line, size_t len)
{
    char verb[32], arg[1024];
    char pass[GW_MAX_PASS];

    /* Mid-AUTH continuations are payload, not commands. */
    if (s->loginPhase == 1) {               /* expecting the base64 user name */
        char user[GW_MAX_USER];
        size_t n = gw_b64_decode(line, len, user, sizeof(user) - 1);
        if (n == (size_t)-1) {
            s->loginPhase = 0;
            say_client(s, "501 5.5.2 Cannot decode that\r\n");
            return;
        }
        user[n] = '\0';
        strncpy(s->user, user, sizeof(s->user) - 1);
        s->loginPhase = 2;
        say_client(s, "334 UGFzc3dvcmQ6\r\n");     /* "Password:" */
        return;
    }
    if (s->loginPhase == 2) {               /* expecting the base64 password */
        size_t n = gw_b64_decode(line, len, pass, sizeof(pass) - 1);
        s->loginPhase = 0;
        if (n == (size_t)-1) {
            say_client(s, "501 5.5.2 Cannot decode that\r\n");
            return;
        }
        pass[n] = '\0';
        {
            int verdict = local_password_check(pass);
            if (verdict != 1) {
                say_client(s, "535 5.7.8 Gateway rejected that password\r\n");
                log_auth_failure(s->id, "SMTP", verdict);
                return;
            }
        }
        smtp_auth_success(s);
        return;
    }
    if (s->loginPhase == 3) {               /* expecting the SASL PLAIN blob */
        s->loginPhase = 0;
        {
            int verdict = gw_sasl_plain_decode(line, len, s->user,
                                               sizeof(s->user),
                                               pass, sizeof(pass))
                              ? local_password_check(pass) : 0;
            if (verdict != 1) {
                say_client(s, "535 5.7.8 Gateway rejected that password\r\n");
                log_auth_failure(s->id, "SMTP", verdict);
                return;
            }
        }
        smtp_auth_success(s);
        return;
    }

    if (!gw_smtp_parse(line, len, verb, sizeof(verb), arg, sizeof(arg))) {
        say_client(s, "500 5.5.2 Gateway could not parse that\r\n");
        return;
    }

    if (gw_stricmp(verb, "EHLO") == 0) {
        say_client(s,
                   "250-Gateway\r\n"
                   "250-AUTH PLAIN LOGIN\r\n"
                   "250-8BITMIME\r\n"
                   "250 SIZE 35882577\r\n");
        return;
    }
    if (gw_stricmp(verb, "HELO") == 0) { say_client(s, "250 Gateway\r\n"); return; }
    if (gw_stricmp(verb, "NOOP") == 0) { say_client(s, "250 2.0.0 OK\r\n");  return; }
    if (gw_stricmp(verb, "RSET") == 0) { say_client(s, "250 2.0.0 OK\r\n");  return; }
    if (gw_stricmp(verb, "QUIT") == 0) {
        say_client(s, "221 2.0.0 Bye\r\n");
        s->state = kMSFlushClose;
        return;
    }

    if (gw_stricmp(verb, "AUTH") == 0) {
        if (gw_strnicmp(arg, "LOGIN", 5) == 0) {
            s->loginPhase = 1;
            say_client(s, "334 VXNlcm5hbWU6\r\n");  /* "Username:" */
            return;
        }
        if (gw_strnicmp(arg, "PLAIN", 5) == 0) {
            const char *payload = arg + 5;
            while (*payload == ' ') payload++;
            if (*payload == '\0') {
                s->loginPhase = 3;
                say_client(s, "334 \r\n");
                return;
            }
            {
                int verdict = gw_sasl_plain_decode(payload, strlen(payload),
                                                   s->user, sizeof(s->user),
                                                   pass, sizeof(pass))
                                  ? local_password_check(pass) : 0;
                if (verdict != 1) {
                    say_client(s,
                               "535 5.7.8 Gateway rejected that password\r\n");
                    log_auth_failure(s->id, "SMTP", verdict);
                    return;
                }
            }
            smtp_auth_success(s);
            return;
        }
        say_client(s, "504 5.5.4 Gateway offers PLAIN and LOGIN only\r\n");
        return;
    }

    say_client(s, "530 5.7.0 Authentication required\r\n");
}

/* ------------------------------------------------------------------ */
/* Upstream login                                                      */
/* ------------------------------------------------------------------ */

static void send_xoauth2(GWMailSession *s)
{
    char blob[3072];
    char line[3200];
    const char *user = GWConfig_Str("oauth_user", s->user);
    const char *token = GWToken_Access();

    if (token == NULL ||
        gw_sasl_xoauth2(user, token, blob, sizeof(blob)) == 0) {
        mail_fail(s, mail_error_text(s, "Gateway could not build an XOAUTH2 token"),
                  "XOAUTH2 blob would not fit");
        return;
    }

    if (s->kind == kMailImap)
        snprintf(line, sizeof(line), "GW1 AUTHENTICATE XOAUTH2 %s\r\n", blob);
    else
        snprintf(line, sizeof(line), "AUTH XOAUTH2 %s\r\n", blob);   /* POP and SMTP */

    say_up(s, line);
    s->state = kMSUpAuth;
}

static void step_up_greet(GWMailSession *s)
{
    char line[GW_MAIL_LINE];

    if (!take_line(s->ubuf, &s->uLen, line, sizeof(line))) return;

    if (s->kind == kMailImap) {
        if (gw_strnicmp(line, "* OK", 4) != 0) {
            mail_fail(s, "* BYE Upstream IMAP server refused the session\r\n",
                      "upstream IMAP greeting was not OK");
            return;
        }
        send_xoauth2(s);
        return;
    }

    if (s->kind == kMailPop) {
        if (line[0] != '+') {
            mail_fail(s, "-ERR Upstream POP server refused the session\r\n",
                      "upstream POP greeting was not +OK");
            return;
        }
        send_xoauth2(s);
        return;
    }

    if (line[0] != '2') {
        mail_fail(s, "421 4.4.1 Upstream SMTP server refused the session\r\n",
                  "upstream SMTP greeting was not 2xx");
        return;
    }
    if (line[3] == '-') return;             /* multi-line banner, keep reading */

    say_up(s, "EHLO gateway\r\n");
    s->state = kMSUpEhlo;
}

static void step_up_ehlo(GWMailSession *s)
{
    char line[GW_MAIL_LINE];

    while (take_line(s->ubuf, &s->uLen, line, sizeof(line))) {
        if (line[0] != '2') {
            mail_fail(s, "421 4.4.1 Upstream SMTP server rejected EHLO\r\n",
                      "upstream EHLO failed");
            return;
        }
        if (line[3] != '-') {               /* final line of the 250 block */
            if (s->wantStartTLS && !s->tlsUp) {
                say_up(s, "STARTTLS\r\n");
                s->state = kMSUpStartTLS;
            } else {
                send_xoauth2(s);
            }
            return;
        }
    }
}

/*
 * The server has answered STARTTLS. Everything after its reply is TLS, so the
 * reply has to have been read in full and nothing may be left buffered before
 * Certainly takes the socket over.
 */
static void step_up_starttls(GWMailSession *s)
{
    char line[GW_MAIL_LINE];

    if (!take_line(s->ubuf, &s->uLen, line, sizeof(line))) return;

    if (line[0] != '2') {
        mail_fail(s, "421 4.4.1 Upstream refused STARTTLS\r\n",
                  "upstream refused STARTTLS");
        return;
    }
    if (s->uLen != 0) {
        mail_fail(s, "421 4.4.1 Upstream sent data before TLS\r\n",
                  "upstream spoke before the TLS handshake");
        return;
    }

    if (!GWStream_UpgradeToTLS(&s->up, s->upHost)) {
        mail_fail(s, "421 4.4.1 Gateway could not start TLS\r\n",
                  "STARTTLS upgrade failed");
        return;
    }
    s->tlsUp = 1;
    gw_log("mail #%ld STARTTLS accepted, negotiating", s->id);
    s->state = kMSUpHandshake;
}

static void step_up_auth(GWMailSession *s)
{
    char line[GW_MAIL_LINE];
    char reply[512];

    while (take_line(s->ubuf, &s->uLen, line, sizeof(line))) {
        if (s->kind == kMailImap) {
            if (line[0] == '+') {
                /* An error challenge: an empty line cancels the exchange and
                 * makes the server send its tagged NO. */
                say_up(s, "\r\n");
                continue;
            }
            if (gw_strnicmp(line, "GW1 OK", 6) == 0) {
                snprintf(reply, sizeof(reply), "%s OK LOGIN completed\r\n",
                         s->tag);
                say_client(s, reply);
                gw_log("mail #%ld IMAP splice established", s->id);
                s->state = kMSSplice;
                return;
            }
            if (gw_strnicmp(line, "GW1 ", 4) == 0) {
                snprintf(reply, sizeof(reply),
                         "%s NO Upstream rejected XOAUTH2\r\n", s->tag);
                mail_fail(s, reply, "upstream rejected XOAUTH2");
                return;
            }
            continue;                       /* untagged chatter: ignore */
        }

        if (s->kind == kMailPop) {
            if (line[0] == '+' && line[1] == ' ') {
                /* Base64 error challenge: '*' cancels the exchange. */
                say_up(s, "*\r\n");
                continue;
            }
            if (gw_strnicmp(line, "+OK", 3) == 0) {
                say_client(s, "+OK Logged in\r\n");
                gw_log("mail #%ld POP splice established", s->id);
                s->state = kMSSplice;
                return;
            }
            mail_fail(s, "-ERR Upstream rejected XOAUTH2\r\n",
                      "upstream rejected XOAUTH2");
            return;
        }

        if (line[0] == '3') { say_up(s, "\r\n"); continue; }
        if (gw_strnicmp(line, "235", 3) == 0) {
            say_client(s, "235 2.7.0 Authentication successful\r\n");
            gw_log("mail #%ld SMTP splice established", s->id);
            s->state = kMSSplice;
            return;
        }
        if (line[0] == '4' || line[0] == '5') {
            mail_fail(s, "535 5.7.8 Upstream rejected XOAUTH2\r\n",
                      "upstream rejected XOAUTH2");
            return;
        }
    }
}

/* ------------------------------------------------------------------ */

static void step_splice(GWMailSession *s)
{
    long n;

    /* client -> upstream */
    if (s->pLen == s->pSent) {
        if (s->cLen > 0) {
            q_push(s->pq, (size_t)GW_MAIL_BUF, &s->pLen, &s->pSent,
                   s->cbuf, s->cLen);
            s->cLen = 0;
            s->lastActivity = GWNet_Ticks();
        } else {
            n = fill(&s->cli, s->cbuf, &s->cLen, (size_t)GW_MAIL_BUF);
            if (n > 0) {
                q_push(s->pq, (size_t)GW_MAIL_BUF, &s->pLen, &s->pSent,
                       s->cbuf, s->cLen);
                s->cLen = 0;
                s->lastActivity = GWNet_Ticks();
            } else if (n == -2) {
                GWStream_Close(&s->up);
            } else if (n == -1) {
                s->state = kMSDone;
                return;
            }
        }
    }
    if (q_flush(&s->up, s->pq, &s->pLen, &s->pSent) < 0) {
        s->state = kMSFlushClose;
        return;
    }

    /* upstream -> client */
    if (s->oLen == s->oSent) {
        if (s->uLen > 0) {
            q_push(s->oq, (size_t)GW_MAIL_BUF, &s->oLen, &s->oSent,
                   s->ubuf, s->uLen);
            s->uLen = 0;
            s->lastActivity = GWNet_Ticks();
        } else {
            n = fill(&s->up, s->ubuf, &s->uLen, (size_t)GW_MAIL_BUF);
            if (n > 0) {
                q_push(s->oq, (size_t)GW_MAIL_BUF, &s->oLen, &s->oSent,
                       s->ubuf, s->uLen);
                s->uLen = 0;
                s->lastActivity = GWNet_Ticks();
            } else if (n == -2 || n == -1) {
                s->state = kMSFlushClose;
            }
        }
    }
    if (q_flush(&s->cli, s->oq, &s->oLen, &s->oSent) < 0) s->state = kMSDone;
}

static void session_reset(GWMailSession *s)
{
    GWStream_Destroy(&s->cli);
    GWStream_Destroy(&s->up);
    if (s->cbuf) DisposePtr((Ptr)s->cbuf);
    if (s->ubuf) DisposePtr((Ptr)s->ubuf);
    if (s->oq)   DisposePtr((Ptr)s->oq);
    if (s->pq)   DisposePtr((Ptr)s->pq);
    memset(s, 0, sizeof(*s));
    s->state = kMSFree;
}

static void session_step(GWMailSession *s)
{
    long n;

    if (s->state == kMSFree) return;

    if (GWNet_Ticks() - s->lastActivity > GW_MAIL_IDLE) {
        gw_log("mail #%ld idle timeout", s->id);
        s->state = kMSDone;
    }

    GWStream_Pump(&s->cli);
    if (s->up.state != kGWStreamIdle) GWStream_Pump(&s->up);
    if (s->cli.state == kGWStreamError) s->state = kMSDone;

    switch (s->state) {
    case kMSGreet:
        switch (s->kind) {
        case kMailImap:
            say_client(s, "* OK [CAPABILITY IMAP4rev1] Gateway ready\r\n");
            break;
        case kMailPop:
            say_client(s, "+OK Gateway ready\r\n");
            break;
        default:
            say_client(s, "220 Gateway ESMTP ready\r\n");
            break;
        }
        s->state = kMSCommand;
        break;

    case kMSCommand: {
        char line[GW_MAIL_LINE];

        if (q_flush(&s->cli, s->oq, &s->oLen, &s->oSent) < 0) {
            s->state = kMSDone;
            break;
        }
        n = fill(&s->cli, s->cbuf, &s->cLen, (size_t)GW_MAIL_BUF);
        if (n == -1) { s->state = kMSDone; break; }
        if (n > 0) s->lastActivity = GWNet_Ticks();

        while (s->state == kMSCommand &&
               take_line(s->cbuf, &s->cLen, line, sizeof(line))) {
            if (s->kind == kMailImap)     imap_command(s, line, strlen(line));
            else if (s->kind == kMailPop) pop_command(s, line, strlen(line));
            else                          smtp_command(s, line, strlen(line));
        }
        if (n == -2 && s->cLen == 0 && s->state == kMSCommand)
            s->state = kMSFlushClose;
        break;
    }

    case kMSToken:
        switch (GWToken_State()) {
        case kGWTokenReady: {
            const char *host;
            long        port;

            if (s->kind == kMailImap) {
                host = GWConfig_Str("imap_host", "outlook.office365.com");
                port = GWConfig_Num("imap_upstream_port", 993);
                s->wantStartTLS = 0;        /* 993 is implicit TLS */
            } else if (s->kind == kMailPop) {
                host = GWConfig_Str("pop_host", "outlook.office365.com");
                port = GWConfig_Num("pop_upstream_port", 995);
                s->wantStartTLS = 0;        /* 995 is implicit TLS */
            } else {
                host = GWConfig_Str("smtp_host", "smtp-mail.outlook.com");
                port = GWConfig_Num("smtp_upstream_port", 587);
                /* 587 is plaintext until STARTTLS; 465 is TLS from the
                 * first byte. Follow the port unless prefs say otherwise. */
                s->wantStartTLS = (int)GWConfig_Num("smtp_starttls",
                                                    port == 465 ? 0 : 1);
            }
            /* Copy before connecting: GWConfig_Str hands back a rotating
             * buffer, and the resolver reads the name asynchronously. */
            strncpy(s->upHost, host, sizeof(s->upHost) - 1);
            host = s->upHost;
            gw_log("mail #%ld connecting to %s:%ld%s", s->id, host, port,
                   s->wantStartTLS ? " (STARTTLS)" : " (TLS)");

            if (!(s->wantStartTLS
                      ? GWStream_ConnectPlain(&s->up, host, (UInt16)port)
                      : GWStream_ConnectTLS(&s->up, host, (UInt16)port))) {
                mail_fail(s,
                          mail_error_text(s, "Gateway could not reach the mail server"),
                          "upstream TLS connect failed to start");
                break;
            }
            s->state = kMSUpConnect;
            break;
        }
        case kGWTokenFailed:
            mail_fail(s,
                      mail_error_text(s, "Gateway could not refresh its access token"),
                      GWToken_Error());
            break;
        case kGWTokenIdle:
            GWToken_Request();
            break;
        default:
            break;
        }
        break;

    case kMSUpConnect:
        if (s->up.state == kGWStreamReady) {
            s->state = kMSUpGreet;
        } else if (s->up.state == kGWStreamError ||
                   s->up.state == kGWStreamClosed) {
            mail_fail(s,
                      mail_error_text(s, "Gateway could not reach the mail server"),
                      GWStream_ErrorText(&s->up));
        }
        break;

    case kMSUpHandshake:
        if (s->up.state == kGWStreamReady) {
            /* RFC 3207: the session resets, so EHLO again inside TLS. */
            say_up(s, "EHLO gateway\r\n");
            q_flush(&s->up, s->pq, &s->pLen, &s->pSent);
            s->state = kMSUpEhlo;
        } else if (s->up.state == kGWStreamError ||
                   s->up.state == kGWStreamClosed) {
            mail_fail(s, "421 4.4.1 TLS handshake with the mail server failed\r\n",
                      GWStream_ErrorText(&s->up));
        }
        break;

    case kMSUpGreet:
    case kMSUpEhlo:
    case kMSUpStartTLS:
    case kMSUpAuth:
        if (q_flush(&s->up, s->pq, &s->pLen, &s->pSent) < 0) {
            s->state = kMSFlushClose;
            break;
        }
        n = fill(&s->up, s->ubuf, &s->uLen, (size_t)GW_MAIL_BUF);
        if (n == -1 || n == -2) {
            mail_fail(s, mail_error_text(s, "Upstream closed the connection"),
                      "upstream closed during login");
            break;
        }
        if (s->state == kMSUpGreet)          step_up_greet(s);
        else if (s->state == kMSUpEhlo)      step_up_ehlo(s);
        else if (s->state == kMSUpStartTLS)  step_up_starttls(s);
        else                                 step_up_auth(s);

        q_flush(&s->up, s->pq, &s->pLen, &s->pSent);
        q_flush(&s->cli, s->oq, &s->oLen, &s->oSent);
        break;

    case kMSSplice:
        step_splice(s);
        break;

    case kMSFlushClose: {
        int r = q_flush(&s->cli, s->oq, &s->oLen, &s->oSent);
        if (r != 0) s->state = kMSDone;
        break;
    }

    default:
        break;
    }

    if (s->state == kMSDone) {
        GWStream_Close(&s->cli);
        session_reset(s);
    }
}

/* ------------------------------------------------------------------ */

void GWMail_Init(void)
{
    if (sMail != NULL) return;
    sMail = (GWMailSession *)NewPtrClear(
        (Size)(sizeof(GWMailSession) * GW_MAX_MAIL_SESSIONS));
}

void GWMail_Shutdown(void)
{
    int i;

    if (sMail == NULL) return;
    for (i = 0; i < GW_MAX_MAIL_SESSIONS; i++)
        if (sMail[i].state != kMSFree) session_reset(&sMail[i]);
    DisposePtr((Ptr)sMail);
    sMail = NULL;
}

static int mail_accept(GWConn *c, GWMailKind kind)
{
    int i;

    if (sMail == NULL) return 0;

    for (i = 0; i < GW_MAX_MAIL_SESSIONS; i++) {
        GWMailSession *s = &sMail[i];
        if (s->state != kMSFree) continue;

        memset(s, 0, sizeof(*s));
        s->cbuf = NewPtr(GW_MAIL_BUF);
        s->ubuf = NewPtr(GW_MAIL_BUF);
        s->oq   = NewPtr(GW_MAIL_BUF);
        s->pq   = NewPtr(GW_MAIL_BUF);
        if (s->cbuf == NULL || s->ubuf == NULL ||
            s->oq == NULL || s->pq == NULL) {
            session_reset(s);
            gw_log("out of memory accepting a mail connection");
            return 0;
        }
        GWStream_Adopt(&s->cli, c);
        s->kind = kind;
        s->id = ++sNextId;
        s->state = kMSGreet;
        s->lastActivity = GWNet_Ticks();
        return 1;
    }
    return 0;
}

int GWMail_AcceptImap(GWConn *c) { return mail_accept(c, kMailImap); }
int GWMail_AcceptPop(GWConn *c)  { return mail_accept(c, kMailPop);  }
int GWMail_AcceptSmtp(GWConn *c) { return mail_accept(c, kMailSmtp); }

void GWMail_Poll(void)
{
    int i;

    if (sMail == NULL) return;
    for (i = 0; i < GW_MAX_MAIL_SESSIONS; i++) session_step(&sMail[i]);
}

int GWMail_ActiveCount(void)
{
    int i, n = 0;

    if (sMail == NULL) return 0;
    for (i = 0; i < GW_MAX_MAIL_SESSIONS; i++)
        if (sMail[i].state != kMSFree) n++;
    return n;
}
