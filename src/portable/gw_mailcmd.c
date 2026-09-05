#include "gw_mailcmd.h"
#include "gw_b64.h"
#include "gw_util.h"

#include <string.h>

static size_t skip_space(const char *s, size_t len, size_t i)
{
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

/* Read one IMAP astring: a quoted string or a bare atom. */
static size_t read_astring(const char *s, size_t len, size_t i,
                           char *out, size_t cap)
{
    size_t o = 0;

    i = skip_space(s, len, i);
    if (i >= len) { if (cap) out[0] = '\0'; return i; }

    if (s[i] == '"') {
        i++;
        while (i < len && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < len) i++;
            if (o + 1 < cap) out[o++] = s[i];
            i++;
        }
        if (i < len) i++;               /* closing quote */
    } else {
        while (i < len && s[i] != ' ' && s[i] != '\t' &&
               s[i] != '\r' && s[i] != '\n') {
            if (o + 1 < cap) out[o++] = s[i];
            i++;
        }
    }
    if (cap) out[o] = '\0';
    return i;
}

int gw_imap_parse(const char *line, size_t len, GWImapCmd *out)
{
    size_t i = 0;

    memset(out, 0, sizeof(*out));

    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) len--;

    i = read_astring(line, len, i, out->tag, sizeof(out->tag));
    if (out->tag[0] == '\0') return 0;

    i = read_astring(line, len, i, out->cmd, sizeof(out->cmd));
    if (out->cmd[0] == '\0') return 0;

    if (gw_stricmp(out->cmd, "LOGIN") == 0) {
        i = read_astring(line, len, i, out->user, sizeof(out->user));
        i = read_astring(line, len, i, out->pass, sizeof(out->pass));
        out->has_credentials = (out->user[0] != '\0');
    }
    return 1;
}

int gw_smtp_parse(const char *line, size_t len,
                  char *verb, size_t verb_cap,
                  char *arg, size_t arg_cap)
{
    size_t i = 0, start;

    if (verb_cap) verb[0] = '\0';
    if (arg_cap) arg[0] = '\0';

    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) len--;

    i = skip_space(line, len, 0);
    start = i;
    while (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != ':') i++;
    if (i == start) return 0;
    gw_copy_n(verb, verb_cap, line + start, i - start);

    i = skip_space(line, len, i);
    if (i < len) gw_copy_n(arg, arg_cap, line + i, len - i);
    return 1;
}

int gw_pop_parse(const char *line, size_t len,
                 char *verb, size_t verb_cap,
                 char *arg, size_t arg_cap)
{
    size_t i = 0, start;

    if (verb_cap) verb[0] = '\0';
    if (arg_cap) arg[0] = '\0';

    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) len--;

    i = skip_space(line, len, 0);
    start = i;
    while (i < len && line[i] != ' ' && line[i] != '\t') i++;
    if (i == start) return 0;
    gw_copy_n(verb, verb_cap, line + start, i - start);

    i = skip_space(line, len, i);
    if (i < len) gw_copy_n(arg, arg_cap, line + i, len - i);
    return 1;
}

int gw_sasl_plain_decode(const char *b64, size_t len,
                         char *user, size_t user_cap,
                         char *pass, size_t pass_cap)
{
    char raw[GW_MAX_USER + GW_MAX_PASS + 8];
    size_t n, i, f0, f1;

    if (user_cap) user[0] = '\0';
    if (pass_cap) pass[0] = '\0';

    n = gw_b64_decode(b64, len, raw, sizeof(raw));
    if (n == (size_t)-1 || n == 0) return 0;

    /* authzid \0 authcid \0 passwd */
    for (i = 0; i < n && raw[i] != '\0'; i++)
        ;
    if (i >= n) return 0;
    f0 = i + 1;

    for (i = f0; i < n && raw[i] != '\0'; i++)
        ;
    if (i >= n) return 0;
    f1 = i;

    gw_copy_n(user, user_cap, raw + f0, f1 - f0);
    gw_copy_n(pass, pass_cap, raw + f1 + 1, n - f1 - 1);
    return user[0] != '\0';
}

size_t gw_sasl_xoauth2(const char *user, const char *token,
                       char *out, size_t cap)
{
    char raw[1536];
    size_t o = 0;
    size_t ul = strlen(user), tl = strlen(token);

    if (5 + ul + 1 + 12 + tl + 2 >= sizeof(raw)) return 0;

    memcpy(raw + o, "user=", 5);              o += 5;
    memcpy(raw + o, user, ul);                o += ul;
    raw[o++] = '\001';
    memcpy(raw + o, "auth=Bearer ", 12);      o += 12;
    memcpy(raw + o, token, tl);               o += tl;
    raw[o++] = '\001';
    raw[o++] = '\001';

    return gw_b64_encode(raw, o, out, cap);
}

size_t gw_sasl_plain_encode(const char *user, const char *pass,
                            char *out, size_t cap)
{
    char raw[GW_MAX_USER + GW_MAX_PASS + 4];
    size_t o = 0;
    size_t ul = strlen(user), pl = strlen(pass);

    if (ul + pl + 3 >= sizeof(raw)) return 0;

    raw[o++] = '\0';
    memcpy(raw + o, user, ul);  o += ul;
    raw[o++] = '\0';
    memcpy(raw + o, pass, pl);  o += pl;

    return gw_b64_encode(raw, o, out, cap);
}
