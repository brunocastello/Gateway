/*
 * settings_win32.c - the preferences window.
 *
 * The counterpart of src/ui/gw_settings.cpp. One field table drives the values
 * and the geometry on both platforms, the panes are the same panes in the same
 * order, and the window is only as tall as the pane on show.
 *
 * Built out of nothing but the six window classes USER32 has carried since
 * Windows NT 3.1 -- BUTTON, EDIT, STATIC, COMBOBOX and the scroll bar an EDIT
 * brings with it. No common controls, so no tab control and no property sheet:
 * a drop-down picks the pane, as it does on the Mac and as a Windows 3.1
 * control panel did. That is what keeps NT 3.51 in range, where COMCTL32 is a
 * different and much older animal.
 *
 * NT 3.51 differs in two places and neither is a control:
 *
 *   - WS_EX_CLIENTEDGE, the sunken field border, arrived with Windows 95 and
 *     is ignored before it, which would leave the fields with no border at
 *     all. So the border is chosen at run time: the extended style from 4.0
 *     up, a plain WS_BORDER below it.
 *   - It draws Program Manager chrome whatever we do -- flat, not chiselled.
 *     That is NT 3.51 looking like itself, and nothing here fights it.
 *
 * Not built as a dialog resource. DIALOGEX is a Windows 95 format 3.51 cannot
 * read, and plain DIALOG cannot carry the extended styles or the per-pane
 * resize this window needs. CreateWindow plus IsDialogMessage gives the
 * keyboard behaviour a dialog would have, which is the only part worth having.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings_win32.h"
#include "../gw_config.h"
#include "../gw_core.h"
#include "../portable/gw_prefs.h"
#include "../portable/gw_util.h"

#define GW_SETTINGS_CLASS "GatewaySettingsClass"

enum { Check, Number, Date, Text, Redirect, Provider, List };

typedef struct {
    int         pane;
    int         kind;
    const char *key, *label, *fallback, *hint;  /* hint lines split on '\n' */
} Field;

#define PANES 8

static const char *const kPanes[PANES] = {
    "Modules", "Web proxy", "Wayback", "Wayback sites",
    "Mail", "Mail upstream", "OAuth", "Log"
};

/* Standing text at the top of a pane, above its first row. */
static const char *const kIntro[PANES] = {
    "Stop and start Gateway after changing listeners.",
    "",
    "",
    "",
    "",
    "Empty host fields use the selected provider's defaults.",
    "Obtain the refresh token outside Gateway, then paste it here.\n"
    "Long values scroll horizontally. Tokens may rotate while running.",
    "",
};

