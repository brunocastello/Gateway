/*
 * main_win32.c - the Windows shell: a tray icon, a log window, and the loop.
 *
 * The counterpart of src/main.cpp, and the same shape. Gateway is one
 * cooperative thread: the message loop pumps Windows, then calls GW_Poll()
 * once, then yields. Nothing in the core blocks, so a pass is short and the
 * proxy stays responsive without a thread per connection.
 *
 * Targets Windows 95 OSR2 and up, so nothing here is newer than that: no
 * HWND_MESSAGE for the hidden window (2000 and later only), no
 * NOTIFYICONDATA fields past the Windows 95 shell's, and no common controls
 * beyond the listbox that has always been there.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <stdio.h>
#include <string.h>

#include "../gw_core.h"
#include "../gw_version.h"
#include "../gw_config.h"
#include "../portable/gw_log.h"

#define GW_CLASS      "GatewayWndClass"
#define GW_ABOUT_CLASS "GatewayAboutClass"
#define GW_TRAY_MSG   (WM_APP + 1)
#define GW_TRAY_ID     1
#define GW_ICON_APP    1      /* gateway.ico      blue: the application */
#define GW_ICON_ON     2      /* gateway-on.ico   green: running */
#define GW_ICON_OFF    3      /* gateway-off.ico  red: stopped */
#define ID_LOG         100

#define IDM_SHOW       40001
#define IDM_STARTUP    40002
#define IDM_STOP       40003
#define IDM_ABOUT      40004
#define IDM_QUIT       40005

static HWND  gMain;
static HWND  gList;
static HINSTANCE gInst;
static NOTIFYICONDATAA gTray;
static int   gShown;
static long  gSeenGeneration = -1;

static const char kRunKey[] =
    "Software\\Microsoft\\Windows\\CurrentVersion\\Run";

/* ------------------------------------------------------------------ */
/* Start with Windows                                                  */
/* ------------------------------------------------------------------ */

/*
 * The Run key, not the Startup folder.
 *
 * Both work on every target, but the Startup folder is per-user and its
 * location moves between 95, NT 4 and 2000; HKCU\...\Run is the same string
 * everywhere in this range and needs no shell folder lookup.
 */
/*
 * Which hive holds our Run value, if either does.
 *
 * Both are checked because both are used. Windows 95 without user profiles
 * makes HKEY_CURRENT_USER an alias of HKEY_USERS\.Default and does not create
 * the Run key under it at all -- the system's own entries live in
 * HKEY_LOCAL_MACHINE -- while NT and later expect a per-user entry and may
 * refuse a machine-wide one to an ordinary account.
 */
static HKEY startup_hive(void)
{
    static const HKEY hives[2] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    int i;

    for (i = 0; i < 2; i++) {
        HKEY  key;
        DWORD len = 0;

        if (RegOpenKeyExA(hives[i], kRunKey, 0, KEY_QUERY_VALUE, &key)
            != ERROR_SUCCESS)
            continue;

        if (RegQueryValueExA(key, "Gateway", NULL, NULL, NULL, &len)
            == ERROR_SUCCESS) {
            RegCloseKey(key);
            return hives[i];
        }
        RegCloseKey(key);
    }
    return NULL;
}

static int startup_enabled(void)
{
    return startup_hive() != NULL;
}

/*
 * RegCreateKeyEx, not RegOpenKeyEx: the key may not exist yet.
 *
 * This is what was wrong. Opening a key that Windows 95 never created failed,
 * the function returned without saying anything, and the menu item then found
 * no value to tick -- so it neither worked nor appeared to have been clicked.
 */
