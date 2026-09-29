/*
 * gw_plat_mac.c - gw_plat.h on Mac OS 9, through the Folder and File Managers.
 *
 * This was spread through gw_config.c and gw_core.c until the Windows port
 * needed the same jobs done differently. Nothing here decides what to write;
 * it only knows where things live on this system.
 */

#include "gw_plat.h"

#include <Files.h>
#include <Folders.h>
#include <InternetConfig.h>
#include <MacTypes.h>

#include <stdio.h>
#include <string.h>

#include "portable/gw_log.h"
#include "portable/gw_util.h"

static FSSpec sSpec;
static int    sHaveSpec;
static char   sSource[64] = "defaults (no prefs file)";

static const unsigned char kPrefsName[] = "\pGateway Prefs";

long GWPlat_ReadPrefs(char *buf, size_t cap)
{
    short  vRefNum, refNum;
    long   dirID, count;
    FSSpec spec;
    OSErr  err;

    err = FindFolder(kOnSystemDisk, kPreferencesFolderType,
                     kDontCreateFolder, &vRefNum, &dirID);
    if (err != noErr) return -1;

    err = FSMakeFSSpec(vRefNum, dirID, kPrefsName, &spec);
    if (err != noErr) return -1;

    /* Kept even if the read fails: it is where a write would go. */
    sSpec = spec;
    sHaveSpec = 1;

    err = FSpOpenDF(&spec, fsRdPerm, &refNum);
    if (err != noErr) return -1;

    count = (long)cap;
    err = FSRead(refNum, &count, buf);
    FSClose(refNum);

    if (err != noErr && err != eofErr) return -1;

    strcpy(sSource, "Preferences:Gateway Prefs");
    return count;
}

const char *GWPlat_PrefsSource(void)
{
    return sSource;
}

int GWPlat_WritePrefs(const char *buf, long len)
{
    short refNum;
    OSErr err;
    long  count = len;

    if (!sHaveSpec) {
        gw_logc("G23", "settings could not be saved: there is no prefs "
                "file to write to");
        return 0;
    }

    err = FSpOpenDF(&sSpec, fsRdWrPerm, &refNum);
    if (err != noErr) {
        gw_logc("G24", "settings could not be saved: the prefs file would "
                "not open");
        gw_logd("FSpOpenDF %d", (int)err);
        return 0;
    }

    err = SetFPos(refNum, fsFromStart, 0);
    if (err == noErr) err = FSWrite(refNum, &count, buf);
    if (err == noErr) err = SetEOF(refNum, len);
    FSClose(refNum);

    if (err != noErr) {
        gw_logc("G25", "settings could not be saved: writing the prefs file "
                "failed");
        gw_logd("write %d", (int)err);
        return 0;
    }

    FlushVol(NULL, sSpec.vRefNum);
    return 1;
}

/* ------------------------------------------------------------------ */
/* The log file                                                        */
/* ------------------------------------------------------------------ */

static short sLogRef;                   /* 0 when no file is open */

int GWPlat_OpenLog(const char *want)
{
    OSErr  err;
    short  vRefNum, refNum;
    long   dirID, gwDir;
    FSSpec spec;

    (void)want;     /* the Mac build has one place for it, named below */

    /*
     * System Folder : Application Support : Gateway : Gateway Log.txt, both
     * folders created if absent. Preferences is where a setting belongs, and a
     * growing log is not a setting.
     */
    err = FindFolder(kOnSystemDisk, kApplicationSupportFolderType,
                     kCreateFolder, &vRefNum, &dirID);
    if (err != noErr) {
        gw_logc("G40", "the log file could not be opened, so the log is "
                "kept in this window only");
        gw_logd("FindFolder %d", (int)err);
        return 0;
    }

    /* dupFNErr just means someone got here first, on an earlier run. */
    err = DirCreate(vRefNum, dirID, "\pGateway", &gwDir);
    if (err != noErr && err != dupFNErr) {
        gw_logc("G40", "the log file could not be opened, so the log is "
                "kept in this window only");
        gw_logd("DirCreate %d", (int)err);
        return 0;
    }

    err = FSMakeFSSpec(vRefNum, dirID, "\pGateway:Gateway Log.txt", &spec);
    if (err == fnfErr)
        err = FSpCreate(&spec, 'GT9A', 'TEXT', 0 /* smRoman: ASCII name */);
    if (err != noErr) {
        gw_logc("G40", "the log file could not be opened, so the log is "
                "kept in this window only");
        gw_logd("FSpCreate %d", (int)err);
        return 0;
    }

    err = FSpOpenDF(&spec, fsWrPerm, &refNum);
    if (err != noErr) {
        gw_logc("G40", "the log file could not be opened, so the log is "
                "kept in this window only");
        gw_logd("FSpOpenDF %d", (int)err);
        return 0;
    }

    SetFPos(refNum, fsFromLEOF, 0);     /* append across runs */
    sLogRef = refNum;
    gw_log("logging to Application Support:Gateway:Gateway Log.txt");
    return 1;
}