/* Rows are laid out in this order, pane by pane. */
static const Field kFields[] = {
    { 0, Check, "http_enabled", "&Web proxy", "1", "Browse modern sites through the web proxy." },
    { 0, Check, "mail_enabled", "&Mail", "1", "Connect a mail client to IMAP, POP and SMTP." },
    { 0, Check, "wayback_enabled", "Wa&yback proxy", "1", "Browse archived pages from the Wayback Machine." },
    { 1, Number, "http_port", "Port:", "8765", "" },
    { 1, Check, "rewrite_https", "&Rewrite https:// links to http://", "1",
      "For browsers without modern TLS support." },
    { 1, Check, "connect_mitm", "&Terminate TLS for typed https:// URLs", "0",
      "Requires the Gateway CA in the browser. Choose one mode." },
    { 1, Redirect, "follow_redirects", "Follow redirects:", "auto", "Automatic follows HTTPS redirects." },
    { 1, Number, "max_body_mb", "Largest response (MB):", "0", "0 for no limit." },
    { 1, Number, "max_connects", "Connections opening at once:", "8", "" },
    { 1, Number, "max_sessions", "Concurrent sessions:", "12", "Shared by Web proxy and Wayback; mail has its own limit." },
    { 2, Number, "wayback_port", "Port:", "8888", "0 disables the archive listener." },
    { 2, Date, "wayback_date", "Era (YYYYMMDD):", "20011231", "Also accepts YYYY or YYYYMM." },
    { 2, Number, "wayback_tolerance", "Days newer allowed:", "730", "0 accepts any date." },
    { 2, Number, "wayback_connects", "Connections opening at once:", "1", "The archive refuses bursts." },
    { 2, Check, "wayback_api", "Find the &nearest available snapshot", "1",
      "Off requests the configured era directly." },
    { 3, Check, "wayback_geocities", "Send &geocities.com to oocities.org", "1", "" },
    { 3, Check, "wayback_cache", "Let the browser &keep snapshots", "1", "" },
    { 3, Check, "wayback_settings", "Serve the browser se&ttings page", "1", "" },
    { 3, Check, "wayback_ct_encoding", "Strip &charset from Content-Type", "1", "" },
    { 3, Check, "wayback_quick_images", "&Quick images (compatibility setting)", "1",
      "Accepted for compatibility; it has no effect." },
    { 3, List, "wayback_live", "&Whitelist:", "",
      "One site per line, or separated with ; -- plain names include subdomains.\n"
      "Maximum 2000 characters. Remove a site to archive it again." },
    { 4, Provider, "provider", "Provider:", "outlook", "" },
    { 4, Text, "oauth_user", "Address:", "", "" },
    { 4, Text, "local_password", "Password for the mail client:", "",
      "Checked locally; never sent upstream." },
    { 4, Number, "imap_port", "IMAP port:", "1993", "" },
    { 4, Number, "pop_port", "POP port:", "1995", "" },
    { 4, Number, "smtp_port", "SMTP port:", "1587", "" },
    { 5, Text, "imap_host", "IMAP host:", "", "" },
    { 5, Number, "imap_upstream_port", "IMAP port:", "993", "" },
    { 5, Text, "pop_host", "POP host:", "", "" },
    { 5, Number, "pop_upstream_port", "POP port:", "995", "" },
    { 5, Text, "smtp_host", "SMTP host:", "", "" },
    { 5, Number, "smtp_upstream_port", "SMTP port:", "587", "" },
    { 5, Check, "smtp_starttls", "Use START&TLS on the SMTP port", "1",
      "Port 465 uses TLS immediately; other ports default to STARTTLS." },
    { 6, Text, "oauth_host", "Token host:", "", "" },
    { 6, Text, "oauth_path", "Token path:", "", "" },
    { 6, Text, "oauth_scope", "Scope:", "", "" },
    { 6, Text, "oauth_client_id", "Client ID:", "", "" },
    { 6, Text, "oauth_client_secret", "Client secret:", "", "" },
    { 6, Text, "refresh_token", "Refresh token:", "", "" },
    { 7, Check, "show_window", "Show the &log window at launch", "1",
      "Off starts with the notification area icon only. Where there is no\n"
      "notification area the window always appears." },
    { 7, Check, "log_file", "Also &write the log to a file", "0",
      "The window keeps the last 200 lines.\n"
      "The log file keeps everything." },
};
#define FIELDS ((int)(sizeof(kFields) / sizeof(kFields[0])))

static const char *const kRedirects[] = { "auto", "always", "never" };
static const char *const kRedirectNames[] = { "Automatic", "Always", "Never" };
static const char *const kProviders[] = { "outlook", "gmail", "custom" };
static const char *const kProviderNames[] = { "Outlook", "Gmail", "Custom" };

/* The display form of the whitelist is one entry per line, so it is longer
 * than the semicolon form the file keeps and the 2000 character ceiling
 * counts. */
#define VALUE_CAP 4096
#define LIST_LIMIT 2000
#define MAX_HINT_LINES 2

/*
 * Metrics, in pixels at 96 DPI; S() scales them to the screen's own. Windows
 * dialog units would be the usual currency, but this window is built control
 * by control rather than from a template, so there is no template font for
 * MapDialogRect to measure against.
 */
#define S(v) MulDiv((v), gDpi, 96)

enum {
    kWidth       = 440,
    kMargin      = 11,
    kComboTop    = 10,
    kComboH      = 21,
    kComboDrop   = 150,       /* the closed box plus its dropped list */
    kPaneComboX  = 85,
    kPaneComboW  = 150,
    kGroupTop    = 40,
    kGroupLeft   = kMargin,
    kGroupRight  = kWidth - kMargin,
    kInset       = 12,        /* group frame to its contents */
    kRowLeft     = kGroupLeft + kInset,
    kRowRight    = kGroupRight - kInset,
    kCapIndent   = 16,        /* a caption under a check box clears its square */
    kFieldLeft   = 200,
    kFirstRow    = kGroupTop + 20,
    kCheckH      = 16,
    kEditH       = 20,
    kListH       = 92,
    kLineH       = 13,
    kCapGap      = 3,
    kRowGap      = 8,
    kGroupPadBot = 12,
    kBtnTop      = 12,
    kBtnW        = 75,
    kBtnH        = 23,
    kBtnGap      = 6,
    kBottom      = 11
};

