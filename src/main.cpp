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
const short kQuitItem  = 5;  /* 4 is the separator */

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

/*
 * Control procedure IDs. Multiversal does not export the classic names, so
 * they are written out by value:
 *
 *   pushButProc   0    CDEF 0, variant 0
 *   checkBoxProc  1    CDEF 0, variant 1
 *   popupMenuProc 1008 CDEF 63, plus popupFixedWidth (1) so the box keeps the
 *                      width it was given rather than sizing to its widest item
 *
 * There is no entry-field CDEF before the Appearance Manager, which is why
 * the settings window's fields are TextEdit records instead.
 */
const short kButtonProc      = 0;
const short kCheckBoxProc    = 1;
const short kPopUpProc       = 1009;

/* movableDBoxProc: a dialog frame with a drag bar and no close or zoom box. */
const short kMovableDBoxProc = 5;

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

/*
 * The settings window's fields.
 *
 * One row per preference: which pane it belongs to, how it is edited, the
 * key it reads and writes, and the default it falls back to. The table is a
 * plain aggregate of scalars and string literals, so it is laid down by the
 * linker and needs no constructor -- Retro68's PowerPC crt0 would not run one
 * (CLAUDE.md rule 2).
 *
 * wayback_live is not here. It is a repeated key, and the writer that could
 * save one was removed in fa64f91; a field that cannot be saved is worse than
 * no field. refresh_token is not here either: CLAUDE.md has it arriving
 * out-of-band in its own file, and it is several hundred characters wide.
 */
enum {
    kSecApp = 0,
    kSecWeb,
    kSecWayback,
    kSecMail,
    kSectionCount
};

enum {
    kFieldCheck = 0,   /* checkbox, written as "1" or "0" */
    kFieldNum,         /* entry field holding a number */
    kFieldText,        /* entry field holding a string */
    kFieldPopup        /* pop-up over a fixed list of values */
};

/* MENU resources in src/ui/gateway_settings.r. Numbered clear of the menu
 * bar's 128 and 129, which are in the menu list the whole time. */
const short kSectionMenuID  = 200;
const short kRedirectMenuID = 201;
const short kProviderMenuID = 202;

struct SettingsField {
    short       section;
    short       kind;
    short       menuID;       /* kFieldPopup */
    short       width;        /* entry field or pop-up width, in pixels */
    long        numDefault;   /* kFieldCheck, kFieldNum */
    const char *key;
    const char *label;
    const char *textDefault;  /* kFieldText, kFieldPopup */
    const char *choices[4];   /* kFieldPopup, null terminated */
};

