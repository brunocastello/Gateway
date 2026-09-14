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
/*
 * The Control Manager. Multiversal has no Controls.h -- its declarations come
 * out in Multiverse.h, which is why an earlier attempt concluded, wrongly,
 * that controls were unavailable.
 */
#include <Multiverse.h>
#include <Processes.h>
#include <Quickdraw.h>
#include <Resources.h>
#include <TextEdit.h>
#include <Windows.h>

#include <cstdio>
#include <cstring>

#include "gw_core.h"
#include "gw_version.h"
#include "portable/gw_log.h"   /* GW_LOG_LINES, for scroll limits */
#include "portable/gw_util.h"  /* gw_parse_dec, gw_stricmp */
#include "gw_config.h"         /* GWConfig_Num, GWConfig_Set, GWConfig_Load */

namespace {

const short kAppleMenuID = 128;
const short kFileMenuID  = 129;

const short kAboutItem = 1;
const short kHideItem  = 1;
const short kStopItem  = 2;
const short kSettingsItem = 3;
const short kQuitItem  = 4;

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

/*
 * A document window with both a zoom box and a grow box: WDEF 0, variant 8.
 * Multiversal's enum stops at movableDBoxProc and rDocProc, so the standard
 * procID goes in by value, as the Monaco font ID above does.
 */
const short kZoomDocProc = 8;



/*
 * Scroll bar geometry and part codes, by value. scrollBarProc is CDEF 16, and
 * the part codes are the classic ones: the names are in Multiversal but
 * spelling them out keeps this block readable next to kZoomDocProc.
 */
const short kScrollBarProc = 16;
const short kScrollWidth   = 15;
const short kInUpButton    = 20;
const short kInDownButton  = 21;
const short kInPageUp      = 22;
const short kInPageDown    = 23;
const short kInThumb       = 129;

/*
 * Platinum. Mac OS 9's window background is 0xDD grey, not white -- an About
 * box painted white reads as a document window rather than part of the system.
 * Matches iWordle's kColorWindowBG.
 */
const unsigned short kPlatinum = 0xDDDD;
const short kLineHeight = 11;
const short kTextLeft   = 6;
const short kHeaderRows = 0;

/* Control procedure IDs (CDEF numbers). Multiversal does not export these,
 * so we define them by value. */
const short kButtonProc       = 0;
const short kCheckBoxProc     = 4;
const short kPopUpButtonProc  = 19;

/* TextEdit CDEF (28) — not exported by Multiversal. */
const short kTextEditProc     = 28;

/* Control reference constants for identification. */
typedef ControlHandle GWControlRef;

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
          mDone(false), mRunning(false), mSeenGeneration(-1),
          mScrollBack(0), mScroll(nullptr), mShownLines(0) {}

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
        UpdateWindowMenuItem();

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
            ToPascal("Hide Window/H", title);
            AppendMenu(mFileMenu, title);
            ToPascal("Stop Gateway/S", title);
            AppendMenu(mFileMenu, title);
            ToPascal("Settings...S", title);
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
        mWindow = NewWindow(nullptr, &bounds, title, true, kZoomDocProc,
                            reinterpret_cast<WindowPtr>(-1L), true, 0);
        if (mWindow == nullptr) return;

        SetPort(reinterpret_cast<GrafPtr>(mWindow));

