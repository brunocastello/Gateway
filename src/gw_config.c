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
#include "portable/gw_provider.h"
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
static unsigned long sSum;              /* of the text last reported */

/* FNV-1a, only ever compared against itself. */
static unsigned long prefs_sum(const char *text, long len)
{
    unsigned long h = 2166136261UL;
    long i;

    for (i = 0; i < len; i++) {
        h ^= (unsigned long)(unsigned char)text[i];
        h *= 16777619UL;
    }
    return h;
}

void GWConfig_Load(void)
{
    long count;
    long wasLen = sLoaded ? sLen : -1;
    unsigned long wasSum = sSum;
    unsigned long sum;

    sLen = 0;
    sLoaded = 0;
    sText[0] = '\0';

    count = GWPlat_ReadPrefs(sText, GW_PREFS_MAX - 1);
    if (count < 0) return;

    /*
     * Reloading is routine -- every Save in the preferences window ends with
     * one, and the core asks for another right after -- so the log says
     * something only when the file has actually changed. Three identical
     * pairs of lines after one Save said nothing the first pair had not.
     */
    sum = prefs_sum(sText, count);
    if (wasLen == count && wasSum == sum) {
        sLen = count;
        sText[sLen] = '\0';
        sLoaded = 1;
        return;
    }
    sSum = sum;

    sLen = count;
    sText[sLen] = '\0';
    sLoaded = 1;

    /*
     * A full buffer means the file was cut off, and every setting past the cut
     * silently reverts to its default. Say so: this went unnoticed once and
     * cost an evening.
     */
    if (sLen >= GW_PREFS_MAX - 1)
        gw_logc("G20", "the prefs file is larger than %d bytes; the rest of "
                "it was ignored", (int)GW_PREFS_MAX - 1);
    else
        gw_log("read %ld bytes of prefs", sLen);

    /*
     * Say out loud whether the settings that matter actually parsed. A prefs
     * file that loads but yields nothing looks exactly like no prefs file at
     * all from the mail client's side -- every login comes back as a bad
     * password -- so the log has to distinguish the two.
     */
    if (GWConfig_Str("local_password", "")[0] == '\0')
        gw_logc("G21", "the prefs file has no local_password, so mail "
                "sign-ins will fail");
    else
        gw_log("local_password is set; mail logins will be checked against it");
}


int GWConfig_Loaded(void)
{
    return sLoaded;
}

/* The provider the prefs name, or "" when they name none. */
static const char *gw_config_provider(void)
{
    static char provider[32];

    if (!gw_prefs_get(sText, (size_t)sLen, "provider",
                      provider, sizeof(provider)))
        provider[0] = '\0';
    return provider;
}

/*
 * The provider's own value for a custom-only setting, or NULL when the file
 * is to be read instead: the key is not custom-only, or provider = custom.
 * Under Outlook or Gmail the file's copy is ignored even if left uncommented,
 * so the file means what the settings window shows. See gw_provider.h.
 */
static const char *gw_config_supplied(const char *key)
{
    return gw_provider_default(gw_config_provider(), key);
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

    {
        const char *supplied = gw_config_supplied(key);
        if (supplied != NULL) return supplied;
    }
    if (gw_prefs_get(sText, (size_t)sLen, key, slot, GW_CFG_VALUE))
        return slot;
    return def;
}

/* The writers' output, shared: one 32 KB buffer is plenty of partition. */
static char sUpdated[GW_PREFS_MAX];

/* Save text produced by one of the gw_prefs writers, n == 0 meaning it did
 * not fit, and keep the in-memory copy in step so later reads see it. */
static int gw_config_commit(const char *key, const char *updated, size_t n)
{
    if (n == 0) {
        gw_logc("G22", "%s could not be saved: the prefs file would be "
                "larger than %d bytes", key, (int)GW_PREFS_MAX);
        return 0;
    }

    if (!GWPlat_WritePrefs(updated, (long)n)) return 0;

    memcpy(sText, updated, n);
    sLen = (long)n;
    sText[sLen] = '\0';
    return 1;
}

int GWConfig_Set(const char *key, const char *value)
{
    return gw_config_commit(key, sUpdated,
                            gw_prefs_set(sText, (size_t)sLen, key, value,
                                         sUpdated, sizeof(sUpdated)));
}

int GWConfig_Comment(const char *key)
{
    size_t n;

    n = gw_prefs_comment(sText, (size_t)sLen, key,
                         sUpdated, sizeof(sUpdated));
    if (n == (size_t)sLen && memcmp(sUpdated, sText, n) == 0)
        return 1;                       /* already commented or absent */
    return gw_config_commit(key, sUpdated, n);
}

int GWConfig_GetCommented(const char *key, char *out, size_t cap)
{
    if (cap) out[0] = '\0';
    if (!sLoaded) return 0;
    return gw_prefs_get_commented(sText, (size_t)sLen, key, out, cap);
}

int GWConfig_GetNth(const char *key, int n, char *out, size_t cap)
{
    if (cap) out[0] = '\0';
    if (!sLoaded) return 0;
    return gw_prefs_get_nth(sText, (size_t)sLen, key, n, out, cap);
}

/*
 * The nth entry across all occurrences of a key, splitting each value on ';'.
 *
 * Thin wrapper over gw_prefs_get_nth_split that supplies the loaded prefs
 * text. Used for wayback_live, which is stored as one ;-separated value.
 */
int GWConfig_GetNthSplit(const char *key, int n, char *out, size_t cap)
{
    if (cap) out[0] = '\0';
    if (!sLoaded) return 0;
    return gw_prefs_get_nth_split(sText, (size_t)sLen, key, n, out, cap);
}

long GWConfig_Num(const char *key, long def)
{
    const char *supplied;

    if (!sLoaded) return def;
    supplied = gw_config_supplied(key);
    if (supplied != NULL) {
        long v = gw_parse_dec(supplied, strlen(supplied));
        return v < 0 ? def : v;
    }
    return gw_prefs_get_num(sText, (size_t)sLen, key, def);
}

const char *GWConfig_Source(void)
{
    return GWPlat_PrefsSource();
}