const SettingsField kSettingsFields[] = {
    /* Application */
    { kSecApp, kFieldCheck, 0,   0,       1, "show_window",
      "Show the window at launch", nullptr, { nullptr } },
    { kSecApp, kFieldCheck, 0,   0,       1, "http_enabled",
      "Run the web proxy", nullptr, { nullptr } },
    { kSecApp, kFieldCheck, 0,   0,       1, "mail_enabled",
      "Run the mail splice", nullptr, { nullptr } },
    { kSecApp, kFieldCheck, 0,   0,       1, "wayback_enabled",
      "Run the Wayback proxy", nullptr, { nullptr } },
    { kSecApp, kFieldCheck, 0,   0,       0, "log_file",
      "Write the log to a file", nullptr, { nullptr } },
    { kSecApp, kFieldNum,   0,  56,      12, "max_sessions",
      "Sessions:", nullptr, { nullptr } },
    { kSecApp, kFieldNum,   0,  56,       8, "max_connects",
      "Connections:", nullptr, { nullptr } },

    /* Web proxy */
    { kSecWeb, kFieldNum,   0,  56,    8765, "http_port",
      "Port:", nullptr, { nullptr } },
    { kSecWeb, kFieldPopup, kRedirectMenuID, 100, 0, "follow_redirects",
      "Follow redirects:", "auto", { "auto", "always", "never", nullptr } },
    { kSecWeb, kFieldCheck, 0,   0,       1, "rewrite_https",
      "Rewrite https:// in page text", nullptr, { nullptr } },
    { kSecWeb, kFieldCheck, 0,   0,       0, "connect_mitm",
      "Terminate TLS for CONNECT to port 443", nullptr, { nullptr } },
    { kSecWeb, kFieldNum,   0,  56,       0, "max_body_mb",
      "Body ceiling (MB, 0 = none):", nullptr, { nullptr } },

    /* Wayback proxy */
    { kSecWayback, kFieldNum, 0,  56,    8888, "wayback_port",
      "Port:", nullptr, { nullptr } },
    { kSecWayback, kFieldNum, 0,  72, 20011231, "wayback_date",
      "Date (YYYYMMDD):", nullptr, { nullptr } },
    { kSecWayback, kFieldNum, 0,  56,     730, "wayback_tolerance",
      "Tolerance (days):", nullptr, { nullptr } },
    { kSecWayback, kFieldCheck, 0, 0,       1, "wayback_geocities",
      "Send geocities.com to oocities.org", nullptr, { nullptr } },
    { kSecWayback, kFieldCheck, 0, 0,       1, "wayback_ct_encoding",
      "Strip the charset from Content-Type", nullptr, { nullptr } },
    { kSecWayback, kFieldCheck, 0, 0,       1, "wayback_settings",
      "Serve the settings page", nullptr, { nullptr } },
    { kSecWayback, kFieldCheck, 0, 0,       1, "wayback_cache",
      "Cache archived responses", nullptr, { nullptr } },

    /* Mail */
    { kSecMail, kFieldNum,   0,  56,    1993, "imap_port",
      "IMAP port:", nullptr, { nullptr } },
    { kSecMail, kFieldNum,   0,  56,    1995, "pop_port",
      "POP port:", nullptr, { nullptr } },
    { kSecMail, kFieldNum,   0,  56,    1587, "smtp_port",
      "SMTP port:", nullptr, { nullptr } },
    { kSecMail, kFieldPopup, kProviderMenuID, 100, 0, "provider",
      "Provider:", "outlook", { "outlook", "gmail", "custom", nullptr } },
    { kSecMail, kFieldText,  0, 200,       0, "local_password",
      "Local password:", "", { nullptr } },
    { kSecMail, kFieldText,  0, 200,       0, "oauth_user",
      "Account:", "", { nullptr } },
    { kSecMail, kFieldText,  0, 200,       0, "oauth_client_id",
      "Client ID:", "", { nullptr } },
    { kSecMail, kFieldText,  0, 200,       0, "oauth_client_secret",
      "Client secret:", "", { nullptr } }
};

const short kSettingsFieldCount =
    static_cast<short>(sizeof(kSettingsFields) / sizeof(kSettingsFields[0]));

/* The live half of a field: whichever of a control, a TextEdit record and a
 * menu it turned out to need, plus the rectangle its label is measured from. */
struct SettingsCtl {
    ControlHandle ctl;
    TEHandle      te;
    MenuHandle    menu;
    Rect          box;
    short         sel;    /* kFieldPopup: the chosen item, 1-based */
};

/* The largest value the window will carry in or out of a field. */
const short kSettingsValueMax = 256;

/*
 * The text of an entry field, as a C string.
 *
 * TEGetText hands back the record's own text handle rather than a copy, so
 * the length comes from teLength and not from a terminator -- there is none.
 */
void SettingsFieldText(TEHandle te, char *out, size_t cap)
{
    Handle h;
    long   n;

    if (cap == 0) return;
    out[0] = '\0';
    if (te == nullptr) return;

    n = (*te)->teLength;
    if (n < 0) n = 0;
    if (static_cast<size_t>(n) > cap - 1) n = static_cast<long>(cap - 1);

    h = reinterpret_cast<Handle>(TEGetText(te));
    if (h == nullptr || *h == nullptr) return;

    HLock(h);
    std::memcpy(out, *h, static_cast<size_t>(n));
    HUnlock(h);
    out[n] = '\0';
}