        SetRect(&bounds, 0, 0, kScrollWidth, 50);   /* placed by LayoutScroll */
        ToPascal("", title);
        mScroll = NewControl(mWindow, &bounds, title, true, 0, 0, 0,
                             kScrollBarProc, 0);
        LayoutScroll();
    }

    /*
     * Along the right edge, stopping short of the grow box, overlapping the
     * window frame by a pixel the way a document window's scroll bar does.
     */
    void LayoutScroll()
    {
        Rect area;

        if (mWindow == nullptr || mScroll == nullptr) return;
        area = reinterpret_cast<GrafPtr>(mWindow)->portRect;

        HideControl(mScroll);
        MoveControl(mScroll, static_cast<short>(area.right - kScrollWidth),
                    static_cast<short>(area.top - 1));
        SizeControl(mScroll, static_cast<short>(kScrollWidth + 1),
                    static_cast<short>(area.bottom - area.top - kScrollWidth + 2));
        ShowControl(mScroll);
    }

    /* Point the scroll bar at where the log actually is. */
    void UpdateScroll()
    {
        int count, rows, most;

        if (mScroll == nullptr) return;

        count = GW_LogCount();
        rows = (mShownLines > 0) ? mShownLines : VisibleRows();
        most = count - rows;
        if (most < 0) most = 0;
        if (mScrollBack > most) mScrollBack = most;

        SetControlMaximum(mScroll, static_cast<short>(most));
        SetControlValue(mScroll, static_cast<short>(most - mScrollBack));
    }

    /* One click in an arrow or a page region. */
    void ScrollByPart(short part)
    {
        int page = VisibleRows() - 1;

        if (page < 1) page = 1;

        switch (part) {
        case kInUpButton:   Scroll(1);      break;
        case kInDownButton: Scroll(-1);     break;
        case kInPageUp:     Scroll(page);   break;
        case kInPageDown:   Scroll(-page);  break;
        default: break;
        }
    }

    /*
     * About Gateway, laid out the way iWordle's is: the application icon,
     * then alternating Charcoal and Geneva lines for name, author and
     * credits. A real title bar with a close box and no OK button, which is
     * the Mac OS 9 convention -- SimpleText's About box does the same.
     */
    /*
     * Settings window handler.
     *
     * Creates a movable dialog box with a pop-up section selector at top,
     * then checkboxes, edit fields and text areas for each preference
     * section.  Follows Mac OS 9 Platinum HIG exactly: Geneva 9 for
     * labels, Monaco 9 inside text areas, standard controls (CDEF 4
     * checkboxes, CDEF 19 pop-up buttons, CDEF 16 scroll bars).
     */
    void HandleSettings()    {
        Rect        bounds;
        Str255      title;
        WindowPtr   win;
        Boolean     done = false;
        EventRecord event;
        short       left, top;

        const short kSettingsWidth  = 380;
        const short kSettingsHeight = 290;

        left = static_cast<short>((qd.screenBits.bounds.right -
                                   qd.screenBits.bounds.left - kSettingsWidth) / 2);
        top = static_cast<short>((qd.screenBits.bounds.bottom -
                                  qd.screenBits.bounds.top - kSettingsHeight) / 3);
        SetRect(&bounds, left, top,
                static_cast<short>(left + kSettingsWidth),
                static_cast<short>(top + kSettingsHeight));

        ToPascal("Settings for Gateway...", title);
        win = NewCWindow(nullptr, &bounds, title, true,
                         kZoomDocProc, reinterpret_cast<WindowPtr>(-1L),
                         true, 0);
        if (win == nullptr) return;

        SelectWindow(win);
        SetPort(reinterpret_cast<GrafPtr>(win));

        RGBColor platinum;
        platinum.red = platinum.green = platinum.blue = kPlatinum;
        RGBBackColor(&platinum);

        /* ---- Section pop-up button (top left) ---- */
        const short kSectionPopUpID = 1;
        Rect popupRect;
        SetRect(&popupRect, static_cast<short>(bounds.left + 12),
                static_cast<short>(bounds.top + 8),
                static_cast<short>(bounds.left + 132),
                static_cast<short>(bounds.top + 24));
        ToPascal("", title);
        ControlHandle hSection = NewControl(win, &popupRect, title,
                                            true, kPopUpButtonProc, 0, 0, 3,
                                            kSectionPopUpID);

        /* ---- OK and Cancel buttons (bottom right) ---- */
        const short kOKBtnID = 1;
        const short kCancelBtnID = 2;
        Rect okRect, cancelRect;

        SetRect(&okRect, static_cast<short>(bounds.right - 104),
                static_cast<short>(bounds.bottom - 28),
                static_cast<short>(bounds.right - 54),
                static_cast<short>(bounds.bottom - 12));
        ToPascal("OK", title);
        ControlHandle hOK = NewControl(win, &okRect, title,
                                       true, kButtonProc, 0, 0, 0,
                                       kOKBtnID);

        SetRect(&cancelRect, static_cast<short>(bounds.right - 48),
                static_cast<short>(bounds.bottom - 28),
                static_cast<short>(bounds.right - 4),
                static_cast<short>(bounds.bottom - 12));
        ToPascal("Cancel", title);
        ControlHandle hCancel = NewControl(win, &cancelRect, title,
                                           true, kButtonProc, 0, 0, 0,
                                           kCancelBtnID);

        /* ---- Content area (below pop-up, above buttons) ---- */
        Rect contentRect;
        SetRect(&contentRect,
                static_cast<short>(bounds.left + 8),
                static_cast<short>(bounds.top + 28),
                static_cast<short>(bounds.right - 16),
                static_cast<short>(bounds.bottom - 40));

        /* ---- Create controls for each section, all hidden except current ----
         *
         * In Classic Toolbox (Multiversal):
         *   - Checkboxes / pop-up buttons: NewControl with procID 4 or 19
         *   - Edit fields: TEHandle via CDEF 28 (NewTE / TESetRect) +
         *     TESetText(TEHandle, Ptr) — second arg is the text buffer
         *   - Scroll bars: NewControl with procID 16 (kScrollBarProc)
         *   - Buttons: NewControl with procID 0 (kButtonProc)
         */
        const short kMaxSections = 4;
        ControlHandle sectionControls[kMaxSections][32];
        short sectionControlCount[kMaxSections];
        std::memset(sectionControls, 0, sizeof(sectionControls));
        std::memset(sectionControlCount, 0, sizeof(sectionControlCount));

        /* Section 0: Application (7 controls)
         *   0=show_window chk, 1=max_sessions edit(TE),
         *   2=http_enabled chk, 3=mail_enabled chk,
         *   4=wayback_enabled chk, 5=log_file chk,
         *   6=max_connects edit(TE) */
        {
            short y = contentRect.top;
            short x = contentRect.left + 4;

            /* show_window checkbox */
            SetRect(&popupRect, x, y, contentRect.right - 4,
                    static_cast<short>(y + 12));
            ToPascal("Show window at launch", title);
            sectionControls[0][sectionControlCount[0]] =
                NewControl(win, &popupRect, title, true, kCheckBoxProc, 0,
                           0, 0, sectionControlCount[0]++);
            SetControlValue(sectionControls[0][0],
                            GW_ShowWindowPref() != 0);
            y += 14;

            /* max_sessions edit field (TEHandle via CDEF 28) */
            char buf[8];
            sprintf(buf, "%ld", GWConfig_Num("max_sessions", 12));
            SetRect(&popupRect, x + 140, y, static_cast<short>(x + 200),
                    static_cast<short>(y + 12));
            ToPascal("Max sessions", title);
            TEHandle te = reinterpret_cast<TEHandle>(NewControl(
                win, &popupRect, title, true, kTextEditProc, 0, 0, 0,
                sectionControlCount[0]++));
            TESetText(te, reinterpret_cast<Ptr>(buf),
                      static_cast<short>(strlen(buf)));
            y += 14;

            /* http_enabled checkbox */
            SetRect(&popupRect, x, y, contentRect.right - 4,
                    static_cast<short>(y + 12));
            ToPascal("Enable HTTP proxy", title);
            sectionControls[0][sectionControlCount[0]] =
                NewControl(win, &popupRect, title, true, kCheckBoxProc, 0,
                           0, 0, sectionControlCount[0]++);
            SetControlValue(sectionControls[0][2],
                            GWConfig_Num("http_enabled", 1) != 0);
            y += 14;

            /* mail_enabled checkbox */
            SetRect(&popupRect, x, y, contentRect.right - 4,
                    static_cast<short>(y + 12));
            ToPascal("Enable mail splice", title);
            sectionControls[0][sectionControlCount[0]] =
                NewControl(win, &popupRect, title, true, kCheckBoxProc, 0,
                           0, 0, sectionControlCount[0]++);
            SetControlValue(sectionControls[0][3],
                            GWConfig_Num("mail_enabled", 1) != 0);
            y += 14;

            /* wayback_enabled checkbox */
            SetRect(&popupRect, x, y, contentRect.right - 4,
                    static_cast<short>(y + 12));
            ToPascal("Enable Wayback proxy", title);
            sectionControls[0][sectionControlCount[0]] =
                NewControl(win, &popupRect, title, true, kCheckBoxProc, 0,
                           0, 0, sectionControlCount[0]++);
            SetControlValue(sectionControls[0][4],
                            GWConfig_Num("wayback_enabled", 1) != 0);
            y += 14;

            /* log_file checkbox */
            SetRect(&popupRect, x, y, contentRect.right - 4,
                    static_cast<short>(y + 12));
            ToPascal("Write log to file", title);
            sectionControls[0][sectionControlCount[0]] =
                NewControl(win, &popupRect, title, true, kCheckBoxProc, 0,
                           0, 0, sectionControlCount[0]++);
            SetControlValue(sectionControls[0][5],
                            GWConfig_Num("log_file", 0) != 0);
            y += 14;

            /* max_connects edit field (TEHandle via CDEF 28) */
            char buf2[8];
            sprintf(buf2, "%ld", GWConfig_Num("max_connects", 8));
            SetRect(&popupRect, x + 140, y, static_cast<short>(x + 200),
                    static_cast<short>(y + 12));
            ToPascal("Max connects", title);
            TEHandle te2 = reinterpret_cast<TEHandle>(NewControl(
                win, &popupRect, title, true, kTextEditProc, 0, 0, 0,
                sectionControlCount[0]++));
            TESetText(te2, reinterpret_cast<Ptr>(buf2),
                      static_cast<short>(strlen(buf2)));
        }

        /* Section 1: Web Proxy (5 controls)
         *   0=http_port edit(TE), 1=follow_redirects pop-up,
         *   2=rewrite_https chk, 3=connect_mitm chk,
         *   4=max_body_mb edit(TE) */
        {
            short y = contentRect.top;
            short x = contentRect.left + 4;

            /* http_port edit field (TEHandle via CDEF 28) */
            char buf[8];
            sprintf(buf, "%ld", GWConfig_Num("http_port", 8765));
            SetRect(&popupRect, x + 140, y, static_cast<short>(x + 200),
                    static_cast<short>(y + 12));
            ToPascal("HTTP port", title);
            TEHandle te = reinterpret_cast<TEHandle>(NewControl(
                win, &popupRect, title, true, kTextEditProc, 0, 0, 0,
                sectionControlCount[1]++));
            TESetText(te, reinterpret_cast<Ptr>(buf),
                      static_cast<short>(strlen(buf)));
            y += 14;

            /* follow_redirects pop-up */
            SetRect(&popupRect, x + 140, y, static_cast<short>(x + 230),
                    static_cast<short>(y + 12));
            ToPascal("", title);
            short sel = 0;
            const char *val = GWConfig_Str("follow_redirects", "auto");
            if (gw_stricmp(val, "always") == 0) sel = 1;
            else if (gw_stricmp(val, "never") == 0) sel = 2;
            sectionControls[1][sectionControlCount[1]] =
                NewControl(win, &popupRect, title, true, kPopUpButtonProc, 0,
                           0, sel, sectionControlCount[1]++);
            y += 14;

            /* rewrite_https checkbox */
            SetRect(&popupRect, x, y, contentRect.right - 4,
                    static_cast<short>(y + 12));
            ToPascal("Rewrite https:// in bodies", title);
            sectionControls[1][sectionControlCount[1]] =
                NewControl(win, &popupRect, title, true, kCheckBoxProc, 0,
                           0, 0, sectionControlCount[1]++);
            SetControlValue(sectionControls[1][2],
                            GWConfig_Num("rewrite_https", 1) != 0);
            y += 14;

            /* connect_mitm checkbox */
            SetRect(&popupRect, x, y, contentRect.right - 4,
                    static_cast<short>(y + 12));
            ToPascal("MITM CONNECT on port 443", title);
            sectionControls[1][sectionControlCount[1]] =
                NewControl(win, &popupRect, title, true, kCheckBoxProc, 0,
                           0, 0, sectionControlCount[1]++);
            SetControlValue(sectionControls[1][3],
                            GWConfig_Num("connect_mitm", 0) != 0);
            y += 14;

            /* max_body_mb edit field (TEHandle via CDEF 28) */
            char buf2[8];
            sprintf(buf2, "%ld", GWConfig_Num("max_body_mb", 0));
            SetRect(&popupRect, x + 140, y, static_cast<short>(x + 200),
                    static_cast<short>(y + 12));
            ToPascal("Max body (MiB)", title);
            TEHandle te2 = reinterpret_cast<TEHandle>(NewControl(
                win, &popupRect, title, true, kTextEditProc, 0, 0, 0,
                sectionControlCount[1]++));
            TESetText(te2, reinterpret_cast<Ptr>(buf2),
                      static_cast<short>(strlen(buf2)));
        }

        /* Section 2: Wayback Proxy (9 controls)
         *   0=wayback_port edit(TE), 1=wayback_date edit(TE),
         *   2=wayback_tolerance edit(TE), 3=text area (skip),
         *   4=scroll bar (skip),
         *   5-8 = wayback_geocities, ct_encoding, settings, cache (chk) */
        {
            short y = contentRect.top;
            short x = contentRect.left + 4;

            /* wayback_port edit field (TEHandle via CDEF 28) */
            char buf[8];
            sprintf(buf, "%ld", GWConfig_Num("wayback_port", 8888));
            SetRect(&popupRect, x + 140, y, static_cast<short>(x + 200),
                    static_cast<short>(y + 12));
            ToPascal("Wayback port", title);
            TEHandle te = reinterpret_cast<TEHandle>(NewControl(
                win, &popupRect, title, true, kTextEditProc, 0, 0, 0,
                sectionControlCount[2]++));
            TESetText(te, reinterpret_cast<Ptr>(buf),
                      static_cast<short>(strlen(buf)));
            y += 14;

            /* wayback_date edit field (TEHandle via CDEF 28) */
            char buf2[9];
            sprintf(buf2, "%ld", GWConfig_Num("wayback_date", 20011231));
            SetRect(&popupRect, x + 140, y, static_cast<short>(x + 230),
                    static_cast<short>(y + 12));
            ToPascal("Wayback date (YYYYMMDD)", title);
            TEHandle te2 = reinterpret_cast<TEHandle>(NewControl(
                win, &popupRect, title, true, kTextEditProc, 0, 0, 0,
                sectionControlCount[2]++));
            TESetText(te2, reinterpret_cast<Ptr>(buf2),
                      static_cast<short>(strlen(buf2)));
            y += 14;

            /* wayback_tolerance edit field (TEHandle via CDEF 28) */
            char buf3[8];
            sprintf(buf3, "%ld", GWConfig_Num("wayback_tolerance", 730));
            SetRect(&popupRect, x + 140, y, static_cast<short>(x + 200),
                    static_cast<short>(y + 12));
            ToPascal("Wayback tolerance (days)", title);
            TEHandle te3 = reinterpret_cast<TEHandle>(NewControl(
                win, &popupRect, title, true, kTextEditProc, 0, 0, 0,
                sectionControlCount[2]++));
            TESetText(te3, reinterpret_cast<Ptr>(buf3),
                      static_cast<short>(strlen(buf3)));
            y += 14;

            /* wayback_geocities checkbox */
            SetRect(&popupRect, x, y, contentRect.right - 4,
                    static_cast<short>(y + 12));
            ToPascal("Redirect geocities.com to oocities.org", title);
            sectionControls[2][sectionControlCount[2]] =
                NewControl(win, &popupRect, title, true, kCheckBoxProc, 0,
                           0, 0, sectionControlCount[2]++);
            SetControlValue(sectionControls[2][5],
                            GWConfig_Num("wayback_geocities", 1) != 0);
            y += 14;

            /* wayback_ct_encoding checkbox */
            SetRect(&popupRect, x, y, contentRect.right - 4,
                    static_cast<short>(y + 12));
            ToPascal("Strip charset from Content-Type", title);
            sectionControls[2][sectionControlCount[2]] =
                NewControl(win, &popupRect, title, true, kCheckBoxProc, 0,
                           0, 0, sectionControlCount[2]++);
            SetControlValue(sectionControls[2][6],
                            GWConfig_Num("wayback_ct_encoding", 1) != 0);
            y += 14;

            /* wayback_settings checkbox */
            SetRect(&popupRect, x, y, contentRect.right - 4,
                    static_cast<short>(y + 12));
            ToPascal("Serve settings page", title);
            sectionControls[2][sectionControlCount[2]] =
                NewControl(win, &popupRect, title, true, kCheckBoxProc, 0,
                           0, 0, sectionControlCount[2]++);
            SetControlValue(sectionControls[2][7],
                            GWConfig_Num("wayback_settings", 1) != 0);
            y += 14;

            /* wayback_cache checkbox */
            SetRect(&popupRect, x, y, contentRect.right - 4,
                    static_cast<short>(y + 12));
            ToPascal("Cache archived responses for a year", title);
            sectionControls[2][sectionControlCount[2]] =
                NewControl(win, &popupRect, title, true, kCheckBoxProc, 0,
                           0, 0, sectionControlCount[2]++);
            SetControlValue(sectionControls[2][8],
                            GWConfig_Num("wayback_cache", 1) != 0);
            y += 14;

            /* wayback_live text area with scroll bar */
            Rect taRect, sbRect;
            SetRect(&taRect, x + 4, y,
                    static_cast<short>(contentRect.right - 20),
                    static_cast<short>(y + 54));
            TEHandle te = reinterpret_cast<TEHandle>(NewControl(
                win, &taRect, title, true, kTextEditProc, 0, 0, 0,
                sectionControlCount[2]++));
            TextFont(4); /* Monaco */
            TextSize(9);

            /* Build the live list text. */
            char liveText[4096];
            liveText[0] = '\0';
            for (int i = 0; ; i++) {
                char val[256];
                if (!GWConfig_GetNth("wayback_live", i, val, sizeof(val))) break;
                if (strlen(val) == 0) continue;
                size_t el = strlen(liveText);
                if (el > 0) {
                    liveText[el] = '\r';
                    el++;
                }
                size_t vlen = strlen(val);
                if (el + vlen >= sizeof(liveText)) break;
                memcpy(liveText + el, val, vlen);
                el += vlen;
            }
            TESetText(te, reinterpret_cast<Ptr>(liveText),
                      static_cast<short>(strlen(liveText)));

            /* Scroll bar for text area */
            SetRect(&sbRect, static_cast<short>(taRect.right + 1),
                    taRect.top, static_cast<short>(taRect.right + kScrollWidth),
                    static_cast<short>(taRect.bottom));
            ToPascal("", title);
            sectionControls[2][sectionControlCount[2]] =
                NewControl(win, &sbRect, title, true, kScrollBarProc, 0,
                           0, 0, sectionControlCount[2]++);
        }

        /* Section 3: Mail (9 controls)
         *   0=imap_port edit(TE), 1=pop_port edit(TE),
         *   2=smtp_port edit(TE), 3=local_password edit(TE),
         *   4=provider pop-up, 5=oauth_user edit(TE),
         *   6=oauth_client_id edit(TE), 7=oauth_client_secret edit(TE),
         *   8=refresh_token edit(TE) */
        {
            short y = contentRect.top;
            short x = contentRect.left + 4;

            /* imap_port edit field (TEHandle via CDEF 28) */
            char buf[8];
            sprintf(buf, "%ld", GWConfig_Num("imap_port", 1993));
            SetRect(&popupRect, x + 140, y, static_cast<short>(x + 200),
                    static_cast<short>(y + 12));
            ToPascal("IMAP port", title);
            TEHandle te = reinterpret_cast<TEHandle>(NewControl(
                win, &popupRect, title, true, kTextEditProc, 0, 0, 0,
                sectionControlCount[3]++));
            TESetText(te, reinterpret_cast<Ptr>(buf),
                      static_cast<short>(strlen(buf)));
            y += 14;

            /* pop_port edit field (TEHandle via CDEF 28) */
            char buf2[8];
            sprintf(buf2, "%ld", GWConfig_Num("pop_port", 1995));
            SetRect(&popupRect, x + 140, y, static_cast<short>(x + 200),
                    static_cast<short>(y + 12));
            ToPascal("POP3 port", title);
            TEHandle te2 = reinterpret_cast<TEHandle>(NewControl(
                win, &popupRect, title, true, kTextEditProc, 0, 0, 0,
                sectionControlCount[3]++));
            TESetText(te2, reinterpret_cast<Ptr>(buf2),
                      static_cast<short>(strlen(buf2)));
            y += 14;

            /* smtp_port edit field (TEHandle via CDEF 28) */
            char buf3[8];
            sprintf(buf3, "%ld", GWConfig_Num("smtp_port", 1587));
            SetRect(&popupRect, x + 140, y, static_cast<short>(x + 200),
                    static_cast<short>(y + 12));
            ToPascal("SMTP port", title);
            TEHandle te3 = reinterpret_cast<TEHandle>(NewControl(
                win, &popupRect, title, true, kTextEditProc, 0, 0, 0,
                sectionControlCount[3]++));
            TESetText(te3, reinterpret_cast<Ptr>(buf3),
                      static_cast<short>(strlen(buf3)));
            y += 14;

            /* local_password edit field (TEHandle via CDEF 28, masked) */
            SetRect(&popupRect, x + 140, y, static_cast<short>(x + 260),
                    static_cast<short>(y + 12));
            ToPascal("Local password", title);
            TEHandle te4 = reinterpret_cast<TEHandle>(NewControl(
                win, &popupRect, title, true, kTextEditProc, 0, 0, 0,
                sectionControlCount[3]++));
            char pwBuf[128];
            if (GWConfig_Str("local_password", "", pwBuf, sizeof(pwBuf))) {
                char masked[128];
                size_t n = strlen(pwBuf);
                if (n >= sizeof(masked)) n = sizeof(masked) - 1;
                for (size_t i = 0; i < n; i++) masked[i] = '*';
                masked[n] = '\0';
                TESetText(te4, reinterpret_cast<Ptr>(masked),
                          static_cast<short>(n));
            } else {
                TESetText(te4, reinterpret_cast<Ptr>(""), 0);
            }
            y += 14;

            /* provider pop-up */
            SetRect(&popupRect, x + 140, y, static_cast<short>(x + 230),
                    static_cast<short>(y + 12));
            ToPascal("", title);
            short sel = 0;
            const char *prov = GWConfig_Str("provider", "outlook");
            if (gw_stricmp(prov, "gmail") == 0) sel = 1;
            else if (gw_stricmp(prov, "custom") == 0) sel = 2;
            sectionControls[3][sectionControlCount[3]] =
                NewControl(win, &popupRect, title, true, kPopUpButtonProc, 0,
                           0, sel, sectionControlCount[3]++);
            y += 14;

            /* oauth_user edit field (TEHandle via CDEF 28) */
            SetRect(&popupRect, x + 140, y, static_cast<short>(x + 260),
                    static_cast<short>(y + 12));
            ToPascal("OAuth user", title);
            TEHandle te5 = reinterpret_cast<TEHandle>(NewControl(
                win, &popupRect, title, true, kTextEditProc, 0, 0, 0,
                sectionControlCount[3]++));
            char userBuf[128];
            if (GWConfig_Str("oauth_user", "", userBuf, sizeof(userBuf)))
                TESetText(te5, reinterpret_cast<Ptr>(userBuf),
                          static_cast<short>(strlen(userBuf)));
            else
                TESetText(te5, reinterpret_cast<Ptr>(""), 0);
            y += 14;

            /* oauth_client_id edit field (TEHandle via CDEF 28) */
            SetRect(&popupRect, x + 140, y, static_cast<short>(x + 260),
                    static_cast<short>(y + 12));
            ToPascal("OAuth client ID", title);
            TEHandle te6 = reinterpret_cast<TEHandle>(NewControl(
                win, &popupRect, title, true, kTextEditProc, 0, 0, 0,
                sectionControlCount[3]++));
            char cidBuf[128];
            if (GWConfig_Str("oauth_client_id", "", cidBuf, sizeof(cidBuf)))
                TESetText(te6, reinterpret_cast<Ptr>(cidBuf),
                          static_cast<short>(strlen(cidBuf)));
            else
                TESetText(te6, reinterpret_cast<Ptr>(""), 0);
            y += 14;

            /* oauth_client_secret edit field (TEHandle via CDEF 28) */
            SetRect(&popupRect, x + 140, y, static_cast<short>(x + 260),
                    static_cast<short>(y + 12));
            ToPascal("OAuth client secret", title);
            TEHandle te7 = reinterpret_cast<TEHandle>(NewControl(
                win, &popupRect, title, true, kTextEditProc, 0, 0, 0,
                sectionControlCount[3]++));
            char csBuf[128];
            if (GWConfig_Str("oauth_client_secret", "", csBuf, sizeof(csBuf)))
                TESetText(te7, reinterpret_cast<Ptr>(csBuf),
                          static_cast<short>(strlen(csBuf)));
            else
                TESetText(te7, reinterpret_cast<Ptr>(""), 0);
            y += 14;

            /* refresh_token edit field (TEHandle via CDEF 28) */
            SetRect(&popupRect, x + 140, y, static_cast<short>(x + 260),
                    static_cast<short>(y + 12));
            ToPascal("Refresh token", title);
            TEHandle te8 = reinterpret_cast<TEHandle>(NewControl(
                win, &popupRect, title, true, kTextEditProc, 0, 0, 0,
                sectionControlCount[3]++));
            char rtBuf[512];
            if (GWConfig_Str("refresh_token", "", rtBuf, sizeof(rtBuf)))
                TESetText(te8, reinterpret_cast<Ptr>(rtBuf),
                          static_cast<short>(strlen(rtBuf)));
            else
                TESetText(te8, reinterpret_cast<Ptr>(""), 0);
        }

        /* ---- Event loop: section switching + OK/Cancel ---- */
        short currentSection = 0;

        /* Hide all sections except 0. */
        for (int s = 1; s < kMaxSections; s++) {
            for (short i = 0; i < sectionControlCount[s]; i++)
                HideControl(sectionControls[s][i]);
        }

        while (!done && !mDone) {
            WaitNextEvent(everyEvent, &event, 5, nullptr);

            switch (event.what) {
            case updateEvt:
                if (reinterpret_cast<WindowPtr>(event.message) == win) {
                    BeginUpdate(win);
                    EraseRect(&contentRect);
                    for (short i = 0; i < sectionControlCount[currentSection]; i++)
                        Draw1Control(sectionControls[currentSection][i]);
                    EndUpdate(win);
                }
                break;

            case mouseDown:
                {
                    WindowPtr win2;
                    short part = FindWindow(event.where, &win2);

                    if (part == inGoAway) {
                        if (TrackGoAway(win, event.where)) done = true;
                    } else if (part == inDrag) {
                        Rect limit = qd.screenBits.bounds;
                        InsetRect(&limit, 4, 4);
                        DragWindow(win, event.where, &limit);
                    } else if (win2 == win) {
                        Point local = event.where;
                        SetPort(reinterpret_cast<GrafPtr>(win));
                        GlobalToLocal(&local);

                        ControlHandle hit;
                        short where = FindControl(local, win, &hit);

                        if (where != 0) {
                            /* Button click: OK or Cancel */
                            if (hit == hOK || hit == hCancel) {
                                done = true;
                            }
                        } else {
                            /* Pop-up button: section change */
                            if (hit == hSection) {
                                short newSel = GetControlValue(hSection);
                                for (short i = 0; i < sectionControlCount[currentSection]; i++)
                                    HideControl(sectionControls[currentSection][i]);
                                currentSection = newSel;
                                for (short i = 0; i < sectionControlCount[currentSection]; i++)
                                    ShowControl(sectionControls[currentSection][i]);
                                Redraw();
                            }
                        }
                    }
                }
                break;

            case keyDown:
            case autoKey: {
                char c = static_cast<char>(event.message & charCodeMask);
                if (c == '\r' || c == 3) {
                    done = true;
                } else if (c == 27) {
                    done = true;
                }
                break;
            }

            default:
                break;
            }
        }

        /* On OK, save all changed settings. */
        if (!done || mDone) {
            /* Cancelled or closed: discard. */
        } else {
            char buf[512];

            /* Section 0: Application (7 controls)
             *   0=show_window chk, 1=max_sessions edit(TE),
             *   2=http_enabled chk, 3=mail_enabled chk,
             *   4=wayback_enabled chk, 5=log_file chk,
             *   6=max_connects edit(TE) */
            {
                GWConfig_Set("show_window",
                             GetControlValue(sectionControls[0][0]) ? "1" : "0");
                /* TEHandle is a struct (TERec), cast to pointer for member access. */
                TEHandle te = reinterpret_cast<TEHandle>(sectionControls[0][1]);
                Byte **tptr = TEGetText(reinterpret_cast<TEPtr>(te));
                char ebuf[8];
                memcpy(ebuf, tptr, te->teLength < 7 ? te->teLength + 1 : 8);
                ebuf[7] = '\0';
                GWConfig_Set("max_sessions", ebuf);
                GWConfig_Set("http_enabled",
                             GetControlValue(sectionControls[0][2]) ? "1" : "0");
                GWConfig_Set("mail_enabled",
                             GetControlValue(sectionControls[0][3]) ? "1" : "0");
                GWConfig_Set("wayback_enabled",
                             GetControlValue(sectionControls[0][4]) ? "1" : "0");
                GWConfig_Set("log_file",
                             GetControlValue(sectionControls[0][5]) ? "1" : "0");
                TEHandle te2 = reinterpret_cast<TEHandle>(sectionControls[0][6]);
                Byte **tptr2 = TEGetText(reinterpret_cast<TEPtr>(te2));
                char ebuf2[8];
                memcpy(ebuf2, tptr2, te2->teLength < 7 ? te2->teLength + 1 : 8);
                ebuf2[7] = '\0';
                GWConfig_Set("max_connects", ebuf2);
            }

            /* Section 1: Web Proxy (5 controls) */
            {
                TEHandle te = reinterpret_cast<TEHandle>(sectionControls[1][0]);
                Byte **tptr = TEGetText(reinterpret_cast<TEPtr>(te));
                char ebuf[8];
                memcpy(ebuf, tptr, te->teLength < 7 ? te->teLength + 1 : 8);
                ebuf[7] = '\0';
                GWConfig_Set("http_port", ebuf);
                {
                    short sel = GetControlValue(sectionControls[1][1]);
                    const char *vals[] = { "auto", "always", "never" };
                    GWConfig_Set("follow_redirects", vals[sel < 3 ? sel : 0]);
                }
                GWConfig_Set("rewrite_https",
                             GetControlValue(sectionControls[1][2]) ? "1" : "0");
                GWConfig_Set("connect_mitm",
                             GetControlValue(sectionControls[1][3]) ? "1" : "0");
                TEHandle te2 = reinterpret_cast<TEHandle>(sectionControls[1][4]);
                Byte **tptr2 = TEGetText(reinterpret_cast<TEPtr>(te2));
                char ebuf2[8];
                memcpy(ebuf2, tptr2, te2->teLength < 7 ? te2->teLength + 1 : 8);
                ebuf2[7] = '\0';
                GWConfig_Set("max_body_mb", ebuf2);
            }

            /* Section 2: Wayback Proxy (9 controls) */
            {
                TEHandle te = reinterpret_cast<TEHandle>(sectionControls[2][0]);
                Byte **tptr = TEGetText(reinterpret_cast<TEPtr>(te));
                char ebuf[8];
                memcpy(ebuf, tptr, te->teLength < 7 ? te->teLength + 1 : 8);
                ebuf[7] = '\0';
                GWConfig_Set("wayback_port", ebuf);
                TEHandle te2 = reinterpret_cast<TEHandle>(sectionControls[2][1]);
                Byte **tptr2 = TEGetText(reinterpret_cast<TEPtr>(te2));
                char ebuf2[9];
                memcpy(ebuf2, tptr2, te2->teLength < 8 ? te2->teLength + 1 : 9);
                ebuf2[8] = '\0';
                GWConfig_Set("wayback_date", ebuf2);
                TEHandle te3 = reinterpret_cast<TEHandle>(sectionControls[2][2]);
                Byte **tptr3 = TEGetText(reinterpret_cast<TEPtr>(te3));
                char ebuf3[8];
                memcpy(ebuf3, tptr3, te3->teLength < 7 ? te3->teLength + 1 : 8);
                ebuf3[7] = '\0';
                GWConfig_Set("wayback_tolerance", ebuf3);
                /* skip text area (3) and scroll bar (4) */
                GWConfig_Set("wayback_geocities",
                             GetControlValue(sectionControls[2][5]) ? "1" : "0");
                GWConfig_Set("wayback_ct_encoding",
                             GetControlValue(sectionControls[2][6]) ? "1" : "0");
                GWConfig_Set("wayback_settings",
                             GetControlValue(sectionControls[2][7]) ? "1" : "0");
                GWConfig_Set("wayback_cache",
                             GetControlValue(sectionControls[2][8]) ? "1" : "0");
            }

            /* Section 3: Mail (9 controls) */
            {
                TEHandle te = reinterpret_cast<TEHandle>(sectionControls[3][0]);
                Byte **tptr = TEGetText(reinterpret_cast<TEPtr>(te));
                char ebuf[8];
                memcpy(ebuf, tptr, te->teLength < 7 ? te->teLength + 1 : 8);
                ebuf[7] = '\0';
                GWConfig_Set("imap_port", ebuf);
                TEHandle te2 = reinterpret_cast<TEHandle>(sectionControls[3][1]);
                Byte **tptr2 = TEGetText(reinterpret_cast<TEPtr>(te2));
                char ebuf2[8];
                memcpy(ebuf2, tptr2, te2->teLength < 7 ? te2->teLength + 1 : 8);
                ebuf2[7] = '\0';
                GWConfig_Set("pop_port", ebuf2);
                TEHandle te3 = reinterpret_cast<TEHandle>(sectionControls[3][2]);
                Byte **tptr3 = TEGetText(reinterpret_cast<TEPtr>(te3));
                char ebuf3[8];
                memcpy(ebuf3, tptr3, te3->teLength < 7 ? te3->teLength + 1 : 8);
                ebuf3[7] = '\0';
                GWConfig_Set("smtp_port", ebuf3);
                TEHandle te4 = reinterpret_cast<TEHandle>(sectionControls[3][3]);
                Byte **tptr4 = TEGetText(reinterpret_cast<TEPtr>(te4));
                char ebuf4[128];
                memcpy(ebuf4, tptr4, te4->teLength < 127 ? te4->teLength + 1 : 128);
                ebuf4[127] = '\0';
                GWConfig_Set("local_password", ebuf4);
                {
                    short sel = GetControlValue(sectionControls[3][4]);
                    const char *vals[] = { "outlook", "gmail", "custom" };
                    GWConfig_Set("provider", vals[sel < 3 ? sel : 0]);
                }
                TEHandle te5 = reinterpret_cast<TEHandle>(sectionControls[3][5]);
                Byte **tptr5 = TEGetText(reinterpret_cast<TEPtr>(te5));
                char ebuf5[128];
                memcpy(ebuf5, tptr5, te5->teLength < 127 ? te5->teLength + 1 : 128);
                ebuf5[127] = '\0';
                GWConfig_Set("oauth_user", ebuf5);
                TEHandle te6 = reinterpret_cast<TEHandle>(sectionControls[3][6]);
                Byte **tptr6 = TEGetText(reinterpret_cast<TEPtr>(te6));
                char ebuf6[128];
                memcpy(ebuf6, tptr6, te6->teLength < 127 ? te6->teLength + 1 : 128);
                ebuf6[127] = '\0';
                GWConfig_Set("oauth_client_id", ebuf6);
                TEHandle te7 = reinterpret_cast<TEHandle>(sectionControls[3][7]);
                Byte **tptr7 = TEGetText(reinterpret_cast<TEPtr>(te7));
                char ebuf7[128];
                memcpy(ebuf7, tptr7, te7->teLength < 127 ? te7->teLength + 1 : 128);
                ebuf7[127] = '\0';
                GWConfig_Set("oauth_client_secret", ebuf7);
                TEHandle te8 = reinterpret_cast<TEHandle>(sectionControls[3][8]);
                Byte **tptr8 = TEGetText(reinterpret_cast<TEPtr>(te8));
                char ebuf8[512];
                memcpy(ebuf8, tptr8, te8->teLength < 511 ? te8->teLength + 1 : 512);
                ebuf8[511] = '\0';
                GWConfig_Set("refresh_token", ebuf8);
            }

            /* Flush prefs to disk. */
            GWConfig_Load();  /* re-read after saves */
        }

        /* Dispose all controls. */
        for (int s = 0; s < kMaxSections; s++) {
            for (short i = 0; i < sectionControlCount[s]; i++) {
                /* The wayback text area (section 2, index 3) is a TEHandle:
                 * dispose it with TEDispose. */
                if (s == 2 && i == 3)
                    TEDispose(sectionControls[2][3]);
                else
                    DisposeControl(sectionControls[s][i]);
            }
        }

        DisposeWindow(win);
    }    void DrawAboutContent(WindowPtr w)
    {
        GrafPtr  port = reinterpret_cast<GrafPtr>(w);
        Rect     box = port->portRect;
        Rect     iconRect;
        Str255   fontName;
        RGBColor platinum, black;
        short    midX, charcoal;

        SetPort(port);

        platinum.red = platinum.green = platinum.blue = kPlatinum;
        black.red = black.green = black.blue = 0;

        RGBBackColor(&platinum);
        RGBForeColor(&platinum);
        PaintRect(&box);
        RGBForeColor(&black);

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
                            "Gateway " GW_VERSION_STRING);

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

    /*
     * The item names what the next click will do, so it has to follow the
     * window rather than be set once at startup: "Hide Window" while one is
     * showing, "Show Window" while none is.
     *
     * Plain text with no "/H" on the end. AppendMenu reads that as a
     * command-key metacharacter when the item is created, but SetMenuItemText
     * takes the string literally and would put the characters in the menu.
     * The command key set at creation survives a text change.
     */
    void UpdateWindowMenuItem()
    {
        Str255 title;

        if (mFileMenu == nullptr) return;
        ToPascal(mWindow != nullptr ? "Hide Window" : "Show Window", title);
        SetMenuItemText(mFileMenu, kHideItem, title);

        /* Same rule for the gateway itself: the item says what a click does. */
        ToPascal(GW_IsRunning() ? "Stop Gateway" : "Start Gateway", title);
        SetMenuItemText(mFileMenu, kStopItem, title);
    }

    /*
     * Release the ports, or bind them again. The application stays up either
     * way, so the log remains readable and the settings can be corrected
     * before starting again -- which is the point of stopping rather than
     * quitting.
     */
    void ToggleRunning()
    {
        if (GW_IsRunning()) {
            GW_Stop();
        } else if (!GW_Start()) {
            gw_log("could not start: the ports may still be in use");
        }
        UpdateWindowMenuItem();
        Redraw();
    }

    /* Put the window away, or bring it back. Either way Gateway keeps
     * proxying; only the display stops. */
    void ToggleWindow()
    {
        bool showing;

        if (mWindow != nullptr) {
            DisposeWindow(mWindow);     /* takes its controls with it */
            mWindow = nullptr;
            mScroll = nullptr;
            showing = false;
        } else {
            SetUpWindow();
            Redraw();
            showing = true;
        }
        UpdateWindowMenuItem();

        /*
         * Remember the choice for next time, but change nothing else about
         * this session. The menu bar stays exactly where it is, so Quit and
         * Show Window are always one click away -- hiding a window must never
         * be the thing that makes an application unreachable.
         *
         * Going faceless is a launch-time decision, applied in Start().
         */
        GW_SetShowWindowPref(showing ? 1 : 0);
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

            if (event.modifiers & cmdKey) {
                HandleMenu(MenuKey(ch));
                break;
            }

            /* The log scrolls from the keyboard: the Control Manager is not
             * in the Multiversal Interfaces, so there are no scroll bars to
             * hang this off. */
            switch (ch) {
            case 0x1E: Scroll(1);                  break;  /* up arrow    */
            case 0x1F: Scroll(-1);                 break;  /* down arrow  */
            case 0x0B: Scroll(VisibleRows() - 1);  break;  /* page up     */
            case 0x0C: Scroll(-(VisibleRows() - 1)); break; /* page down  */
            case 0x01: Scroll(GW_LOG_LINES);       break;  /* home        */
            case 0x04: Scroll(-GW_LOG_LINES);      break;  /* end         */
            default:   break;
            }
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

        case inGrow: {
            Rect limit;
            long size;

            SetRect(&limit, 260, 120,
                    static_cast<short>(qd.screenBits.bounds.right),
                    static_cast<short>(qd.screenBits.bounds.bottom));
            size = GrowWindow(win, event.where, &limit);
            if (size != 0) {
                /* GrowWindow packs height above width, and HiWord/LoWord are
                 * 68K trap glue InterfaceLib does not export to PowerPC. */
                SizeWindow(win, static_cast<short>(size & 0xFFFF),
                           static_cast<short>((size >> 16) & 0xFFFF), true);
                LayoutScroll();
                Redraw();
            }
            break;
        }

        case inZoomIn:
        case inZoomOut:
            if (TrackBox(win, event.where, part)) {
                SetPort(reinterpret_cast<GrafPtr>(win));
                ZoomWindow(win, part, true);
                LayoutScroll();
                Redraw();
            }
            break;

        case inContent: {
            Point         local;
            ControlHandle hit = nullptr;
            short         where;

            if (win != FrontWindow()) {
                SelectWindow(win);
                break;
            }
            if (win != mWindow || mScroll == nullptr) break;

            local = event.where;
            SetPort(reinterpret_cast<GrafPtr>(mWindow));
            GlobalToLocal(&local);

            where = FindControl(local, win, &hit);
            if (hit != mScroll) break;

            if (where == kInThumb) {
                /* The Control Manager drags the thumb; read where it landed. */
                if (TrackControl(hit, local, nullptr) == kInThumb) {
                    int most = GetControlMaximum(mScroll);
                    mScrollBack = most - GetControlValue(mScroll);
                    Redraw();
                }
            } else if (where != 0) {
                if (TrackControl(hit, local, nullptr) == where)
                    ScrollByPart(where);
            }
            break;
        }

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
            else if (item == kStopItem) ToggleRunning();
            else if (item == kSettingsItem) HandleSettings();
            else if (item == kQuitItem) mDone = true;
            break;

        default:
            break;
        }
        HiliteMenu(0);
    }

    int VisibleRows() const
    {
        Rect area;
        int  rows;

        if (mWindow == nullptr) return 1;
        area = reinterpret_cast<GrafPtr>(mWindow)->portRect;
        rows = (area.bottom - 2 - kHeaderRows * kLineHeight) / kLineHeight;
        return rows > 1 ? rows : 1;
    }

    /* Move the view through the log. Positive is towards older lines. */
    void Scroll(int lines)
    {
        int count = GW_LogCount();
        int rows = VisibleRows();
        int most = count - rows;

        if (most < 0) most = 0;

        mScrollBack += lines;
        if (mScrollBack > most) mScrollBack = most;
        if (mScrollBack < 0) mScrollBack = 0;
        Redraw();
    }

    void Redraw()
    {
        if (mWindow == nullptr) return;
        SetPort(reinterpret_cast<GrafPtr>(mWindow));
        DrawContents();
        mSeenGeneration = GW_LogGeneration();
    }

    /* How many rows a log line needs once wrapped to `cols` columns. */
    static int RowsFor(const char *text, int cols)
    {
        int len = static_cast<int>(strlen(text));

        if (cols < 1) return 1;
        if (len < 1) return 1;
        return (len + cols - 1) / cols;
    }

    void DrawContents()
    {
        GrafPtr port = reinterpret_cast<GrafPtr>(mWindow);
        Rect    area = port->portRect;
        Rect    textArea, grow;
        short   v;
        int     count, first, i, rows, used, cols, cw;

        SetPort(port);
        EraseRect(&area);

        /*
         * DrawGrowIcon draws the grow box *and* the lines that delimit where a
         * window's scroll bars sit -- including one across the whole bottom
         * edge, which on a window with no horizontal scroll bar is just a
         * black line. Clipping to the corner leaves the grow box alone.
         */
        grow = area;
        grow.left = static_cast<short>(grow.right - kScrollWidth);
        grow.top  = static_cast<short>(grow.bottom - kScrollWidth);
        ClipRect(&grow);
        DrawGrowIcon(mWindow);

        /* Keep the log clear of the scroll bar rather than drawing under it. */
        textArea = area;
        textArea.right = static_cast<short>(textArea.right - kScrollWidth);
        ClipRect(&textArea);

        TextFont(4);           /* Monaco: the log needs a fixed pitch */
        TextSize(9);

        cw = CharWidth('0');   /* Monaco is monospaced, so any glyph will do */
        if (cw < 1) cw = 6;
        cols = (textArea.right - kTextLeft - 2) / cw;
        if (cols < 8) cols = 8;

        rows = (area.bottom - 2) / kLineHeight;
        if (rows < 1) rows = 1;
        count = GW_LogCount();

        /*
         * Wrapped lines are taller than one row, so which line starts the page
         * cannot be found by subtracting. Walk back from the newest line the
         * scroll position asks for, adding up the rows each one needs, and
         * stop when the window is full.
         */
        first = count - mScrollBack;
        if (first > count) first = count;
        if (first < 0) first = 0;

        used = 0;
        while (first > 0) {
            const char *text = GW_LogLine(first - 1);
            int need = (text == nullptr) ? 1 : RowsFor(text, cols);

            if (used + need > rows) break;
            used += need;
            first--;
        }
        mShownLines = count - mScrollBack - first;
        if (mShownLines < 1) mShownLines = 1;

        v = 0;
        for (i = first; i < count; i++) {
            const char *text = GW_LogLine(i);
            int len, off;

            if (text == nullptr) break;
            len = static_cast<int>(strlen(text));
            off = 0;

            /* Break the line at the window edge instead of running under it. */
            do {
                int n = len - off;

                if (n > cols) n = cols;
                v = static_cast<short>(v + kLineHeight);
                if (v > area.bottom - 2) { i = count; break; }
                MoveTo(kTextLeft, v);
                if (n > 0) DrawText(const_cast<char *>(text), off, n);
                off += (n > 0) ? n : 1;
            } while (off < len);
        }

        ClipRect(&area);

        UpdateScroll();
        if (mScroll != nullptr) Draw1Control(mScroll);
    }

    /* How many lines back from the newest the log is scrolled. */
    int           mScrollBack;

    ControlHandle mScroll;
    /* Log lines the last redraw actually fitted, wrapping included. */
    int           mShownLines;
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