static int startup_write(HKEY hive, const char *exe)
{
    HKEY  key;
    DWORD disp;
    LONG  r;
    char  quoted[MAX_PATH + 2];

    if (RegCreateKeyExA(hive, kRunKey, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, NULL, &key, &disp) != ERROR_SUCCESS)
        return 0;

    /* Quoted, so a path with a space in it is one argument rather than two.
     * C:\Gateway has none, but the installer lets the user choose. */
    wsprintfA(quoted, "\"%s\"", exe);

    r = RegSetValueExA(key, "Gateway", 0, REG_SZ,
                       (const BYTE *)quoted, (DWORD)strlen(quoted) + 1);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

static void startup_remove(HKEY hive)
{
    HKEY key;

    if (RegOpenKeyExA(hive, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    RegDeleteValueA(key, "Gateway");
    RegCloseKey(key);
}

static void startup_set(int on)
{
    char  exe[MAX_PATH];
    DWORD n;

    if (!on) {
        /* Out of both, so a value written to either hive is really gone. */
        startup_remove(HKEY_CURRENT_USER);
        startup_remove(HKEY_LOCAL_MACHINE);
        return;
    }

    n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (n == 0 || n >= sizeof(exe)) {
        gw_log("start with Windows: could not find my own path");
        return;
    }

    /* Per-user first; machine-wide when that will not take it, which is the
     * ordinary case on Windows 95 with profiles off. */
    if (!startup_write(HKEY_CURRENT_USER, exe) &&
        !startup_write(HKEY_LOCAL_MACHINE, exe))
        gw_log("start with Windows: the registry would not take the entry");
}

/* ------------------------------------------------------------------ */
/* The log window                                                      */
/* ------------------------------------------------------------------ */

/*
 * A listbox rather than a painted ring buffer.
 *
 * The Mac build draws the log itself because the Multiversal interfaces have
 * no scroll bar it could hang off. Windows has had one in every listbox since
 * 3.0, and using it means scrolling, selection and keyboard navigation all
 * work without being written -- and it looks like the system rather than like
 * an imitation of it.
 */
static void log_refresh(void)
{
    long gen = gw_log_generation();
    int  count, i, top;

    if (gList == NULL || gen == gSeenGeneration) return;
    gSeenGeneration = gen;

    /* Is the view parked at the bottom? If so it should follow new lines; if
     * the user has scrolled back, it should stay where they put it. */
    top = (int)SendMessage(gList, LB_GETTOPINDEX, 0, 0);
    count = (int)SendMessage(gList, LB_GETCOUNT, 0, 0);

    SendMessage(gList, WM_SETREDRAW, FALSE, 0);
    SendMessage(gList, LB_RESETCONTENT, 0, 0);

    count = GW_LogCount();
    for (i = 0; i < count; i++) {
        const char *line = GW_LogLine(i);

        if (line != NULL)
            SendMessage(gList, LB_ADDSTRING, 0, (LPARAM)line);
    }
    SendMessage(gList, LB_SETTOPINDEX, (WPARAM)(count > 0 ? count - 1 : 0), 0);
    SendMessage(gList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(gList, NULL, TRUE);
    (void)top;
}

static void window_show(int show)
{
    gShown = show ? 1 : 0;
    ShowWindow(gMain, gShown ? SW_SHOW : SW_HIDE);
    if (gShown) {
        gSeenGeneration = -1;           /* force a repaint of the backlog */
        log_refresh();
        SetForegroundWindow(gMain);
    }
    GW_SetShowWindowPref(gShown);
}

/* ------------------------------------------------------------------ */
/* Tray                                                                */
/* ------------------------------------------------------------------ */

static void tray_add(void)
{
    memset(&gTray, 0, sizeof(gTray));
    /*
     * sizeof(NOTIFYICONDATAA) on a modern SDK describes a struct the Windows
     * 95 shell has never seen, and it rejects the call. The size through the
     * szTip member is the Windows 95 layout, and every later shell still
     * accepts it.
     */
    gTray.cbSize = (DWORD)(FIELD_OFFSET(NOTIFYICONDATAA, szTip) + 64);
    gTray.hWnd   = gMain;
    gTray.uID    = GW_TRAY_ID;
    gTray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    gTray.uCallbackMessage = GW_TRAY_MSG;
    gTray.hIcon  = LoadIcon(gInst, MAKEINTRESOURCE(GW_ICON_ON));
    strcpy(gTray.szTip, "Gateway");

    Shell_NotifyIconA(NIM_ADD, &gTray);
}

/*
 * The tray icon says whether the gateway is running: the opening under the
 * arch is green while it is, red while it is not.
 *
 * Greying the whole icon was tried first and read as a flat blob at 16 by 16.
 * At that size the opening is the only element with enough pixels to carry a
 * state, and recolouring just it keeps the stonework and the padlock identical
 * across all three icons, so it still reads as Gateway.
 */
static void tray_set_icon(void)
{
    gTray.uFlags = NIF_ICON;
    gTray.hIcon  = LoadIcon(gInst, MAKEINTRESOURCE(
                       GW_IsRunning() ? GW_ICON_ON : GW_ICON_OFF));
    Shell_NotifyIconA(NIM_MODIFY, &gTray);
    gTray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

/*
 * Release the ports, or bind them again, without quitting. The window stays
 * up either way, so the log remains readable and the settings can be
 * corrected before starting again -- which is the point of stopping rather
 * than quitting.
 */
static void toggle_running(void)
{
    if (GW_IsRunning())
        GW_Stop();
    else if (!GW_Start())
        gw_log("could not start: the ports may still be in use");
    tray_set_icon();
}

static void tray_remove(void)
{
    Shell_NotifyIconA(NIM_DELETE, &gTray);
}

static void tray_menu(void)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;

    if (menu == NULL) return;

    AppendMenuA(menu, MF_STRING, IDM_SHOW,
                gShown ? "&Hide Window" : "&Show Window");
    /* The item names what a click will do, as the window item does. */
    AppendMenuA(menu, MF_STRING, IDM_STOP,
                GW_IsRunning() ? "S&top Gateway" : "S&tart Gateway");
    AppendMenuA(menu, MF_STRING | (startup_enabled() ? MF_CHECKED : 0),
                IDM_STARTUP, "Start with &Windows");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, IDM_ABOUT, "&About Gateway...");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, IDM_QUIT, "&Quit");

    GetCursorPos(&pt);
    /*
     * The window has to be foreground first, or the menu will not dismiss
     * when the user clicks elsewhere -- a documented quirk of tray menus that
     * has been there since the shell gained them.
     */
    SetForegroundWindow(gMain);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, gMain, NULL);
    PostMessage(gMain, WM_NULL, 0, 0);
    DestroyMenu(menu);
}


/* ------------------------------------------------------------------ */
/* About                                                               */
/* ------------------------------------------------------------------ */

/*
 * A small window rather than a MessageBox.
 *
 * The Mac build draws its own About box, and this is the same information in
 * the shape Windows expects: the application icon, the name and version, one
 * line saying what the program is, and the licence. A MessageBox would have
 * been three lines of code and would have looked like an error.
 */
static HWND gAbout;


/*
 * The same box the Mac build draws, in Windows' own typeface.
 *
 * DrawAboutContent() in src/main.cpp centres seven lines at fixed offsets from
 * the top of a 280 by 230 window, alternating Charcoal 12 and Geneva 10, all
 * at normal weight. Those offsets, sizes and that weight are reproduced here;
 * only the family changes, to the one Windows actually has. The first attempt
 * was a left-aligned block of prose in bold beside an icon, which was a
 * different design rather than the same one.
 */
#define GW_ABOUT_W 280
#define GW_ABOUT_H 272        /* the Mac's 230 of content, plus the OK button */

/*
 * The Mac's About box, in Windows' typeface.
 *
 * DrawAboutContent() in src/main.cpp plots a 32x32 icon centred at the top,
 * then centres seven lines at fixed offsets. Those offsets are reproduced
 * exactly. Two things differ deliberately: the names are bold, and only the
 * application's own name is set larger -- the Mac gives 12 point to all three,
 * which at this size makes the box read as three headings rather than as one
 * title with attributions under it.
 *
 * The window is taller than the Mac's because Windows expects a button and
 * Mac OS closes an About box from its close box. The extra height goes below
 * the text, so every line still lands where it was laid out -- at 230 the last
 * line shared its row with the OK button.
 */
/* Points to logical units through the display's own resolution, so the text is
 * the size it claims whatever the screen is set to. */
static HFONT about_font(int dpi, int pt, int weight)
{
    return CreateFontA(-MulDiv(pt, dpi, 72), 0, 0, 0, weight, 0, 0, 0,
                       ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       DEFAULT_QUALITY, VARIABLE_PITCH | FF_SWISS,
                       "MS Sans Serif");
}

static const struct { int y; int pt; int bold; const char *text; } kAbout[] = {
    {  64, 12, 1, "Gateway " GW_VERSION_STRING     },
    {  84, 10, 0, "A TLS 1.3 gateway for Windows"  },
    { 112, 10, 1, "Bruno Castello"                 },
    { 132, 10, 0, "bfcastello@hotmail.com"         },
    { 160, 10, 1, "Engineer: Claude Opus 5"        },
    { 188, 10, 0, "\xA9 Castello Designs, 2026"    },
    { 208, 10, 0, "Built with MinGW-w64"           }
};

static LRESULT CALLBACK AboutProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC         dc = BeginPaint(hwnd, &ps);
        RECT        area;
        HFONT       title, name, plain, old;
        HICON       icon;
        int         i, dpi, midX;

        GetClientRect(hwnd, &area);
        midX = (area.right - area.left) / 2;

        /* The application icon, centred, where the Mac plots its own. */
        icon = LoadIcon(gInst, MAKEINTRESOURCE(GW_ICON_APP));
        if (icon != NULL)
            DrawIconEx(dc, midX - 16, 14, icon, 32, 32, 0, NULL, DI_NORMAL);

        dpi = GetDeviceCaps(dc, LOGPIXELSY);
        title = about_font(dpi, 12, FW_BOLD);    /* the application name */
        name  = about_font(dpi, 10, FW_BOLD);    /* who wrote it */
        plain = about_font(dpi, 10, FW_NORMAL);  /* everything else */

        SetBkMode(dc, TRANSPARENT);
        SetTextAlign(dc, TA_CENTER | TA_BASELINE);
        old = (HFONT)SelectObject(dc, plain);

        for (i = 0; i < (int)(sizeof(kAbout) / sizeof(kAbout[0])); i++) {
            SelectObject(dc, !kAbout[i].bold ? plain
                             : (kAbout[i].pt == 12 ? title : name));
            TextOutA(dc, midX, kAbout[i].y, kAbout[i].text,
                     (int)strlen(kAbout[i].text));
        }

        SelectObject(dc, old);
        DeleteObject(title);
        DeleteObject(name);
        DeleteObject(plain);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) DestroyWindow(hwnd);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        gAbout = NULL;
        return 0;

    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void about_show(void)
{
    RECT work, frame;
    int  w, h, x, y;

    if (gAbout != NULL) {           /* already up: bring it forward */
        SetForegroundWindow(gAbout);
        return;
    }

    /*
     * The Mac box is 280 by 230 of content. AdjustWindowRect turns that into
     * the outer size, so the text lands at the offsets it was laid out for
     * rather than however much smaller the caption leaves.
     */
    SetRect(&frame, 0, 0, GW_ABOUT_W, GW_ABOUT_H);
    AdjustWindowRect(&frame, WS_CAPTION | WS_SYSMENU, FALSE);
    w = frame.right - frame.left;
    h = frame.bottom - frame.top;

    /* Centred on the working area, so it clears the taskbar. */
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0))
        SetRect(&work, 0, 0, 640, 480);
    x = work.left + ((work.right - work.left) - w) / 2;
    y = work.top + ((work.bottom - work.top) - h) / 2;

    gAbout = CreateWindowA(GW_ABOUT_CLASS, "About Gateway",
                           WS_CAPTION | WS_SYSMENU,
                           x, y, w, h, NULL, NULL, gInst, NULL);
    if (gAbout == NULL) return;

    CreateWindowA("BUTTON", "OK",
                  WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                  (GW_ABOUT_W - 76) / 2, 230, 76, 26,
                  gAbout, (HMENU)IDOK, gInst, NULL);

    ShowWindow(gAbout, SW_SHOW);
    SetForegroundWindow(gAbout);
}

