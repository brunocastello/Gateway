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
 * SIZE resource flag. The Process Manager reads it at launch and keeps the
 * application out of the Application menu -- and out of any dock, which has
 * nothing else to go on. Gateway only ever CLEARS this bit, never sets it;
 * see ClearFacelessFlag().
 */
const short kOnlyBackgroundFlag = 0x0400;

/*
 * The application's own resource file, captured before anything else can
 * change the current one. Gateway edits its SIZE resource through this and
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
 * Clear onlyBackground if an earlier version left it set.
 *
 * Gateway used to set this bit when the window was hidden. That produced a
 * running application with no window, no menu bar and no Application menu
 * entry -- nothing to quit it with short of restarting the machine. Hiding a
 * window must never be what makes an application unreachable, so nothing sets
 * it any more and this repairs a file that still carries it.
 *
 * The edit goes through the resource map the Process Manager already opened,
 * and closes nothing: the Resource Manager hands back the map that is already
 * open, so closing it would take the application's own resources with it. A
 * read-only fork simply fails the write.
 *
 * SIZE(0) is what the Finder writes after someone edits the memory settings in
 * Get Info and takes precedence over SIZE(-1); both are checked.
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

class GatewayApp;
extern GatewayApp *gApp;
pascal OSErr HandleQuitEvent(const AppleEvent *event, AppleEvent *reply,
                             long refcon);

class GatewayApp {
public:
    GatewayApp()
        : mWindow(nullptr), mAppleMenu(nullptr), mFileMenu(nullptr),
          mDone(false), mRunning(false), mSeenGeneration(-1) {}

    bool Start()
    {
        bool wantWindow;

        GW_LoadSettings();
        wantWindow = GW_ShowWindowPref() != 0;

        mRunning = GW_Init() != 0;

        /* Nowhere to report a problem without a window, so come up visible if
         * the core will not start. */
        if (!mRunning) wantWindow = true;

        /* The menu bar is always present: it is how Gateway is quit, and how
         * the window is brought back. */
        SetUpMenus();
        if (wantWindow) SetUpWindow();

        EnsureBundleBit();
        ClearFacelessFlag();

        gApp = this;
        AEInstallEventHandler(kCoreEventClass, kAEQuitApplication,
                              NewAEEventHandlerUPP(HandleQuitEvent), 0, false);
        return true;
    }

    void Run()
    {
        while (!mDone) {
            EventRecord event;
            long sleep = (GW_ActiveSessions() > 0) ? 1L : 10L;

            if (WaitNextEvent(everyEvent, &event, sleep, nullptr))
                HandleEvent(event);

            /* One cooperative slice per pass; every OT and TLS step inside
             * yields rather than spinning (CLAUDE.md rule 6). */
            GW_Poll();

            if (mWindow != nullptr && GW_LogGeneration() != mSeenGeneration)
                Redraw();
        }
    }

    void Stop()
    {
        GW_Shutdown();
        if (mWindow != nullptr) DisposeWindow(mWindow);
    }

    void Quit() { mDone = true; }

