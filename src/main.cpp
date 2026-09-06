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
#include <Fonts.h>
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

/* SIZE resource flag: a background-only application has no menu bar, no
 * windows, and does not appear in the Application menu or in a dock. */
const short kOnlyBackgroundFlag = 0x0400;

const short kButtonHeight = 20;
const short kButtonWidth  = 60;
const short kButtonMargin = 8;

const short kWinWidth   = 520;
const short kWinHeight  = 340;
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

void DrawCString(const char *s)
{
    /* Multiversal types DrawText's buffer as Ptr, so the cast is required
     * even though QuickDraw only reads it. */
    DrawText(const_cast<char *>(s), 0, static_cast<short>(std::strlen(s)));
}

/* True when the Process Manager started us as a background-only application. */
bool RunningBackgroundOnly()
{
    ProcessSerialNumber psn;
    ProcessInfoRec      info;
    FSSpec              spec;

    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN  = kCurrentProcess;

    std::memset(&info, 0, sizeof(info));
    info.processInfoLength = sizeof(info);
    info.processName       = nullptr;
    info.processAppSpec    = &spec;

    if (GetProcessInformation(&psn, &info) != noErr) return false;
    return (info.processMode & modeOnlyBackground) != 0;
}

/*
 * Set or clear the onlyBackground bit in our own SIZE resource.
 *
 * There is no way to change this while running: the Process Manager reads
 * SIZE when it launches a process, and the answer is fixed for that run. So
 * the setting is written back to the application file and takes effect at the
 * next launch, which is how classic applications have always handled it.
 *
 * SIZE(0), when present, is what the Finder wrote after someone changed the
 * memory settings in Get Info, and it takes precedence over SIZE(-1); both are
 * updated when both exist.
 */
bool SetBackgroundOnlyFlag(bool on)
{
    ProcessSerialNumber psn;
    ProcessInfoRec      info;
    FSSpec              spec;
    short               saved, ref;
    bool                changed = false;
    const short         ids[2] = { 0, -1 };

    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN  = kCurrentProcess;

    std::memset(&info, 0, sizeof(info));
    info.processInfoLength = sizeof(info);
    info.processName       = nullptr;
    info.processAppSpec    = &spec;

    if (GetProcessInformation(&psn, &info) != noErr) return false;

    saved = CurResFile();
    ref = FSpOpenResFile(&spec, fsRdWrPerm);
    if (ref == -1) return false;      /* locked volume, or busy: leave it be */

    UseResFile(ref);
    for (int i = 0; i < 2; i++) {
        Handle h = Get1Resource('SIZE', ids[i]);
        short flags, want;

        if (h == nullptr || GetHandleSize(h) < 2) continue;

        flags = *reinterpret_cast<short *>(*h);
        want = on ? static_cast<short>(flags | kOnlyBackgroundFlag)
                  : static_cast<short>(flags & ~kOnlyBackgroundFlag);
        if (want == flags) continue;

        *reinterpret_cast<short *>(*h) = want;
        ChangedResource(h);
        WriteResource(h);
        changed = true;
    }
    if (changed) UpdateResFile(ref);
    CloseResFile(ref);
    UseResFile(saved);

    return changed;
}

class GatewayApp;
extern GatewayApp *gApp;
pascal OSErr HandleQuitEvent(const AppleEvent *event, AppleEvent *reply,
                             long refcon);

class GatewayApp {
public:
    GatewayApp()
        : mWindow(nullptr), mAppleMenu(nullptr), mFileMenu(nullptr),
          mDone(false), mRunning(false), mFaceless(false),
          mSeenGeneration(-1) {}

    bool Start()
    {
        bool wantWindow;

        GW_LoadSettings();
        mFaceless = RunningBackgroundOnly();
        wantWindow = GW_ShowWindowPref() != 0;

        /*
         * A faceless launch has no menu bar and no window by definition, so
         * there is nowhere to report a problem. If the core will not start,
         * come up with a window anyway rather than failing invisibly.
         */
        mRunning = GW_Init() != 0;
        if (!mRunning) wantWindow = true;

        if (!mFaceless && wantWindow) {
            SetUpMenus();
            SetUpWindow();
        }

        /*
         * Whether we appear in the Application menu is fixed at launch by the
         * SIZE resource, so bring the file into line with the setting for
         * next time. Only ever writes when the two actually disagree.
         */
        if (mRunning && SetBackgroundOnlyFlag(!wantWindow)) {
            GW_SetStatus(wantWindow
                             ? "quit and relaunch to show in the Application menu"
                             : "quit and relaunch to run without a window");
        }

        if (mFaceless) GW_SetStatus("running in the background");

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
            ToPascal("Hide Window/H", title);
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
     * The Quit button is drawn rather than made from a Control Manager
     * push button: the Multiversal Interfaces have no Controls.h, and one
     * rounded rectangle with a label is not worth an interface dependency.
     */
    Rect QuitButtonRect() const
    {
        Rect r;
        SetRect(&r,
                static_cast<short>(kWinWidth - kButtonMargin - kButtonWidth),
                static_cast<short>(kWinHeight - kButtonMargin - kButtonHeight),
                static_cast<short>(kWinWidth - kButtonMargin),
                static_cast<short>(kWinHeight - kButtonMargin));
        return r;
    }

    void DrawQuitButton()
    {
        Rect  r = QuitButtonRect();
        short width;

        PenNormal();
        EraseRoundRect(&r, 10, 10);
        FrameRoundRect(&r, 10, 10);

        TextFont(0);                    /* the system font, as a button wants */
        TextSize(12);
        width = TextWidth(const_cast<char *>("Quit"), 0, 4);
        MoveTo(static_cast<short>(r.left + ((r.right - r.left) - width) / 2),
               static_cast<short>(r.top + 14));
        DrawCString("Quit");
    }

    /*
     * Track a press the way the Control Manager would: highlight while the
     * mouse is held inside, and act only if it is released there. The proxy
     * is still pumped throughout, so holding the button down does not stall a
     * transfer.
     */
    void TrackQuitButton()
    {
        Rect    r = QuitButtonRect();
        Boolean inside = true;

        InvertRoundRect(&r, 10, 10);
        while (StillDown()) {
            Point   p;
            Boolean now;

            GetMouse(&p);
            now = PtInRect(p, &r);
            if (now != inside) {
                InvertRoundRect(&r, 10, 10);
                inside = now;
            }
            GW_Poll();
        }
        if (inside) {
            InvertRoundRect(&r, 10, 10);
            mDone = true;
        }
    }

    /* Take the window down without quitting: Gateway keeps proxying. */
    void HideWindow()
    {
        if (mWindow == nullptr) return;
        DisposeWindow(mWindow);
        mWindow = nullptr;
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
            if (win == mWindow) {
                Point local = event.where;
                Rect  button = QuitButtonRect();

                SetPort(reinterpret_cast<GrafPtr>(mWindow));
                GlobalToLocal(&local);
                if (PtInRect(local, &button)) TrackQuitButton();
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
                GW_SetStatus("Gateway 0.1 - TLS 1.3 gateway for Mac OS 9");
                Redraw();
            } else if (mAppleMenu != nullptr) {
                Str255 name;
                GetMenuItemText(mAppleMenu, item, name);
                OpenDeskAcc(name);
            }
            break;

        case kFileMenuID:
            if (item == kHideItem) HideWindow();
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

        DrawQuitButton();

        {
            short logBottom =
                static_cast<short>(area.bottom - kButtonHeight -
                                   2 * kButtonMargin);
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
    bool          mFaceless;
    long          mSeenGeneration;
};

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