/*
 * Put up one pop-up and return the item chosen, or 0 if the menu was
 * dismissed.
 *
 * CDEF 63 draws a pop-up correctly and will not track a click -- that cost a
 * night once already -- so the control is left to draw and the menu is put up
 * by hand at the top left of the box the control drew.
 */
short TrackSettingsPopUp(WindowPtr win, ControlHandle ctl, MenuHandle menu,
                         short menuID, const Rect &box, short current)
{
    Point corner;
    long  chosen;
    short picked;

    if (ctl == nullptr || menu == nullptr) return 0;

    /* The box is passed in rather than read out of the control record: the
     * rectangle is already known here, and nothing else in this file reaches
     * inside a ControlHandle. */
    SetPort(reinterpret_cast<GrafPtr>(win));
    corner.h = box.left;
    corner.v = box.top;
    LocalToGlobal(&corner);

    InsertMenu(menu, -1);          /* -1 is the hierarchical portion */
    CalcMenuSize(menu);
    chosen = PopUpMenuSelect(menu, corner.v, corner.h, current);
    DeleteMenu(menuID);

    picked = static_cast<short>(chosen & 0xFFFF);
    if (picked <= 0) return 0;

    SetControlValue(ctl, picked);
    Draw1Control(ctl);
    return picked;
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
            ToPascal("Settings...", title);
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
     * Settings.
     *
     * One window with four panes, chosen by a pop-up at the top left. The
     * controls are the classic ones -- checkBoxProc for the checkboxes,
     * pushButProc for OK and Cancel, popupMenuProc for the pop-ups -- and the
     * entry fields are TextEdit records rather than controls, because the
     * Control Manager has no entry field before the Appearance Manager and
     * Multiversal does not carry the Appearance Manager.
     *
     * That last point is worth keeping in view: this is a System 7 rendering
     * of a Mac OS 9 panel. It works, and it will not match the Internet
     * control panel pixel for pixel. Matching that means compiling a separate
     * translation unit against the Universal Interfaces in
     * third_party/InterfacesAndLibraries and linking AppearanceLib, the way
     * CLAUDE.md rule 3 already does for Open Transport.
     */
    void HandleSettings()
    {
        const short kWidth    = 420;
        const short kHeight   = 300;
        const short kLabelCol = 150;   /* entry fields start a third across */
        const short kRowStep  = 20;

        Rect         bounds, contentRect, sectionRect, okRect, cancelRect;
        Str255       title;
        WindowPtr    win;
        EventRecord  event;
        GrafPtr      savePort;
        SettingsCtl  item[kSettingsFieldCount];
        short        rowY[kSectionCount];
        ControlHandle hSection, hOK, hCancel;
        MenuHandle   mSection;
        Boolean      done = false;
        Boolean      accept = false;
        short        section = kSecApp;
        short        focus = -1;
        short        i, left, top;

        left = static_cast<short>((qd.screenBits.bounds.right -
                                   qd.screenBits.bounds.left - kWidth) / 2);
        top = static_cast<short>((qd.screenBits.bounds.bottom -
                                  qd.screenBits.bounds.top - kHeight) / 3);
        if (top < 44) top = 44;
        SetRect(&bounds, left, top,
                static_cast<short>(left + kWidth),
                static_cast<short>(top + kHeight));

        /*
         * NewWindow makes a monochrome GrafPort, which silently discards
         * RGBForeColor and leaves the Platinum ground white. NewCWindow is
         * what the About window uses and what this needs too.
         */
        ToPascal("Settings", title);
        win = NewCWindow(nullptr, &bounds, title, false,
                         kMovableDBoxProc, reinterpret_cast<WindowPtr>(-1L),
                         false, 0);
        if (win == nullptr) return;

        GetPort(&savePort);
        SetPort(reinterpret_cast<GrafPtr>(win));

        {
            RGBColor platinum;
            platinum.red = platinum.green = platinum.blue = kPlatinum;
            RGBBackColor(&platinum);
        }

        /* TENew copies the port's face into the record, so the font has to be
         * set before any field is made, not after. */
        TextFont(kFontGeneva);
        TextSize(9);

        /* Everything below is in the window's local coordinates. */
        SetRect(&contentRect, 16, 42,
                static_cast<short>(kWidth - 16),
                static_cast<short>(kHeight - 48));
        SetRect(&okRect, static_cast<short>(kWidth - 86),
                static_cast<short>(kHeight - 36),
                static_cast<short>(kWidth - 16),
                static_cast<short>(kHeight - 16));
        SetRect(&cancelRect, static_cast<short>(kWidth - 166),
                static_cast<short>(kHeight - 36),
                static_cast<short>(kWidth - 96),
                static_cast<short>(kHeight - 16));

        /* The section pop-up. Measure the widest menu item and size to fit. */
        mSection = GetMenu(kSectionMenuID);
        {
            short maxW = 0;
            short nItems = GetMenuItems(mSection);
            Str255 itemStr;
            short j;

            for (j = 1; j <= nItems; j++) {
                GetMenuItemText(mSection, j, itemStr);
                {
                    short w = StringWidth(itemStr);
                    if (w > maxW) maxW = w;
                }
            }
            /* Add a bit of padding for the button frame. */
            maxW += 20;
            SetRect(&sectionRect, 14, 10, static_cast<short>(14 + maxW), 28);
        }
        ToPascal("", title);
        hSection = NewControl(win, &sectionRect, title, false,
                              0, kSectionMenuID, 0, kPopUpProc, 0);
        if (hSection != nullptr) SetControlValue(hSection, 1);

        ToPascal("OK", title);
        hOK = NewControl(win, &okRect, title, false, 0, 0, 1, kButtonProc, 0);
        ToPascal("Cancel", title);
        hCancel = NewControl(win, &cancelRect, title, false,
                             0, 0, 1, kButtonProc, 0);

        /* ---- Build every pane, all of them hidden ---- */
        for (i = 0; i < kSectionCount; i++) rowY[i] = contentRect.top;

        for (i = 0; i < kSettingsFieldCount; i++) {
            const SettingsField *f = &kSettingsFields[i];
            short s = f->section;
            short y = rowY[s];
            char  value[kSettingsValueMax];

            item[i].ctl  = nullptr;
            item[i].te   = nullptr;
            item[i].menu = nullptr;
            item[i].sel  = 1;
            SetRect(&item[i].box, 0, 0, 0, 0);

            switch (f->kind) {
            case kFieldCheck:
                SetRect(&item[i].box, contentRect.left, y,
                        contentRect.right, static_cast<short>(y + 16));
                ToPascal(f->label, title);
                item[i].ctl = NewControl(win, &item[i].box, title, false,
                                         0, 0, 1, kCheckBoxProc, 0);
                if (item[i].ctl != nullptr)
                    SetControlValue(item[i].ctl,
                                    GWConfig_Num(f->key, f->numDefault) != 0);
                break;

            case kFieldPopup: {
                short n;
                const char *cur = GWConfig_Str(f->key, f->textDefault);

                for (n = 0; n < 3 && f->choices[n] != nullptr; n++)
                    if (gw_stricmp(cur, f->choices[n]) == 0)
                        item[i].sel = static_cast<short>(n + 1);

                SetRect(&item[i].box,
                        static_cast<short>(contentRect.left + kLabelCol), y,
                        static_cast<short>(contentRect.left + kLabelCol +
                                           f->width),
                        static_cast<short>(y + 18));
                ToPascal("", title);
                item[i].ctl = NewControl(win, &item[i].box, title, false,
                                         0, f->menuID, 0, kPopUpProc, 0);
                item[i].menu = GetMenu(f->menuID);
                if (item[i].ctl != nullptr)
                    SetControlValue(item[i].ctl, item[i].sel);
                break;
            }

            case kFieldNum:
            case kFieldText: {
                Rect dest, view;
                short fieldLeft;

                /* Application panel: fields are left-aligned (no label column). */
                if (s == kSecApp) {
                    fieldLeft = contentRect.left + 70;
                } else {
                    fieldLeft = contentRect.left + kLabelCol;
                }

                if (f->kind == kFieldNum) {
                    sprintf(value, "%ld", GWConfig_Num(f->key, f->numDefault));
                } else {
                    const char *cur = GWConfig_Str(f->key, f->textDefault);
                    std::strncpy(value, cur, sizeof(value) - 1);
                    value[sizeof(value) - 1] = '\0';
                }

                SetRect(&item[i].box,
                        fieldLeft, y,
                        static_cast<short>(fieldLeft + f->width),
                        static_cast<short>(y + 16));
                view = item[i].box;
                InsetRect(&view, 3, 2);
                dest = view;
                item[i].te = TENew(&dest, &view);
                if (item[i].te != nullptr)
                    TESetText(value, static_cast<long>(std::strlen(value)),
                              item[i].te);
                break;
            }

            default:
                break;
            }

            rowY[s] = static_cast<short>(y + kRowStep);
        }

        /* ---- Show the first pane and open the window ---- */
        for (i = 0; i < kSettingsFieldCount; i++)
            if (kSettingsFields[i].section == section &&
                item[i].ctl != nullptr)
                ShowControl(item[i].ctl);
        if (hSection != nullptr) ShowControl(hSection);
        if (hOK != nullptr) ShowControl(hOK);
        if (hCancel != nullptr) ShowControl(hCancel);

        for (i = 0; i < kSettingsFieldCount; i++) {
            if (kSettingsFields[i].section != section) continue;
            if (item[i].te == nullptr) continue;
            focus = i;
            TEActivate(item[i].te);
            break;
        }

        ShowWindow(win);
        SelectWindow(win);

        /* ---- Run it ---- */
        while (!done && !mDone) {
            WaitNextEvent(everyEvent, &event, 5, nullptr);

            /* Anything drawn behind this window leaves the port on itself, so
             * the port is claimed again before the caret is blinked. */
            SetPort(reinterpret_cast<GrafPtr>(win));

            /* The gateway keeps running while this is up: one cooperative
             * slice per pass, the same as the main loop (CLAUDE.md rule 6). */
            GW_Poll();

            if (focus >= 0 && item[focus].te != nullptr)
                TEIdle(item[focus].te);

            switch (event.what) {
            case updateEvt: {
                /* An update event that is never answered comes back on every
                 * pass, so the log window behind this one is redrawn here
                 * rather than left to spin the loop. */
                WindowPtr hit = reinterpret_cast<WindowPtr>(event.message);

                BeginUpdate(hit);
                if (hit == win) {
                    SetPort(reinterpret_cast<GrafPtr>(win));
                    DrawSettingsPane(win, item, section, contentRect,
                                     kLabelCol, okRect);
                } else if (hit == mWindow) {
                    DrawContents();
                }
                EndUpdate(hit);
                break;
            }

            case mouseDown: {
                WindowPtr     hitWin;
                Point         local;
                ControlHandle hit;
                short         part;

                part = FindWindow(event.where, &hitWin);
                if (hitWin != win) { SysBeep(1); break; }

                if (part == inDrag) {
                    Rect limit = qd.screenBits.bounds;
                    InsetRect(&limit, 4, 4);
                    DragWindow(win, event.where, &limit);
                    break;
                }
                if (part != inContent) break;

                local = event.where;
                SetPort(reinterpret_cast<GrafPtr>(win));
                GlobalToLocal(&local);

                /* Pop-ups first: their CDEF does not hit-test a click the way
                 * FindControl expects, so they are matched by rectangle. */
                if (PtInRect(local, &sectionRect)) {
                    short picked = TrackSettingsPopUp(
                        win, hSection, mSection, kSectionMenuID, sectionRect,
                        static_cast<short>(section + 1));
                    if (picked > 0 && picked - 1 != section) {
                        for (i = 0; i < kSettingsFieldCount; i++) {
                            if (kSettingsFields[i].section != section) continue;
                            if (item[i].ctl != nullptr)
                                HideControl(item[i].ctl);
                            if (item[i].te != nullptr)
                                TEDeactivate(item[i].te);
                        }
                        section = static_cast<short>(picked - 1);
                        focus = -1;
                        for (i = 0; i < kSettingsFieldCount; i++) {
                            if (kSettingsFields[i].section != section) continue;
                            if (item[i].ctl != nullptr)
                                ShowControl(item[i].ctl);
                            if (focus < 0 && item[i].te != nullptr) {
                                focus = i;
                                TEActivate(item[i].te);
                            }
                        }
                        InvalRect(&contentRect);
                    }
                    break;
                }

                {
                    Boolean handled = false;

                    for (i = 0; i < kSettingsFieldCount; i++) {
                        if (kSettingsFields[i].section != section) continue;
                        if (kSettingsFields[i].kind != kFieldPopup) continue;
                        if (!PtInRect(local, &item[i].box)) continue;
                        {
                            short picked = TrackSettingsPopUp(
                                win, item[i].ctl, item[i].menu,
                                kSettingsFields[i].menuID, item[i].box,
                                item[i].sel);
                            if (picked > 0) item[i].sel = picked;
                        }
                        handled = true;
                        break;
                    }
                    if (handled) break;

                    /* Then the entry fields, which are not controls. */
                    for (i = 0; i < kSettingsFieldCount; i++) {
                        if (kSettingsFields[i].section != section) continue;
                        if (item[i].te == nullptr) continue;
                        if (!PtInRect(local, &item[i].box)) continue;
                        if (focus != i) {
                            if (focus >= 0 && item[focus].te != nullptr)
                                TEDeactivate(item[focus].te);
                            focus = i;
                            TEActivate(item[i].te);
                        }
                        TEClick(local, false, item[i].te);
                        handled = true;
                        break;
                    }
                    if (handled) break;
                }

                hit = nullptr;
                if (FindControl(local, win, &hit) != 0 && hit != nullptr) {
                    if (hit == hOK) {
                        if (TrackControl(hit, local, nullptr) != 0) {
                            accept = true;
                            done = true;
                        }
                    } else if (hit == hCancel) {
                        if (TrackControl(hit, local, nullptr) != 0)
                            done = true;
                    } else {
                        TrackControl(hit, local, nullptr);
                    }
                }
                break;
            }

            case keyDown:
            case autoKey: {
                char ch = static_cast<char>(event.message & charCodeMask);

                if ((event.modifiers & cmdKey) != 0) {
                    if (ch == '.') done = true;
                    break;
                }

                if (ch == '\r' || ch == 3) {          /* Return, Enter */
                    accept = true;
                    done = true;
                } else if (ch == 27) {                /* Escape */
                    done = true;
                } else if (ch == '\t') {
                    short n;
                    for (n = 1; n <= kSettingsFieldCount; n++) {
                        short k = static_cast<short>((focus + n) %
                                                     kSettingsFieldCount);
                        if (kSettingsFields[k].section != section) continue;
                        if (item[k].te == nullptr) continue;
                        if (focus >= 0 && item[focus].te != nullptr)
                            TEDeactivate(item[focus].te);
                        focus = k;
                        TEActivate(item[k].te);
                        break;
                    }
                } else if (focus >= 0 && item[focus].te != nullptr) {
                    TEKey(ch, item[focus].te);
                }
                break;
            }

            default:
                break;
            }
        }

        /* ---- Save, if OK ---- */
        if (accept) {
            for (i = 0; i < kSettingsFieldCount; i++) {
                const SettingsField *f = &kSettingsFields[i];
                char value[kSettingsValueMax];

                switch (f->kind) {
                case kFieldCheck:
                    if (item[i].ctl == nullptr) break;
                    GWConfig_Set(f->key,
                                 GetControlValue(item[i].ctl) ? "1" : "0");
                    break;

                case kFieldPopup:
                    if (item[i].sel >= 1 && item[i].sel <= 3 &&
                        f->choices[item[i].sel - 1] != nullptr)
                        GWConfig_Set(f->key, f->choices[item[i].sel - 1]);
                    break;

                case kFieldNum:
                case kFieldText:
                    if (item[i].te == nullptr) break;
                    SettingsFieldText(item[i].te, value, sizeof(value));
                    GWConfig_Set(f->key, value);
                    break;

                default:
                    break;
                }
            }

            /* The core caches what it read at launch, so it has to be told. */
            GWConfig_Load();
            GW_LoadSettings();
        }

        /* ---- Take it down ---- */
        for (i = 0; i < kSettingsFieldCount; i++) {
            if (item[i].te != nullptr) TEDispose(item[i].te);
            if (item[i].ctl != nullptr) DisposeControl(item[i].ctl);
            if (item[i].menu != nullptr) DisposeMenu(item[i].menu);
        }
        if (hSection != nullptr) DisposeControl(hSection);
        if (hOK != nullptr) DisposeControl(hOK);
        if (hCancel != nullptr) DisposeControl(hCancel);
        if (mSection != nullptr) DisposeMenu(mSection);
        DisposeWindow(win);

        SetPort(savePort);
        UpdateWindowMenuItem();
        if (mWindow != nullptr) Redraw();
    }

    /*
     * Draw one pane: its labels, the frames around its entry fields, and the
     * controls the Control Manager owns. The checkboxes draw their own titles
     * in the system font, so nothing here measures those -- measuring a
     * system-font title in the port's font is what once cut "Web proxy" to
     * "Web pro".
     */
    void DrawSettingsPane(WindowPtr win, SettingsCtl *item, short section,
                          const Rect &contentRect, short labelCol,
                          const Rect &okRect)
    {
        Rect   all = reinterpret_cast<GrafPtr>(win)->portRect;
        Str255 title;
        short  i;

        EraseRect(&all);

        TextFont(kFontGeneva);
        TextSize(9);

        for (i = 0; i < kSettingsFieldCount; i++) {
            const SettingsField *f = &kSettingsFields[i];
            short labelX;

            if (f->section != section) continue;
            if (f->kind == kFieldCheck) continue;

            /* Application panel: labels are right-aligned before the field,
             * not at the global label column. */
            if (f->section == kSecApp) {
                labelX = static_cast<short>(item[i].box.left - 8 -
                                            StringWidth(title));
            } else {
                labelX = static_cast<short>(contentRect.left + labelCol - 8 -
                                            StringWidth(title));
            }

            ToPascal(f->label, title);
            MoveTo(labelX,
                   static_cast<short>(item[i].box.top + 12));
            DrawString(title);

            if (item[i].te != nullptr) {
                Rect frame = item[i].box;
                FrameRect(&frame);
                TEUpdate(&frame, item[i].te);
            }
        }

        DrawControls(win);

        /* Mac OS 9 rings the default button. */
        {
            Rect ring = okRect;
            InsetRect(&ring, -4, -4);
            PenSize(3, 3);
            FrameRoundRect(&ring, 16, 16);
            PenSize(1, 1);
        }
    }

    /*
     * About Gateway, laid out the way iWordle's is: the application icon,
     * then alternating Charcoal and Geneva lines for name, author and
     * credits. A real title bar with a close box and no OK button, which is
     * the Mac OS 9 convention -- SimpleText's About box does the same.
     */
    void DrawAboutContent(WindowPtr w)
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