private:
    void SetUpMenus()
    {
        Str255 title;
        char   appleTitle[2] = { '\024', '\0' };    /* the Apple glyph */

        ToPascal(appleTitle, title);
        mAppleMenu = NewMenu(kAppleMenuID, title);
        if (mAppleMenu != nullptr) {
            ToPascal("About Gateway...", title);
            AppendMenu(mAppleMenu, title);
            ToPascal("(-", title);
            AppendMenu(mAppleMenu, title);
            AppendResMenu(mAppleMenu, 'DRVR');
            InsertMenu(mAppleMenu, 0);
        }

        ToPascal("File", title);
        mFileMenu = NewMenu(kFileMenuID, title);
        if (mFileMenu != nullptr) {
            ToPascal("Show Window/H", title);
            AppendMenu(mFileMenu, title);
            ToPascal("(-", title);
            AppendMenu(mFileMenu, title);
            ToPascal("Quit/Q", title);
            AppendMenu(mFileMenu, title);
            InsertMenu(mFileMenu, 0);
        }
        DrawMenuBar();
    }

    void SetUpWindow()
    {
        Rect   bounds;
        Str255 title;

        SetRect(&bounds, 20, 60, 20 + kWinWidth, 60 + kWinHeight);
        ToPascal("Gateway", title);
        mWindow = NewWindow(nullptr, &bounds, title, true, documentProc,
                            reinterpret_cast<WindowPtr>(-1L), true, 0);
        if (mWindow == nullptr) return;

        SetPort(reinterpret_cast<GrafPtr>(mWindow));
    }

    /*
     * About Gateway, laid out the way iWordle's is: the application icon,
     * then alternating Charcoal and Geneva lines for name, author and
     * credits. A real title bar with a close box and no OK button, which is
     * the Mac OS 9 convention -- SimpleText's About box does the same.
     */
    void DrawAboutContent(WindowPtr w)
    {
        GrafPtr port = reinterpret_cast<GrafPtr>(w);
        Rect    box = port->portRect;
        Rect    iconRect;
        Str255  fontName;
        short   midX, charcoal;

        SetPort(port);
        EraseRect(&box);

        midX = static_cast<short>(box.left + (box.right - box.left) / 2);

        SetRect(&iconRect, static_cast<short>(midX - 16),
                static_cast<short>(box.top + 14),
                static_cast<short>(midX + 16),
                static_cast<short>(box.top + 46));
        PlotIconID(&iconRect, atNone, ttNone, 128);

        /* Charcoal is a later TrueType face rather than a fixed classic font
         * ID, so it has to be looked up by name; GetFNum falls back to the
         * system font when it is not installed. */
        ToPascal("Charcoal", fontName);
        GetFNum(fontName, &charcoal);

        TextFace(normal);

        TextFont(charcoal);
        TextSize(12);
        DrawCenteredCString(midX, static_cast<short>(box.top + 64),
                            "Gateway 0.1");

        TextFont(kFontGeneva);
        TextSize(10);
        DrawCenteredCString(midX, static_cast<short>(box.top + 84),
                            "A TLS 1.3 gateway for Mac OS 9");

        TextFont(charcoal);
        TextSize(12);
        DrawCenteredCString(midX, static_cast<short>(box.top + 112),
                            "Bruno Castello");

        TextFont(kFontGeneva);
        TextSize(10);
        DrawCenteredCString(midX, static_cast<short>(box.top + 132),
                            "bfcastello@hotmail.com");

        TextFont(charcoal);
        TextSize(12);
        DrawCenteredCString(midX, static_cast<short>(box.top + 160),
                            "Engineer: Claude Opus 5");

        TextFont(kFontGeneva);
        TextSize(10);
        DrawCenteredCString(midX, static_cast<short>(box.top + 188),
                            "\xA9 Castello Designs, 2026");
        DrawCenteredCString(midX, static_cast<short>(box.top + 208),
                            "Built with Retro68");
    }

    void ShowAbout()
    {
        Rect        bounds;
        Str255      title;
        WindowPtr   about;
        Boolean     done = false;
        EventRecord event;
        short       left, top;

        left = static_cast<short>((qd.screenBits.bounds.right -
                                   qd.screenBits.bounds.left - kAboutWidth) / 2);
        top = static_cast<short>((qd.screenBits.bounds.bottom -
                                  qd.screenBits.bounds.top - kAboutHeight) / 3);
        SetRect(&bounds, left, top,
                static_cast<short>(left + kAboutWidth),
                static_cast<short>(top + kAboutHeight));

        ToPascal("About Gateway", title);
        about = NewCWindow(nullptr, &bounds, title, true, noGrowDocProc,
                           reinterpret_cast<WindowPtr>(-1L), true, 0);
        if (about == nullptr) return;

        SelectWindow(about);

        while (!done && !mDone) {
            WaitNextEvent(everyEvent, &event, 5, nullptr);

            switch (event.what) {
            case updateEvt:
                if (reinterpret_cast<WindowPtr>(event.message) == about) {
                    BeginUpdate(about);
                    DrawAboutContent(about);
                    EndUpdate(about);
                } else {
                    HandleEvent(event);
                }
                break;

            case keyDown:
            case autoKey: {
                char c = static_cast<char>(event.message & charCodeMask);
                if (c == '\r' || c == 3 || c == 27) done = true;
                break;
            }

            case mouseDown: {
                WindowPtr win;
                short part = FindWindow(event.where, &win);

                if (win != about) break;        /* About stays in front */
                if (part == inGoAway) {
                    if (TrackGoAway(about, event.where)) done = true;
                } else if (part == inDrag) {
                    Rect limit = qd.screenBits.bounds;
                    InsetRect(&limit, 4, 4);
                    DragWindow(about, event.where, &limit);
                }
                break;
            }

            default:
                break;
            }

            /* The proxy keeps running while the box is open. */
            GW_Poll();
        }

        DisposeWindow(about);
        if (mWindow != nullptr) Redraw();
    }

    /* Put the window away, or bring it back. Either way Gateway keeps
     * proxying; only the display stops. */
    void ToggleWindow()
    {
        Str255 title;
        bool   showing;

        if (mWindow != nullptr) {
            DisposeWindow(mWindow);
            mWindow = nullptr;
            showing = false;
            ToPascal("Show Window/H", title);
        } else {
            SetUpWindow();
            Redraw();
            showing = true;
            ToPascal("Hide Window/H", title);
        }
        if (mFileMenu != nullptr) SetMenuItemText(mFileMenu, kHideItem, title);

        /*
         * Remember the choice for next time, but change nothing else about
         * this session. The menu bar stays exactly where it is, so Quit and
         * Show Window are always one click away -- hiding a window must never
         * be the thing that makes an application unreachable.
         *
         * Going faceless is a launch-time decision, applied in Start().
         */
        GW_SetShowWindowPref(showing ? 1 : 0);
        if (!showing)
            GW_Log("window hidden; Gateway keeps running");
        if (mWindow != nullptr) Redraw();
    }

    void HandleEvent(EventRecord &event)
    {
        switch (event.what) {
        case mouseDown:
            HandleMouseDown(event);
            break;

        case keyDown:
        case autoKey: {
            char ch = static_cast<char>(event.message & charCodeMask);
            if (event.modifiers & cmdKey) HandleMenu(MenuKey(ch));
            break;
        }

        case updateEvt: {
            WindowPtr win = reinterpret_cast<WindowPtr>(event.message);
            BeginUpdate(win);
            if (win == mWindow) DrawContents();
            EndUpdate(win);
            break;
        }

        case kHighLevelEvent:
            AEProcessAppleEvent(&event);
            break;

        case activateEvt:
        default:
            break;
        }
    }

    void HandleMouseDown(EventRecord &event)
    {
        WindowPtr win;
        short     part = FindWindow(event.where, &win);

        switch (part) {
        case inMenuBar:
            HandleMenu(MenuSelect(event.where));
            break;

        case inSysWindow:
            SystemClick(&event, win);
            break;

        case inDrag: {
            Rect limit = qd.screenBits.bounds;
            InsetRect(&limit, 4, 4);
            DragWindow(win, event.where, &limit);
            break;
        }

        case inGoAway:
            if (TrackGoAway(win, event.where)) mDone = true;
            break;

        case inContent:
            if (win != FrontWindow()) {
                SelectWindow(win);
                break;
            }
            break;

        default:
            break;
        }
    }

    void HandleMenu(long selection)
    {
        /* MenuSelect packs the menu ID in the high word and the item in the
         * low one. HiWord/LoWord are 68K trap glue that InterfaceLib does not
         * export to PowerPC, so unpack it by hand. */
        short menu = static_cast<short>((selection >> 16) & 0xFFFF);
        short item = static_cast<short>(selection & 0xFFFF);

        switch (menu) {
        case kAppleMenuID:
            if (item == kAboutItem) {
                ShowAbout();
            } else if (mAppleMenu != nullptr) {
                Str255 name;
                GetMenuItemText(mAppleMenu, item, name);
                OpenDeskAcc(name);
            }
            break;

        case kFileMenuID:
            if (item == kHideItem) ToggleWindow();
            else if (item == kQuitItem) mDone = true;
            break;

        default:
            break;
        }
        HiliteMenu(0);
    }

    void Redraw()
    {
        if (mWindow == nullptr) return;
        SetPort(reinterpret_cast<GrafPtr>(mWindow));
        DrawContents();
        mSeenGeneration = GW_LogGeneration();
    }

    void DrawContents()
    {
        GrafPtr port = reinterpret_cast<GrafPtr>(mWindow);
        Rect    area = port->portRect;
        char    line[160];
        short   v;
        int     count, first, i;

        SetPort(port);
        EraseRect(&area);

        TextFont(4);           /* Monaco: the log needs a fixed pitch */
        TextSize(9);

        v = kLineHeight;
        MoveTo(kTextLeft, v);
        std::snprintf(line, sizeof(line),
                      "proxy :%d  imap :%d  pop :%d  smtp :%d  splices %d",
                      GW_HttpPort(), GW_ImapPort(), GW_PopPort(),
                      GW_SmtpPort(), GW_ActiveSessions());
        DrawCString(line);

        v = static_cast<short>(v + kLineHeight);
        MoveTo(kTextLeft, v);
        DrawCString(GW_StatusLine());

        v = static_cast<short>(v + 4);
        MoveTo(kTextLeft, v);
        LineTo(static_cast<short>(area.right - kTextLeft), v);

        {
            short logBottom = static_cast<short>(area.bottom - 2);
            int rows = (logBottom - kHeaderRows * kLineHeight) / kLineHeight;
            count = GW_LogCount();
            first = (count > rows) ? count - rows : 0;

            v = static_cast<short>(kHeaderRows * kLineHeight);
            for (i = first; i < count; i++) {
                const char *text = GW_LogLine(i);
                if (text == nullptr) break;
                v = static_cast<short>(v + kLineHeight);
                if (v > logBottom) break;
                MoveTo(kTextLeft, v);
                DrawCString(text);
            }
        }
    }

    WindowPtr     mWindow;
    MenuHandle    mAppleMenu;
    MenuHandle    mFileMenu;
    bool          mDone;
    bool          mRunning;
    long          mSeenGeneration;
};

/*
 * The Quit Apple event: the Finder sends it at shutdown and restart, and
 * AppleScript can send it on demand.
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
