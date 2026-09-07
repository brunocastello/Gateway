/*
 * gw_mail.h - Module 2: the IMAP (:1993) and SMTP (:1587) splices.
 *
 * Platform independent (gw_transport.h names no operating system). Outlook Express 5 is configured with SSL off on
 * both ports and authentication on for SMTP; Gateway checks the password it is
 * given against its own prefs, then speaks IMAPS / SMTPS upstream with
 * AUTHENTICATE XOAUTH2 using a token refreshed by gw_token.c.
 */
#ifndef GW_MAIL_H
#define GW_MAIL_H

#include "../net/gw_transport.h"

#define GW_MAX_MAIL_SESSIONS 4

void GWMail_Init(void);
void GWMail_Shutdown(void);

/* All return 0 when no slot is free; the caller then disposes of the conn. */
int  GWMail_AcceptImap(GWConn *c);
int  GWMail_AcceptPop(GWConn *c);
int  GWMail_AcceptSmtp(GWConn *c);

void GWMail_Poll(void);
int  GWMail_ActiveCount(void);

#endif /* GW_MAIL_H */
