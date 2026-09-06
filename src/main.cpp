/*
 * main.cpp - Gateway's Classic Toolbox shell.
 *
 * Compiled against the Multiversal Interfaces, which use the classic header
 * names (<Windows.h>, not <MacWindows.h>). It must not include
 * <OpenTransport.h> or anything that reaches it: the Universal Interfaces are
 * supplied only to the Certainly and Open Transport translation units
 * (CLAUDE.md rule 3). Everything this file knows about the proxy comes through
 * the plain-C declarations in gw_core.h.
 *
 * Retro68's PowerPC crt0 does not run C++ global constructors (CLAUDE.md rule
 * 2), so there are no objects at file scope: the single application object is
 * allocated with new inside main().
 */

#include <AppleEvents.h>
#include <Devices.h>
#include <Events.h>
#include <Files.h>
#include <Fonts.h>
#include <Icons.h>
#include <Menus.h>
#include <Processes.h>
#include <Quickdraw.h>
#include <Resources.h>
#include <TextEdit.h>
#include <Windows.h>

#include <cstdio>
#include <cstring>

#include "gw_core.h"

namespace {

const short kAppleMenuID = 128;
const short kFileMenuID  = 129;

const short kAboutItem = 1;
const short kHideItem  = 1;
const short kQuitItem  = 3;

/*
 * Finder flags, from Finder.h. Written as literals so this file does not take
 * a dependency on which header the Multiversal Interfaces put them in.
 */
const short kFlagHasBundle    = 0x2000;
const short kFlagHasBeenInited = 0x0100;

/*
 * SIZE resource flag. A background-only application is kept out of the
 * Application menu by the Process Manager -- and out of any dock, since a
 * dock has nothing else to go on.
 *
 * Gateway only ever CLEARS this bit, never sets it. See ClearFacelessFlag().
 */
const short kOnlyBackgroundFlag = 0x0400;

/*
 * The application's own resource file, captured before anything else can
 * change the current one. Gateway reads its SIZE resource through this and
 * never closes it: the Resource Manager hands back the map that is already
 * open, so closing it would take the application's own resources with it.
 */
short gAppResFile = 0;

const short kWinWidth   = 520;
const short kWinHeight  = 340;

const short kAboutWidth  = 280;
const short kAboutHeight = 230;

const short kFontGeneva = 3;
const short kLineHeight = 11;
const short kTextLeft   = 6;
const short kHeaderRows = 3;

/* Build a Pascal string without relying on the compiler's "\p" literals. */
void ToPascal(const char *src, Str255 dst)
{
    size_t n = std::strlen(src);
    if (n > 255) n = 255;
    dst[0] = static_cast<unsigned char>(n);
    std::memcpy(dst + 1, src, n);
}

void DrawCenteredCString(short centerX, short baseline, const char *s)
{
    short len = static_cast<short>(std::strlen(s));
    short width = TextWidth(const_cast<char *>(s), 0, len);

    MoveTo(static_cast<short>(centerX - width / 2), baseline);
    DrawText(const_cast<char *>(s), 0, len);
}

void DrawCString(const char *s)
{
    /* Multiversal types DrawText's buffer as Ptr, so the cast is required
     * even though QuickDraw only reads it. */
    DrawText(const_cast<char *>(s), 0, static_cast<short>(std::strlen(s)));
}

/*
 * Ask the Finder to use our icon.
 *
 * The icon family, BNDL and FREF only take effect when the application file
 * carries the "has bundle" flag, and the build toolchain does not set it.
 * Setting it here is a plain file-info write -- unlike touching our own
 * resource fork, which cannot be done safely while running: the Resource
 * Manager hands back the map that is already open, and closing it takes the
 * application's own resources with it.
 *
 * Clearing "has been inited" is what asks the Finder to look again.
 */
void EnsureBundleBit()
{
    ProcessSerialNumber psn;
    ProcessInfoRec      info;
    FSSpec              spec;
    FInfo               finder;

    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN  = kCurrentProcess;

    std::memset(&info, 0, sizeof(info));
    info.processInfoLength = sizeof(info);
    info.processName       = nullptr;
    info.processAppSpec    = &spec;

    if (GetProcessInformation(&psn, &info) != noErr) return;
    if (FSpGetFInfo(&spec, &finder) != noErr) return;
    if ((finder.fdFlags & kFlagHasBundle) != 0) return;

    finder.fdFlags |= kFlagHasBundle;
    finder.fdFlags &= ~kFlagHasBeenInited;
    FSpSetFInfo(&spec, &finder);
}

/*
 * Clear onlyBackground if some earlier version left it set.
 *
 * Gateway used to set this bit when the window was hidden, which produced a
 * running application with no window, no menu bar and no Application menu
 * entry -- nothing to quit it with short of restarting the machine. Hiding a
 * window must never be the thing that makes an application unreachable, so
 * nothing sets it any more and this repairs a file that still carries it.
 *
 * The edit goes through the resource map the Process Manager already opened,
 * and closes nothing. A read-only fork simply fails the write.
 *
 * SIZE(0) is what the Finder writes after someone edits the memory settings
 * in Get Info and takes precedence over SIZE(-1); both are checked.
 */
void ClearFacelessFlag()
{
    const short ids[2] = { 0, -1 };
    short saved = CurResFile();
    bool  changed = false;

    if (gAppResFile == 0) return;

    UseResFile(gAppResFile);
    for (int i = 0; i < 2; i++) {
        Handle h = Get1Resource('SIZE', ids[i]);
        short  flags;

        if (h == nullptr || GetHandleSize(h) < 2) continue;

        flags = *reinterpret_cast<short *>(*h);
        if ((flags & kOnlyBackgroundFlag) == 0) continue;

        *reinterpret_cast<short *>(*h) =
            static_cast<short>(flags & ~kOnlyBackgroundFlag);
        ChangedResource(h);
        if (ResError() != noErr) continue;
        WriteResource(h);
        if (ResError() == noErr) changed = true;
    }
    if (changed) UpdateResFile(gAppResFile);
    UseResFile(saved);
}

/*
 * A background-only Gateway has no menu bar and no window, so the Quit Apple
 * event is the only way to stop it short of restarting: the Finder sends it at
 * shutdown, and AppleScript can send it on demand.
 *
 * The handler cannot capture, so it reaches the application through a file
 * scope pointer. That is a plain pointer with no constructor, which is what
 * CLAUDE.md rule 2 requires -- Retro68's PowerPC crt0 would never have run one.
 */
GatewayApp *gApp = nullptr;

pascal OSErr HandleQuitEvent(const AppleEvent *event, AppleEvent *reply,
                             long refcon)
{
    (void)event;
    (void)reply;
    (void)refcon;

    if (gApp != nullptr) gApp->Quit();
    return noErr;
}

}  /* namespace */

int main()
{
    /* Before anything can change the current resource file: Gateway edits its
     * own SIZE resource through this refNum and must never close it. */
    gAppResFile = CurResFile();

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(nullptr);
    InitCursor();

    /* Heap-allocated on purpose: Retro68's PPC crt0 skips global
     * constructors, so nothing may live at file scope. */
    GatewayApp *app = new GatewayApp();

    if (app->Start()) app->Run();
    app->Stop();

    delete app;
    return 0;
}
