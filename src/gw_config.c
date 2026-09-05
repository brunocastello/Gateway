/*
 * gw_config.c - load "Gateway Prefs" from the Preferences folder.
 *
 * Compiled with the Universal Interfaces on the include path along with the
 * rest of the Toolbox-facing code. Nothing here touches Open Transport, but
 * keeping it in that group means only one include-path story to reason about.
 */

#include "gw_config.h"

#include <Files.h>
#include <Folders.h>
#include <MacTypes.h>
#include <string.h>

#include "portable/gw_prefs.h"
#include "portable/gw_log.h"

#define GW_PREFS_MAX 8192

static char   sText[GW_PREFS_MAX];
static long   sLen;
static int    sLoaded;
static char   sSource[64];

static const unsigned char kPrefsName[] = "\pGateway Prefs";

void GWConfig_Load(void)
{
    short  vRefNum;
    long   dirID;
    FSSpec spec;
    short  refNum;
    OSErr  err;
    long   count;

    sLen = 0;
    sLoaded = 0;
    sText[0] = '\0';
    strcpy(sSource, "defaults (no prefs file)");

    err = FindFolder(kOnSystemDisk, kPreferencesFolderType,
                     kDontCreateFolder, &vRefNum, &dirID);
    if (err != noErr) return;

    err = FSMakeFSSpec(vRefNum, dirID, kPrefsName, &spec);
    if (err != noErr) return;

    err = FSpOpenDF(&spec, fsRdPerm, &refNum);
    if (err != noErr) return;

    count = GW_PREFS_MAX - 1;
    err = FSRead(refNum, &count, sText);
    FSClose(refNum);

    if (err != noErr && err != eofErr) return;

    sLen = count;
    sText[sLen] = '\0';
    sLoaded = 1;
    strcpy(sSource, "Preferences:Gateway Prefs");
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
 * Callers routinely hold two or three settings at once (host, user, token),
 * so hand out a small rotation of buffers rather than one shared slot.
 */
#define GW_CFG_SLOTS 6

const char *GWConfig_Str(const char *key, const char *def)
{
    static char sValue[GW_CFG_SLOTS][512];
    static int  sSlot;
    char *slot;

    if (!sLoaded) return def;

    slot = sValue[sSlot];
    sSlot = (sSlot + 1) % GW_CFG_SLOTS;

    if (gw_prefs_get(sText, (size_t)sLen, key, slot, 512)) return slot;
    return def;
}

long GWConfig_Num(const char *key, long def)
{
    if (!sLoaded) return def;
    return gw_prefs_get_num(sText, (size_t)sLen, key, def);
}

const char *GWConfig_Source(void)
{
    return sSource;
}
