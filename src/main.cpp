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

#include <Devices.h>
#include <Events.h>
#include <Fonts.h>
#include <Menus.h>
#include <Quickdraw.h>
#include <TextEdit.h>
#include <ToolUtils.h>
#include <Windows.h>

#include <cstdio>
#include <cstring>

#include "gw_core.h"

namespace {

const short kAppleMenuID = 128;
const short kFileMenuID  = 129;

const short kAboutItem = 1;
const short kQuitItem  = 1;

const short kWinWidth   = 520;
const short kWinHeight   = 320;
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

class GatewayApp {
public:
    GatewayApp()
        : mWindow(nullptr), mAppleMenu(nullptr), mFileMenu(nullptr),
          mDone(false), mRunning(false), mSeenGeneration(-1) {}

    bool Start()
    {
        SetUpMenus();
        SetUpWindow();
        mRunning = GW_Init() != 0;
        return mWindow != nullptr;
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

            if (GW_LogGeneration() != mSeenGeneration) Redraw();
        }
    }

    void Stop()
    {
        GW_Shutdown();
        if (mWindow != nullptr) DisposeWindow(mWindow);
    }

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
        if (mWindow != nullptr) SetPort(reinterpret_cast<GrafPtr>(mWindow));
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
            if (win != FrontWindow()) SelectWindow(win);
            break;

        default:
            break;
        }
    }

    void HandleMenu(long selection)
    {
        short menu = HiWord(selection);
        short item = LoWord(selection);

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
            if (item == kQuitItem) mDone = true;
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
                      "proxy :%d    imap :%d    smtp :%d    splices %d",
                      GW_HttpPort(), GW_ImapPort(), GW_SmtpPort(),
                      GW_ActiveSessions());
        DrawCString(line);

        v = static_cast<short>(v + kLineHeight);
        MoveTo(kTextLeft, v);
        DrawCString(GW_StatusLine());

        v = static_cast<short>(v + 4);
        MoveTo(kTextLeft, v);
        LineTo(static_cast<short>(area.right - kTextLeft), v);

        {
            int rows = (area.bottom - kHeaderRows * kLineHeight) / kLineHeight;
            count = GW_LogCount();
            first = (count > rows) ? count - rows : 0;

            v = static_cast<short>(kHeaderRows * kLineHeight);
            for (i = first; i < count; i++) {
                const char *text = GW_LogLine(i);
                if (text == nullptr) break;
                v = static_cast<short>(v + kLineHeight);
                if (v > area.bottom - 2) break;
                MoveTo(kTextLeft, v);
                DrawCString(text);
            }
        }
    }

    WindowPtr  mWindow;
    MenuHandle mAppleMenu;
    MenuHandle mFileMenu;
    bool       mDone;
    bool       mRunning;
    long       mSeenGeneration;
};

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
