/*
 * gw_token.h - OAuth 2 access-token refresh over Certainly.
 *
 * Platform independent (gw_transport.h names no operating system). Gateway holds one access token at a time, shared by
 * the IMAP and SMTP splices. Consent happens out of band: the refresh token is
 * read from the prefs file, and all Gateway ever does is trade it for a
 * short-lived access token at the provider's token endpoint.
 */
#ifndef GW_TOKEN_H
#define GW_TOKEN_H

#include "../net/gw_transport.h"

typedef enum {
    kGWTokenIdle = 0,
    kGWTokenWorking,
    kGWTokenReady,
    kGWTokenFailed
} GWTokenState;

void         GWToken_Init(void);
void         GWToken_Shutdown(void);

/* Start a refresh unless a live token is already held or one is in flight. */
void         GWToken_Request(void);

void         GWToken_Poll(void);
GWTokenState GWToken_State(void);

/* The bearer token, or NULL unless the state is kGWTokenReady. */
const char  *GWToken_Access(void);
const char  *GWToken_Error(void);

#endif /* GW_TOKEN_H */
