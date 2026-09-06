/*
 * gw_mailcmd.h - IMAP and SMTP client-command parsing for Module 2.
 *
 * PORTABLE: no Mac or Windows headers. See gw_util.h. Outlook Express 5 is the
 * client Gateway is written against: IMAP on :1993 with SSL off, SMTP on :1587
 * with SSL off and authentication on. Gateway checks the password it is given
 * against its own prefs and then talks XOAUTH2 upstream.
 */
#ifndef GW_MAILCMD_H
#define GW_MAILCMD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_MAX_TAG   32
#define GW_MAX_CMD   32
#define GW_MAX_USER  256
#define GW_MAX_PASS  256

/*
 * A Microsoft OAuth access token is a JWT and runs to a couple of thousand
 * characters; the SASL payload wrapping it has to have room for that plus the
 * user name and the fixed text around them.
 */
#define GW_MAX_TOKEN     4096
#define GW_XOAUTH2_RAW   (GW_MAX_USER + GW_MAX_TOKEN + 32)
/* base64 expands by 4/3, plus padding and the terminator. */
#define GW_XOAUTH2_B64   (((GW_XOAUTH2_RAW + 2) / 3) * 4 + 4)

typedef struct {
    char tag[GW_MAX_TAG];
    char cmd[GW_MAX_CMD];
    char user[GW_MAX_USER];
    char pass[GW_MAX_PASS];
    int  has_credentials;
} GWImapCmd;

/*
 * Parse one CRLF-terminated IMAP client line (the terminator may be omitted).
 * Always fills in tag and cmd. For LOGIN it also extracts the user name and
 * password, handling both bare atoms and quoted strings with backslash escapes.
 * Returns 1 when the tag and command were recognised, 0 otherwise.
 */
int gw_imap_parse(const char *line, size_t len, GWImapCmd *out);

/*
 * Parse one SMTP client line into a verb and its argument.
 * Returns 1 when a verb was found.
 */
int gw_smtp_parse(const char *line, size_t len,
                  char *verb, size_t verb_cap,
                  char *arg, size_t arg_cap);

/*
 * Parse one POP3 client line into a verb and its argument. Same shape as
 * gw_smtp_parse, minus the colon handling that MAIL FROM: needs.
 * Returns 1 when a verb was found.
 */
int gw_pop_parse(const char *line, size_t len,
                 char *verb, size_t verb_cap,
                 char *arg, size_t arg_cap);

/*
 * Decode a SASL PLAIN payload ("[authzid]\0authcid\0password", base64).
 * Returns 1 on success.
 */
int gw_sasl_plain_decode(const char *b64, size_t len,
                         char *user, size_t user_cap,
                         char *pass, size_t pass_cap);

/*
 * Build the base64 SASL XOAUTH2 initial response:
 *   "user=" user ^A "auth=Bearer " token ^A ^A
 * Returns the encoded length, or 0 on overflow.
 */
size_t gw_sasl_xoauth2(const char *user, const char *token,
                       char *out, size_t cap);

/*
 * Build a base64 SASL PLAIN initial response for the same shape.
 * Returns the encoded length, or 0 on overflow.
 */
size_t gw_sasl_plain_encode(const char *user, const char *pass,
                            char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* GW_MAILCMD_H */
