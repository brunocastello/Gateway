/*
 * gw_oauth.h - OAuth 2 refresh-token request bodies and response parsing.
 *
 * PORTABLE: no Mac or Windows headers. See gw_util.h.
 *
 * Consent is obtained out of band (CLAUDE.md non-goals: Gateway never runs the
 * consent flow inside Classilla). All Gateway does is exchange a refresh token
 * that was dropped into its prefs file for a short-lived access token, then use
 * that token with AUTHENTICATE XOAUTH2 upstream.
 */
#ifndef GW_OAUTH_H
#define GW_OAUTH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Percent-encode into application/x-www-form-urlencoded.
 * Returns bytes written (excluding the NUL) or 0 on overflow. */
size_t gw_form_escape(const char *src, char *out, size_t cap);

/*
 * Build the token-endpoint request body:
 *   client_id=..&refresh_token=..&grant_type=refresh_token[&scope=..]
 *   [&client_secret=..]
 * client_secret and scope may be NULL or empty. Returns bytes written, or 0.
 */
size_t gw_oauth_refresh_body(const char *client_id, const char *client_secret,
                             const char *refresh_token, const char *scope,
                             char *out, size_t cap);

/*
 * Pull a string value out of a flat JSON object. Handles \" \\ \/ \n \r \t
 * escapes and skips \uXXXX (replacing it with '?'), which is enough for the
 * token responses Gateway sees. Returns 1 on success.
 */
int gw_json_string(const char *json, size_t len, const char *key,
                   char *out, size_t cap);

/* Pull a numeric value (e.g. "expires_in"). Returns -1 when absent. */
long gw_json_number(const char *json, size_t len, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* GW_OAUTH_H */