#define IDC_PANE     1000
#define IDC_GROUP    1001
#define IDC_UNDO     1002
#define IDC_FIELD    1100     /* + the field's index */

typedef struct {
    HWND ctrl;
    HWND label;                        /* NULL where the control draws its own */
    HWND hint[MAX_HINT_LINES];
    int  lines;
    char original[VALUE_CAP];
    int  overflow;                     /* never silently save a truncated list */
} Item;

static HWND      gWnd;
static HINSTANCE gInst;
static HFONT     gFont;
static HBRUSH    gFace;
static int       gDpi = 96;
static int       gSunken;              /* WS_EX_CLIENTEDGE is Windows 95 and up */
static int       gPane;
static int       gPaneBottom[PANES];   /* group frame bottom, per pane */
static int       gPaneHeight[PANES];   /* client height, per pane */
static int       gTallest;
static Item      gItems[FIELDS];
static HWND      gIntro[PANES][MAX_HINT_LINES];
static HWND      gGroup, gCombo, gComboLabel, gSave, gCancel, gUndo;

static void show_pane(int pane);

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int hint_lines(const char *s)
{
    int n = s[0] != '\0';
    for (; *s; ++s) if (*s == '\n') ++n;
    return n > MAX_HINT_LINES ? MAX_HINT_LINES : n;
}

/* The nth line of a '\n'-separated string, copied out. */
static void hint_line(const char *s, int n, char *out, size_t cap)
{
    const char *stop;
    size_t len;
    int i;

    for (i = 0; i < n && s != NULL; ++i) {
        s = strchr(s, '\n');
        if (s != NULL) ++s;
    }
    if (s == NULL) { out[0] = '\0'; return; }
    stop = strchr(s, '\n');
    len = stop != NULL ? (size_t)(stop - s) : strlen(s);
    if (len >= cap) len = cap - 1;
    memcpy(out, s, len);
    out[len] = '\0';
}

static int editable(int kind)
{
    return kind == Text || kind == Number || kind == Date || kind == List;
}

static HWND child(const char *cls, const char *text, DWORD style, DWORD ex,
                  int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExA(ex, cls, text, WS_CHILD | style,
                             S(x), S(y), S(w), S(h),
                             gWnd, (HMENU)(INT_PTR)id, gInst, NULL);
    if (c != NULL && gFont != NULL)
        SendMessageA(c, WM_SETFONT, (WPARAM)gFont, TRUE);
    return c;
}

