/*
 * gw_plat_win32.c - gw_plat.h on Windows 95 OSR2 and up.
 *
 * The counterpart of gw_plat_mac.c. Where the Mac build asks the Folder
 * Manager where preferences live, this one puts them beside the executable:
 * on the systems Gateway targets there is no per-user application data folder
 * worth relying on -- 95 and 98 have no profiles by default and NT's are in a
 * different place again -- and a proxy that keeps its settings next to itself
 * is also the one that can be moved to another machine on a floppy.
 */

#include "gw_plat.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "portable/gw_log.h"

static char sSource[MAX_PATH + 32] = "defaults (no prefs file)";
static char sPrefsPath[MAX_PATH];
static int  sHavePath;
static FILE *sLogFile;

/*
 * Build a path in the executable's own directory.
 *
 * GetModuleFileName is available on every target and needs no shell or
 * registry, both of which differ across this range of Windows versions.
 */
static int path_beside_exe(const char *leaf, char *out, size_t cap)
{
    char  exe[MAX_PATH];
    DWORD n;
    char *slash;

    n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (n == 0 || n >= sizeof(exe)) return 0;

    slash = strrchr(exe, '\\');
    if (slash == NULL) return 0;
    *(slash + 1) = '\0';

    if (strlen(exe) + strlen(leaf) >= cap) return 0;
    strcpy(out, exe);
    strcat(out, leaf);
    return 1;
}

long GWPlat_ReadPrefs(char *buf, size_t cap)
{
    FILE  *f;
    size_t n;

    if (!path_beside_exe("Gateway.ini", sPrefsPath, sizeof(sPrefsPath)))
        return -1;
    sHavePath = 1;

    /*
     * Binary, not text. The portable parser handles CR, LF and CRLF itself
     * (Mac OS 9 writes CR), and a text-mode read on Windows would quietly
     * rewrite line endings underneath it.
     */
    f = fopen(sPrefsPath, "rb");
    if (f == NULL) return -1;

    n = fread(buf, 1, cap, f);
    fclose(f);

    sprintf(sSource, "%s", sPrefsPath);
    return (long)n;
}

const char *GWPlat_PrefsSource(void)
{
    return sSource;
}

int GWPlat_WritePrefs(const char *buf, long len)
{
    FILE  *f;
    size_t n;

    if (!sHavePath) {
        gw_log("cannot save settings: no prefs file to write to");
        return 0;
    }

    f = fopen(sPrefsPath, "wb");
    if (f == NULL) {
        gw_log("cannot save settings: prefs file would not open");
        return 0;
    }

    n = fwrite(buf, 1, (size_t)len, f);
    if (fclose(f) != 0 || n != (size_t)len) {
        gw_log("cannot save settings: write failed");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* The log file                                                        */
/* ------------------------------------------------------------------ */

int GWPlat_OpenLog(const char *want)
{
    char path[MAX_PATH];

    /*
     * A switch means "beside the executable"; anything else is taken as a
     * path, so a log can be sent to a share or another drive without a
     * rebuild.
     */
    if (want != NULL && want[0] != '\0' &&
        strcmp(want, "1") != 0 && _stricmp(want, "yes") != 0 &&
        _stricmp(want, "on") != 0 && _stricmp(want, "true") != 0) {
        if (strlen(want) >= sizeof(path)) return 0;
        strcpy(path, want);
    } else if (!path_beside_exe("Gateway.log", path, sizeof(path))) {
        return 0;
    }

    sLogFile = fopen(path, "ab");
    if (sLogFile == NULL) {
        gw_log("log file: could not open %s", path);
        return 0;
    }
    gw_log("logging to %s", path);
    return 1;
}

/*
 * Flushed per line rather than buffered: the sessions worth capturing tend to
 * be the ones that end badly, and a buffered tail is exactly the part that
 * would be lost. CRLF, so Notepad shows it as lines rather than one long one.
 */
void GWPlat_WriteLog(const char *line)
{
    if (sLogFile == NULL) return;

    fputs(line, sLogFile);
    fputs("\r\n", sLogFile);
    fflush(sLogFile);
}

void GWPlat_CloseLog(void)
{
    if (sLogFile != NULL) {
        fclose(sLogFile);
        sLogFile = NULL;
    }
}