/*
 * Written and left unbuffered rather than accumulated: the sessions worth
 * capturing tend to be the ones that end in a crash, and a buffered tail is
 * exactly the part that would be lost. CR is the Mac OS line ending, so the
 * file opens correctly in SimpleText.
 */
void GWPlat_WriteLog(const char *line)
{
    long count;

    if (sLogRef == 0) return;

    count = (long)strlen(line);
    if (count > 0) FSWrite(sLogRef, &count, line);
    count = 1;
    FSWrite(sLogRef, &count, "\r");
}

void GWPlat_CloseLog(void)
{
    if (sLogRef != 0) {
        FSClose(sLogRef);
        sLogRef = 0;
    }
}

/* ------------------------------------------------------------------ */
/* A named file in the Preferences folder                              */
/* ------------------------------------------------------------------ */

/*
 * C string to Pascal, in a static, because FSMakeFSSpec wants a StringPtr and
 * the callers here pass literals. Names are Gateway's own and short; anything
 * longer than an HFS name is refused rather than truncated, since a truncated
 * name would silently read and write the wrong file.
 */
static int pascal_name(const char *leaf, Str63 out)
{
    size_t n = strlen(leaf);

    if (n == 0 || n > 63) return 0;
    out[0] = (unsigned char)n;
    memcpy(out + 1, leaf, n);
    return 1;
}

static int prefs_spec(const char *leaf, FSSpec *spec)
{
    short vRefNum;
    long  dirID;
    Str63 name;
    OSErr err;

    if (!pascal_name(leaf, name)) return 0;

    err = FindFolder(kOnSystemDisk, kPreferencesFolderType,
                     kDontCreateFolder, &vRefNum, &dirID);
    if (err != noErr) return 0;

    err = FSMakeFSSpec(vRefNum, dirID, name, spec);
    /* fnfErr still fills in the spec, which is what a write needs. */
    return (err == noErr || err == fnfErr);
}

long GWPlat_ReadFile(const char *leaf, void *buf, size_t cap)
{
    FSSpec spec;
    short  refNum;
    long   count;
    OSErr  err;

    if (leaf == NULL || buf == NULL) return -1;
    if (!prefs_spec(leaf, &spec)) return -1;

    err = FSpOpenDF(&spec, fsRdPerm, &refNum);
    if (err != noErr) return -1;

    count = (long)cap;
    err = FSRead(refNum, &count, buf);
    FSClose(refNum);

    if (err != noErr && err != eofErr) return -1;
    return count;
}

/* ------------------------------------------------------------------ */
/* Opening a URL in the default browser                                */
/* ------------------------------------------------------------------ */

/*
 * Internet Config, not any direct Finder or Apple Event call: it is the one
 * mechanism a Mac OS 9 application of this vintage has for "ask whatever
 * browser is registered to open a URL", and it is what carries a hint to the
 * shared library rather than to a specific browser Gateway would have to
 * name. ICStart/ICStop bracket the one call; the connection is not kept open
 * between clicks, since "Download" is pressed once per dialog at most.
 */
int GWPlat_OpenURL(const char *url)
{
    ICInstance inst;
    OSStatus   err;
    long       start, end;

    if (url == NULL || url[0] == '\0') return 0;

    err = ICStart(&inst, 'GT9A');
    if (err != noErr) return 0;

    /* No hint scheme (an empty Pascal string): url is always given to us as
     * a full "http://..." link, never a bare "host/path" that would need
     * one. selStart/selEnd bound the whole string, so ICLaunchURL parses it
     * all rather than hunting for a URL inside surrounding text. */
    start = 0;
    end = (long)strlen(url);
    err = ICLaunchURL(inst, "\p", url, end, &start, &end);

    ICStop(inst);
    return err == noErr;
}

int GWPlat_WriteFile(const char *leaf, const void *buf, long len)
{
    FSSpec spec;
    short  refNum;
    long   count = len;
    OSErr  err;

    if (leaf == NULL || buf == NULL || len < 0) return 0;
    if (!prefs_spec(leaf, &spec)) return 0;

    /*
     * Created with Gateway's own creator so the Finder shows it as ours, and
     * type 'BINA' because it is not text -- an RSA key opened in SimpleText by
     * a curious double-click would be offered for editing, and saved back
     * mangled.
     */
    err = FSpCreate(&spec, 'GT9A', 'BINA', 0 /* smRoman */);
    if (err != noErr && err != dupFNErr) {
        gw_logc("G41", "%s could not be saved", leaf);
        gw_logd("FSpCreate %d", (int)err);
        return 0;
    }

    err = FSpOpenDF(&spec, fsRdWrPerm, &refNum);
    if (err != noErr) {
        gw_logc("G41", "%s could not be saved", leaf);
        gw_logd("FSpOpenDF %d", (int)err);
        return 0;
    }

    err = SetEOF(refNum, 0);
    if (err == noErr) err = FSWrite(refNum, &count, buf);
    FSClose(refNum);

    if (err != noErr || count != len) {
        gw_logc("G41", "%s could not be saved", leaf);
        gw_logd("write %d", (int)err);
        return 0;
    }
    return 1;
}