static HWND caption(const char *text, int x, int y, int right, int id)
{
    /* No SS_NOPREFIX: '&' marks the mnemonic on the labels that have one,
     * and no caption text here carries a literal ampersand. */
    return child("STATIC", text, SS_LEFT, 0,
                 x, y, right - x, kLineH, id);
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

/*
 * Walk every pane, create its controls where they fall, and record how tall
 * the pane leaves the window. Fields never move afterwards; only the group
 * frame, the buttons and the window itself change size with the pane.
 */
static int build_pane(int pane)
{
    char line[256];
    int y = kFirstRow;
    int i, n, lines;

    lines = hint_lines(kIntro[pane]);
    for (n = 0; n < lines; ++n) {
        hint_line(kIntro[pane], n, line, sizeof(line));
        gIntro[pane][n] = caption(line, kRowLeft, y + n * kLineH, kRowRight, -1);
    }
    if (lines > 0) y += lines * kLineH + kRowGap;

    for (i = 0; i < FIELDS; ++i) {
        const Field *f = &kFields[i];
        Item *it = &gItems[i];
        DWORD ex = 0;
        int bottom, capLeft = kRowLeft;

        if (f->pane != pane) continue;

        if (editable(f->kind)) {
            if (gSunken) ex = WS_EX_CLIENTEDGE;
        }

        switch (f->kind) {
        case Check:
            it->ctrl = child("BUTTON", f->label,
                             BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                             kRowLeft, y, kRowRight - kRowLeft, kCheckH,
                             IDC_FIELD + i);
            bottom = y + kCheckH;
            capLeft = kRowLeft + kCapIndent;
            break;

        case List:
            it->label = caption(f->label, kRowLeft, y, kRowRight, -1);
            y += kLineH + 2;
            it->ctrl = child("EDIT", "",
                             ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
                             WS_VSCROLL | WS_TABSTOP |
                             (gSunken ? 0 : WS_BORDER), ex,
                             kRowLeft, y, kRowRight - kRowLeft, kListH,
                             IDC_FIELD + i);
            if (it->ctrl != NULL)
                SendMessageA(it->ctrl, EM_LIMITTEXT, VALUE_CAP - 1, 0);
            bottom = y + kListH;
            break;

        case Redirect:
        case Provider: {
            const char *const *names = f->kind == Redirect ? kRedirectNames
                                                           : kProviderNames;
            it->label = caption(f->label, kRowLeft, y + 4, kFieldLeft - 8, -1);
            it->ctrl = child("COMBOBOX", "",
                             CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0,
                             kFieldLeft, y, kRowRight - kFieldLeft, kComboDrop,
                             IDC_FIELD + i);
            for (n = 0; n < 3; ++n)
                SendMessageA(it->ctrl, CB_ADDSTRING, 0, (LPARAM)names[n]);
            bottom = y + kComboH;
            break;
        }

        default:
            it->label = caption(f->label, kRowLeft, y + 4, kFieldLeft - 8, -1);
            it->ctrl = child("EDIT", "",
                             ES_AUTOHSCROLL | WS_TABSTOP |
                             (gSunken ? 0 : WS_BORDER) |
                             (f->kind == Text ? 0 : ES_NUMBER), ex,
                             kFieldLeft, y, kRowRight - kFieldLeft, kEditH,
                             IDC_FIELD + i);
            if (it->ctrl != NULL)
                SendMessageA(it->ctrl, EM_LIMITTEXT, VALUE_CAP - 1, 0);
            bottom = y + kEditH;
            break;
        }
        if (it->ctrl == NULL) return 0;

        it->lines = hint_lines(f->hint);
        for (n = 0; n < it->lines; ++n) {
            hint_line(f->hint, n, line, sizeof(line));
            it->hint[n] = caption(line, capLeft,
                                  bottom + kCapGap + n * kLineH, kRowRight, -1);
        }
        y = bottom + (it->lines > 0 ? kCapGap + it->lines * kLineH : 0) + kRowGap;
    }

    gPaneBottom[pane] = y - kRowGap + kGroupPadBot;
    gPaneHeight[pane] = gPaneBottom[pane] + kBtnTop + kBtnH + kBottom;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Values                                                              */
/* ------------------------------------------------------------------ */

static void read_value(int i, char *out, size_t cap)
{
    const Field *f = &kFields[i];
    const Item *it = &gItems[i];

    if (editable(f->kind)) {
        GetWindowTextA(it->ctrl, out, (int)cap);
    } else if (f->kind == Check) {
        strcpy(out, SendMessageA(it->ctrl, BM_GETCHECK, 0, 0) == BST_CHECKED
                    ? "1" : "0");
    } else {
        const char *const *values = f->kind == Provider ? kProviders : kRedirects;
        LRESULT n = SendMessageA(it->ctrl, CB_GETCURSEL, 0, 0);
        if (n < 0 || n > 2) n = 0;
        strcpy(out, values[n]);
    }
}

static void load_values(void)
{
    char value[VALUE_CAP];
    char entry[VALUE_CAP];
    int i, n;

    GWConfig_Load();
    for (i = 0; i < FIELDS; ++i) {
        const Field *f = &kFields[i];
        Item *it = &gItems[i];

        value[0] = '\0';
        it->overflow = 0;

        if (f->kind == List) {
            /* Joined the way the file keeps it, so the 2000 character ceiling
             * means the same thing on both platforms; shown one per line,
             * which is what a multi-line edit is for. */
            size_t used;
            for (n = 0; GWConfig_GetNthSplit(f->key, n, entry, sizeof(entry)); ++n) {
                size_t len = strlen(entry);
                used = strlen(value);
                if (used + len + (used ? 1 : 0) > LIST_LIMIT) {
                    it->overflow = 1;
                    break;
                }
                if (used) strcat(value, "\r\n");
                strcat(value, entry);
            }
        } else if (f->kind == Check || f->kind == Number || f->kind == Date) {
            long fallback = strtol(f->fallback, NULL, 10);
            long num;
            if (strcmp(f->key, "smtp_starttls") == 0)
                fallback = GWConfig_Num("smtp_upstream_port", 587) != 465;
            num = GWConfig_Num(f->key, fallback);
            if (f->kind == Check) num = num != 0;
            sprintf(value, "%ld", num);
        } else {
            const char *current = GWConfig_Str(f->key, f->fallback);
            strncpy(value, current, sizeof(value) - 1);
            value[sizeof(value) - 1] = '\0';
        }

        strcpy(it->original, value);

        if (editable(f->kind)) {
            SetWindowTextA(it->ctrl, value);
            SendMessageA(it->ctrl, EM_SETSEL, 0, 0);
        } else if (f->kind == Check) {
            SendMessageA(it->ctrl, BM_SETCHECK,
                         value[0] == '1' ? BST_CHECKED : BST_UNCHECKED, 0);
        } else {
            const char *const *values = f->kind == Provider ? kProviders
                                                            : kRedirects;
            int chosen = 0;
            for (n = 0; n < 3; ++n)
                if (gw_stricmp(value, values[n]) == 0) chosen = n;
            SendMessageA(it->ctrl, CB_SETCURSEL, chosen, 0);
        }
    }
}

/* The Mac beeps and takes focus rather than printing a message; so does this. */
static void reject(int i)
{
    int pane = kFields[i].pane;

    if (pane != gPane) {
        SendMessageA(gCombo, CB_SETCURSEL, pane, 0);
        show_pane(pane);
    }
    MessageBeep(MB_ICONEXCLAMATION);
    SetFocus(gItems[i].ctrl);
    if (editable(kFields[i].kind))
        SendMessageA(gItems[i].ctrl, EM_SETSEL, 0, -1);
}

static int save_values(void)
{
    char value[VALUE_CAP];
    int i;

    /* Validate every pane before writing anything. */
    for (i = 0; i < FIELDS; ++i) {
        const Field *f = &kFields[i];
        int valid = 1;

        read_value(i, value, sizeof(value));

        if (gItems[i].overflow) {
            /* An oversized on-disk list can be kept, but not truncated here. */
            valid = strcmp(value, gItems[i].original) == 0;
        }
        /* The ceiling counts the form the file keeps, so it is measured after
         * normalising rather than by capping what the edit will accept. */
        if (valid && f->kind == List) {
            char joined[VALUE_CAP];
            strcpy(joined, value);
            gw_prefs_normalize_list(joined);
            valid = strlen(joined) <= LIST_LIMIT;
        }
        if (f->kind == Number || f->kind == Date) {
            unsigned long num = 0;
            const char *p;
            valid = value[0] != '\0';
            for (p = value; valid && *p; ++p) {
                if (*p < '0' || *p > '9' || num > 214748364UL) valid = 0;
                else {
                    num = num * 10 + (unsigned long)(*p - '0');
                    if (num > 2147483647UL) valid = 0;
                }
            }
            if (f->kind == Date)
                valid = valid && (strlen(value) == 4 || strlen(value) == 6 ||
                                  strlen(value) == 8);
            else if (strstr(f->key, "port") != NULL)
                valid = valid && num <= 65535 &&
                        (num > 0 || strcmp(f->key, "wayback_port") == 0);
            else if (strcmp(f->key, "max_body_mb") != 0 &&
                     strcmp(f->key, "wayback_tolerance") != 0)
                valid = valid && num > 0;
        }
        if (!valid) { reject(i); return 0; }
    }

    /* Write edits across all panes. Preserve untouched provider defaults and
     * any refresh token rotated by the live core while this window was open. */
    for (i = 0; i < FIELDS; ++i) {
        read_value(i, value, sizeof(value));
        if (strcmp(value, gItems[i].original) == 0) continue;
        if (kFields[i].kind == List) gw_prefs_normalize_list(value);
        if (!GWConfig_Set(kFields[i].key, value)) {
            MessageBeep(MB_ICONEXCLAMATION);
            SetFocus(gItems[i].ctrl);
            return 0;
        }
        strcpy(gItems[i].original, value);
    }

    GWConfig_Load();
    GW_LoadSettings();
    return 1;
}

/* ------------------------------------------------------------------ */
/* Panes                                                               */
/* ------------------------------------------------------------------ */

static void show_pane(int pane)
{
    RECT frame;
    int i, n, top;

    gPane = pane;

    for (i = 0; i < PANES; ++i)
        for (n = 0; n < MAX_HINT_LINES; ++n)
            if (gIntro[i][n] != NULL)
                ShowWindow(gIntro[i][n], i == pane ? SW_SHOW : SW_HIDE);

    for (i = 0; i < FIELDS; ++i) {
        int on = kFields[i].pane == pane ? SW_SHOW : SW_HIDE;
        ShowWindow(gItems[i].ctrl, on);
        if (gItems[i].label != NULL) ShowWindow(gItems[i].label, on);
        for (n = 0; n < gItems[i].lines; ++n)
            if (gItems[i].hint[n] != NULL) ShowWindow(gItems[i].hint[n], on);
    }

    SetWindowTextA(gGroup, kPanes[pane]);
    MoveWindow(gGroup, S(kGroupLeft), S(kGroupTop),
               S(kGroupRight - kGroupLeft), S(gPaneBottom[pane] - kGroupTop), TRUE);

    top = gPaneBottom[pane] + kBtnTop;
    MoveWindow(gSave, S(kGroupRight - 3 * kBtnW - 2 * kBtnGap), S(top),
               S(kBtnW), S(kBtnH), TRUE);
    MoveWindow(gCancel, S(kGroupRight - 2 * kBtnW - kBtnGap), S(top),
               S(kBtnW), S(kBtnH), TRUE);
    MoveWindow(gUndo, S(kGroupRight - kBtnW), S(top), S(kBtnW), S(kBtnH), TRUE);

    /* The window is exactly as tall as the pane on show. */
    SetRect(&frame, 0, 0, S(kWidth), S(gPaneHeight[pane]));
    AdjustWindowRect(&frame, WS_CAPTION | WS_SYSMENU, FALSE);
    SetWindowPos(gWnd, NULL, 0, 0,
                 frame.right - frame.left, frame.bottom - frame.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(gWnd, NULL, TRUE);
}

/* ------------------------------------------------------------------ */
/* Window                                                              */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    /* Statics, check boxes and the group frame sit on the window's own face
     * colour. A dialog would do this for us; a plain window has to be told. */
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
        SetTextColor((HDC)wp, GetSysColor(COLOR_BTNTEXT));
        return (LRESULT)gFace;

    /* IsDialogMessage asks who the default button is before it acts on
     * Return. DefWindowProc does not answer, so Enter would do nothing. */
    case DM_GETDEFID:
        return MAKELRESULT(IDOK, DC_HASDEFID);

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PANE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                LRESULT n = SendMessageA(gCombo, CB_GETCURSEL, 0, 0);
                if (n >= 0 && n < PANES) show_pane((int)n);
            }
            return 0;
        case IDOK:
            if (save_values()) DestroyWindow(hwnd);
            return 0;
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        case IDC_UNDO:
            load_values();
            return 0;
        default:
            break;
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        gWnd = NULL;
        if (gFont != NULL) { DeleteObject(gFont); gFont = NULL; }
        if (gFace != NULL) { DeleteObject(gFace); gFace = NULL; }
        return 0;

    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

HWND GWSettings_Window(void)
{
    return gWnd;
}

void GWSettings_Show(HINSTANCE inst)
{
    static int registered;
    WNDCLASSA wc;
    RECT work, frame;
    HDC dc;
    int i, w, h, x, y;

    if (gWnd != NULL) {                 /* already up: bring it forward */
        SetForegroundWindow(gWnd);
        return;
    }

    gInst = inst;
    if (!registered) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = SettingsProc;
        wc.hInstance     = inst;
        wc.hIcon         = LoadIcon(inst, MAKEINTRESOURCE(1));
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = GW_SETTINGS_CLASS;
        if (!RegisterClassA(&wc)) return;
        registered = 1;
    }

    dc = GetDC(NULL);
    if (dc != NULL) {
        gDpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(NULL, dc);
    }
    if (gDpi < 96) gDpi = 96;

    /* WS_EX_CLIENTEDGE came with Windows 95 and is ignored before it, which
     * would leave every field without a border. NT 3.51 gets WS_BORDER. */
    gSunken = (int)(LOBYTE(LOWORD(GetVersion()))) >= 4;

    /* MS Sans Serif 8pt: the shell font from Windows 3.1 through XP, and the
     * one the About box already uses. */
    gFont = CreateFontA(-MulDiv(8, gDpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        DEFAULT_QUALITY, VARIABLE_PITCH | FF_SWISS,
                        "MS Sans Serif");
    gFace = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));

    memset(gItems, 0, sizeof(gItems));
    memset(gIntro, 0, sizeof(gIntro));

    /*
     * Created at a nominal size and positioned for the tallest pane, so that
     * resizing between panes never walks the window off the screen.
     */
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0))
        SetRect(&work, 0, 0, 640, 480);

    gWnd = CreateWindowExA(0, GW_SETTINGS_CLASS, "Gateway Preferences",
                           WS_CAPTION | WS_SYSMENU | WS_CLIPSIBLINGS,
                           work.left, work.top, S(kWidth), S(kGroupTop),
                           NULL, NULL, inst, NULL);
    if (gWnd == NULL) return;

    gComboLabel = caption("Settings &for:", kMargin, kComboTop + 4,
                          kPaneComboX - 4, -1);
    gCombo = child("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0,
                   kPaneComboX, kComboTop, kPaneComboW, kComboDrop, IDC_PANE);
    gGroup = child("BUTTON", kPanes[0], BS_GROUPBOX, 0,
                   kGroupLeft, kGroupTop, kGroupRight - kGroupLeft, kCheckH,
                   IDC_GROUP);
    if (gCombo == NULL || gGroup == NULL) { DestroyWindow(gWnd); return; }

    gTallest = 0;
    for (i = 0; i < PANES; ++i) {
        SendMessageA(gCombo, CB_ADDSTRING, 0, (LPARAM)kPanes[i]);
        if (!build_pane(i)) { DestroyWindow(gWnd); return; }
        if (gPaneHeight[i] > gTallest) gTallest = gPaneHeight[i];
    }
    SendMessageA(gCombo, CB_SETCURSEL, 0, 0);

    /* Created after the fields, so Tab runs pop-up, fields, then buttons. */
    gSave   = child("BUTTON", "&Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
                    0, 0, kBtnW, kBtnH, IDOK);
    gCancel = child("BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0,
                    0, 0, kBtnW, kBtnH, IDCANCEL);
    gUndo   = child("BUTTON", "&Undo", BS_PUSHBUTTON | WS_TABSTOP, 0,
                    0, 0, kBtnW, kBtnH, IDC_UNDO);
    if (gSave == NULL || gCancel == NULL || gUndo == NULL) {
        DestroyWindow(gWnd);
        return;
    }

    load_values();

    ShowWindow(gComboLabel, SW_SHOW);
    ShowWindow(gCombo, SW_SHOW);
    ShowWindow(gGroup, SW_SHOW);
    ShowWindow(gSave, SW_SHOW);
    ShowWindow(gCancel, SW_SHOW);
    ShowWindow(gUndo, SW_SHOW);
    show_pane(0);

    SetRect(&frame, 0, 0, S(kWidth), S(gTallest));
    AdjustWindowRect(&frame, WS_CAPTION | WS_SYSMENU, FALSE);
    w = frame.right - frame.left;
    h = frame.bottom - frame.top;
    x = work.left + ((work.right - work.left) - w) / 2;
    y = work.top + ((work.bottom - work.top) - h) / 2;
    if (y < work.top) y = work.top;
    SetWindowPos(gWnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    ShowWindow(gWnd, SW_SHOW);
    SetForegroundWindow(gWnd);
    SetFocus(gCombo);
}
