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
#include "../gw_config.h"
#include "../portable/gw_log.h"

#define GW_CLASS      "GatewayWndClass"
#define GW_TRAY_MSG   (WM_APP + 1)
#define GW_TRAY_ID     1
#define ID_LOG         100

#define IDM_SHOW       40001
#define IDM_STARTUP    40002
#define IDM_QUIT       40003

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
static int startup_enabled(void)
{
    HKEY  key;
    DWORD type, len = 0;
    int   found = 0;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key)
        != ERROR_SUCCESS)
        return 0;

    if (RegQueryValueExA(key, "Gateway", NULL, &type, NULL, &len)
        == ERROR_SUCCESS)
        found = 1;

    RegCloseKey(key);
    return found;
}

static void startup_set(int on)
{
    HKEY key;
    char exe[MAX_PATH];

    if (RegOpenKeyExA(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key)
        != ERROR_SUCCESS)
        return;

    if (on) {
        DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));

        if (n > 0 && n < sizeof(exe))
            RegSetValueExA(key, "Gateway", 0, REG_SZ,
                           (const BYTE *)exe, (DWORD)strlen(exe) + 1);
    } else {
        RegDeleteValueA(key, "Gateway");
    }
    RegCloseKey(key);
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
    gTray.hIcon  = LoadIcon(NULL, IDI_APPLICATION);
    strcpy(gTray.szTip, "Gateway");

    Shell_NotifyIconA(NIM_ADD, &gTray);
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
    AppendMenuA(menu, MF_STRING | (startup_enabled() ? MF_CHECKED : 0),
                IDM_STARTUP, "Start with &Windows");
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

    (void)prev; (void)cmd; (void)show;
    gInst = inst;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = GW_CLASS;
    if (!RegisterClassA(&wc)) return 1;

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

    /*
     * The cooperative loop. Drain the queue, take one pass through the proxy,
     * then yield. Sleep(1) rather than a spin: a pass with nothing to do costs
     * a fraction of a millisecond, and without the yield this would take a
     * whole core from a machine that has one.
     */
    for (;;) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
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
    return 0;
}
