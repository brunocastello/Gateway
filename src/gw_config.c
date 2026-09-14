/*
 * gw_config.c - load "Gateway Prefs" from the Preferences folder.
 *
 * Compiled with the Universal Interfaces on the include path along with the
 * rest of the Toolbox-facing code. Nothing here touches Open Transport, but
 * keeping it in that group means only one include-path story to reason about.
 */

#include "gw_config.h"

#include "gw_plat.h"

#include <string.h>

#include "portable/gw_prefs.h"
#include "portable/gw_util.h"
#include "portable/gw_log.h"

/*
 * Generous on purpose. An 8 KB buffer was silently truncating a 9.7 KB prefs
 * file mid-way through the refresh token, which the provider then rejected as
 * malformed -- a failure that looked exactly like a revoked credential.
 */
#define GW_PREFS_MAX 32768

static char   sText[GW_PREFS_MAX];
static long   sLen;
static int    sLoaded;


void GWConfig_Load(void)
{
    long count;

    sLen = 0;
    sLoaded = 0;
    sText[0] = '\0';

    count = GWPlat_ReadPrefs(sText, GW_PREFS_MAX - 1);
    if (count < 0) return;

    sLen = count;
    sText[sLen] = '\0';
    sLoaded = 1;

    /*
     * A full buffer means the file was cut off, and every setting past the cut
     * silently reverts to its default. Say so: this went unnoticed once and
     * cost an evening.
     */
    if (sLen >= GW_PREFS_MAX - 1)
        gw_log("WARNING: prefs file is larger than %d bytes and was TRUNCATED",
               (int)GW_PREFS_MAX - 1);
    else
        gw_log("read %ld bytes of prefs", sLen);

    /*
     * Say out loud whether the settings that matter actually parsed. A prefs
     * file that loads but yields nothing looks exactly like no prefs file at
     * all from the mail client's side -- every login comes back as a bad
     * password -- so the log has to distinguish the two.
     */
    if (GWConfig_Str("local_password", "")[0] == '\0')
        gw_log("WARNING: no local_password in prefs; mail logins will fail");
    else
        gw_log("local_password is set; mail logins will be checked against it");
}


int GWConfig_Loaded(void)
{
    return sLoaded;
}

/*
 * Per-provider defaults.
 *
 * Every one of these can still be set explicitly in the prefs file; the table
 * only supplies what was left out. It exists because the difference between
 * one mail provider and another is six hostnames and a scope, and getting one
 * of them wrong produces a failure that looks like a bad password.
 */
typedef struct {
    const char *key;
    const char *outlook;
    const char *gmail;
} GWProviderDefault;

static const GWProviderDefault kProviderDefaults[] = {
    { "oauth_host",  "login.microsoftonline.com",  "oauth2.googleapis.com" },
    { "oauth_path",  "/common/oauth2/v2.0/token",  "/token" },
    { "oauth_scope",
      "offline_access https://outlook.office.com/IMAP.AccessAsUser.All "
      "https://outlook.office.com/POP.AccessAsUser.All "
      "https://outlook.office.com/SMTP.Send",
      "https://mail.google.com/" },
    { "imap_host",   "outlook.office365.com",      "imap.gmail.com" },
    { "pop_host",    "outlook.office365.com",      "pop.gmail.com" },
    { "smtp_host",   "smtp-mail.outlook.com",      "smtp.gmail.com" },
    { NULL, NULL, NULL }
};

/* The default for key under the configured provider, or NULL if there is
 * none. Never consults the provider setting itself, which would recurse. */
static const char *gw_provider_default(const char *key)
{
    char provider[32];
    int  gmail, i;

    if (!sLoaded) return NULL;
    if (gw_stricmp(key, "provider") == 0) return NULL;

    if (!gw_prefs_get(sText, (size_t)sLen, "provider",
                      provider, sizeof(provider)))
        provider[0] = '\0';

    /*
     * "custom" supplies nothing, so the explicit imap_host/pop_host/smtp_host
     * settings stand on their own. Anything else falls back to Outlook, which
     * is what an unset provider should do.
     */
    if (gw_stricmp(provider, "custom") == 0) return NULL;

    gmail = (gw_stricmp(provider, "gmail") == 0 ||
             gw_stricmp(provider, "google") == 0);

    for (i = 0; kProviderDefaults[i].key != NULL; i++) {
        if (gw_stricmp(kProviderDefaults[i].key, key) == 0)
            return gmail ? kProviderDefaults[i].gmail
                         : kProviderDefaults[i].outlook;
    }
    return NULL;
}

/*
 * Callers routinely hold two or three settings at once (host, user, token),
 * so hand out a small rotation of buffers rather than one shared slot.
 *
 * The slots are large because one of the values is an OAuth refresh token,
 * which runs to several hundred characters and is worthless if it arrives
 * truncated -- a silent failure that looks exactly like a rejected token.
 */
#define GW_CFG_SLOTS  6
#define GW_CFG_VALUE  2048

const char *GWConfig_Str(const char *key, const char *def)
{
    static char sValue[GW_CFG_SLOTS][GW_CFG_VALUE];
    static int  sSlot;
    char *slot;

    if (!sLoaded) return def;

    slot = sValue[sSlot];
    sSlot = (sSlot + 1) % GW_CFG_SLOTS;

    if (gw_prefs_get(sText, (size_t)sLen, key, slot, GW_CFG_VALUE))
        return slot;

    {
        const char *fallback = gw_provider_default(key);
        if (fallback != NULL) return fallback;
    }
    return def;
}

int GWConfig_Set(const char *key, const char *value)
{
    static char updated[GW_PREFS_MAX];
    size_t n;

    n = gw_prefs_set(sText, (size_t)sLen, key, value,
                     updated, sizeof(updated));
    if (n == 0) {
        gw_log("cannot save %s: prefs file would exceed %d bytes",
               key, (int)GW_PREFS_MAX);
        return 0;
    }

    if (!GWPlat_WritePrefs(updated, (long)n)) return 0;

    /* Keep the in-memory copy in step so later reads see the new value. */
    memcpy(sText, updated, n);
    sLen = (long)n;
    sText[sLen] = '\0';
    return 1;
}

int GWConfig_GetNth(const char *key, int n, char *out, size_t cap)
{
    if (cap) out[0] = '\0';
    if (!sLoaded) return 0;
    return gw_prefs_get_nth(sText, (size_t)sLen, key, n, out, cap);
}

long GWConfig_Num(const char *key, long def)
{
    if (!sLoaded) return def;
    return gw_prefs_get_num(sText, (size_t)sLen, key, def);
}

const char *GWConfig_Source(void)
{
    return GWPlat_PrefsSource();
}