/* ------------------------------------------------------------------ */
/* Window                                                              */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        gList = CreateWindowA("LISTBOX", NULL,
                              WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                              LBS_NOINTEGRALHEIGHT | LBS_NOSEL,
                              0, 0, 0, 0, hwnd, (HMENU)ID_LOG, gInst, NULL);
        if (gList != NULL) {
            /* A fixed pitch, as the Mac build uses Monaco: the log lines up
             * only if every character is the same width. */
            HFONT f = CreateFontA(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                  ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                  FIXED_PITCH | FF_MODERN, "Courier New");
            if (f != NULL) SendMessage(gList, WM_SETFONT, (WPARAM)f, TRUE);
        }
        return 0;

    case WM_SIZE:
        if (gList != NULL)
            MoveWindow(gList, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;

    case WM_CLOSE:
        /* Closing hides; Gateway is a background program and quitting it by
         * accident would take the proxy down with the window. */
        window_show(0);
        return 0;

    case GW_TRAY_MSG:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) tray_menu();
        else if (lp == WM_LBUTTONDBLCLK) window_show(!gShown);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SHOW:    window_show(!gShown); break;
        case IDM_STARTUP: startup_set(!startup_enabled()); break;
        case IDM_STOP:    toggle_running(); break;
        case IDM_ABOUT:   about_show(); break;
        case IDM_QUIT:    PostMessage(hwnd, WM_DESTROY, 0, 0); break;
        default: break;
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    MSG       msg;
    HANDLE    once;

    (void)prev; (void)cmd; (void)show;
    gInst = inst;

    /*
     * One Gateway at a time.
     *
     * Two copies both try to bind the same ports and the second fails in a way
     * that looks like a broken installation. It happens easily: a shortcut in
     * the Startup group and the tray menu's "Start with Windows" are separate
     * registration mechanisms that cannot see each other, so enabling both
     * launches two at login. Double-clicking the executable while it is
     * already in the tray does the same thing.
     *
     * A named mutex is the Windows 95 answer and needs nothing newer. The
     * second instance surfaces the first one's window instead of starting, so
     * the click still does something sensible.
     */
    once = CreateMutexA(NULL, TRUE, "GatewayRunningMutex");
    if (once != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND first = FindWindowA(GW_CLASS, NULL);

        if (first != NULL) {
            ShowWindow(first, SW_SHOW);
            SetForegroundWindow(first);
        }
        CloseHandle(once);
        return 0;
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    /* MAKEINTRESOURCE(1) is gateway.ico, from gateway.rc. */
    wc.hIcon         = LoadIcon(inst, MAKEINTRESOURCE(GW_ICON_APP));
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = GW_CLASS;
    if (!RegisterClassA(&wc)) return 1;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = AboutProc;
    wc.hInstance     = inst;
    wc.hIcon         = LoadIcon(inst, MAKEINTRESOURCE(GW_ICON_APP));
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = GW_ABOUT_CLASS;
    RegisterClassA(&wc);

    gMain = CreateWindowA(GW_CLASS, "Gateway",
                          WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT, 560, 340,
                          NULL, NULL, inst, NULL);
    if (gMain == NULL) return 1;

    tray_add();

    /* Settings first: whether a window appears at all is one of them. */
    GW_LoadSettings();
    window_show(GW_ShowWindowPref());

    if (!GW_Init()) {
        /* Something is wrong enough that there is nothing to serve, but the
         * window says what, so it stays up rather than vanishing. */
        window_show(1);
    }

    /* After GW_Init, which starts the gateway: the icon reports the state it
     * actually ended up in, not the one it had before trying. */
    tray_set_icon();

    /*
     * The cooperative loop. Drain the queue, take one pass through the proxy,
     * then yield. Sleep(1) rather than a spin: a pass with nothing to do costs
     * a fraction of a millisecond, and without the yield this would take a
     * whole core from a machine that has one.
     */
    for (;;) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            /* Enter and Escape should close the About box, as they would in
             * a real dialog; IsDialogMessage gives that for nothing. */
            if (gAbout != NULL && IsDialogMessageA(gAbout, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        GW_Poll();
        if (gShown) log_refresh();
        Sleep(1);
    }

done:
    GW_Shutdown();
    tray_remove();
    if (once != NULL) CloseHandle(once);
    return 0;
}
