/* Classic Appearance Manager preferences. Universal Interfaces stay private
 * to this translation unit; main.cpp continues to use Multiversal. */
#include <Appearance.h>
#include <ControlDefinitions.h>
#include <Controls.h>
#include <Dialogs.h>
#include <Events.h>
#include <Fonts.h>
#include <MacWindows.h>
#include <Menus.h>
#include <Resources.h>
#include <Scrap.h>
#include <Sound.h>
#include <TextEdit.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>
#include "gw_settings.h"
#include "../gw_config.h"
#include "../gw_core.h"
#include "../portable/gw_prefs.h"
#include "../portable/gw_util.h"

namespace {
enum Kind { Check, Number, Date, Text, Redirect, Provider, List };
struct Field {
    short pane;
    Kind kind;
    const char *key, *label, *fallback, *hint;  // hint lines are split on '\n'
};
const short kPaneCount = 8;

/* The pane names, in the order of MENU 200. */
const char *const kPanes[kPaneCount] = {
    "Modules", "Web proxy", "Wayback", "Wayback sites",
    "Mail", "Mail upstream", "OAuth", "Log"
};

/* Standing text at the top of a pane, above its first row. */
const char *const kIntro[kPaneCount] = {
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
const Field kFields[] = {
    { 0, Check, "http_enabled", "Web proxy", "1", "Browse modern sites through the web proxy." },
    { 0, Check, "mail_enabled", "Mail", "1", "Connect a mail client to IMAP, POP and SMTP." },
    { 0, Check, "wayback_enabled", "Wayback proxy", "1", "Browse archived pages from the Wayback Machine." },
    { 1, Number, "http_port", "Port:", "8765", "" },
    { 1, Check, "rewrite_https", "Rewrite https:// links to http://", "1",
      "For browsers without modern TLS support." },
    { 1, Check, "connect_mitm", "Terminate TLS for typed https:// URLs", "0",
      "Requires the Gateway CA in the browser. Choose one mode." },
    { 1, Redirect, "follow_redirects", "Follow redirects:", "auto", "Automatic follows HTTPS redirects." },
    { 1, Number, "max_body_mb", "Largest response (MB):", "0", "0 for no limit." },
    { 1, Number, "max_connects", "Connections opening at once:", "8", "" },
    { 1, Number, "max_sessions", "Concurrent sessions:", "12", "Shared by Web proxy and Wayback; mail has its own limit." },
    { 2, Number, "wayback_port", "Port:", "8888", "0 disables the archive listener." },
    { 2, Date, "wayback_date", "Era (YYYYMMDD):", "20011231", "Also accepts YYYY or YYYYMM." },
    { 2, Number, "wayback_tolerance", "Days newer allowed:", "730", "0 accepts any date." },
    { 2, Number, "wayback_connects", "Connections opening at once:", "1", "The archive refuses bursts." },
    { 2, Check, "wayback_api", "Find the nearest available snapshot", "1",
      "Off requests the configured era directly." },
    { 3, Check, "wayback_geocities", "Send geocities.com to oocities.org", "1", "" },
    { 3, Check, "wayback_cache", "Let the browser keep snapshots", "1", "" },
    { 3, Check, "wayback_settings", "Serve the browser settings page", "1", "" },
    { 3, Check, "wayback_ct_encoding", "Strip charset from Content-Type", "1", "" },
    { 3, Check, "wayback_quick_images", "Quick images (compatibility setting)", "1",
      "Accepted for compatibility; it has no effect." },
    { 3, List, "wayback_live", "Whitelist", "",
      "Separate sites with ; or new lines. Plain names include subdomains.\n"
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
    { 5, Check, "smtp_starttls", "Use STARTTLS on the SMTP port", "1",
      "Port 465 uses TLS immediately; other ports default to STARTTLS." },
    { 6, Text, "oauth_host", "Token host:", "", "" },
    { 6, Text, "oauth_path", "Token path:", "", "" },
    { 6, Text, "oauth_scope", "Scope:", "", "" },
    { 6, Text, "oauth_client_id", "Client ID:", "", "" },
    { 6, Text, "oauth_client_secret", "Client secret:", "", "" },
    { 6, Text, "refresh_token", "Refresh token:", "", "" },
    { 7, Check, "show_window", "Show the log window at launch", "1",
      "Off launches without a window, menu bar or Application\n"
      "menu entry. Send a Quit Apple event to stop Gateway." },
    { 7, Check, "log_file", "Also write the log to a file", "0",
      "The window keeps the last 200 lines.\n"
      "The log file keeps everything." },
};
const int kFieldCount = sizeof(kFields) / sizeof(kFields[0]);
const char *const kRedirects[] = { "auto", "always", "never" };
const char *const kProviders[] = { "outlook", "gmail", "custom" };
const int kValueCapacity = 2048;
const int kListLimit = 2000;

/*
 * Window metrics, in window-local pixels, taken off the Mac OS 9 QuickTime
 * Settings window. Everything below is derived: no pane carries a hand-placed
 * coordinate, so a pane is as tall as its rows and no taller.
 *
 *   selector      the pane pop-up straddles the group frame's top line
 *   group         a primary group 12 pixels in from each window edge
 *   rows          a check box glyph, a 22-pixel field frame or a 20-pixel
 *                 pop-up, all starting at the same y
 *   captions      Geneva 9, 17 pixels below the row and 17 above the next one
 */
const short kWindowWidth   = 460;
const short kMargin        = 12;   // window edge to the group frame
const short kSelectorTop   = 12;
const short kSelectorHeight = 20;
const short kGroupTop      = 21;   // the selector's middle line
const short kGroupLeft     = kMargin;                         // 12
const short kGroupRight    = kWindowWidth - kMargin;          // 448
const short kRowLeft       = kGroupLeft + 16;                 // 28
const short kCheckTextLeft = kRowLeft + 16;                   // 44
const short kFieldLeft     = 244;                             // editable column
const short kFieldRight    = kGroupRight - 16;                // 432
const short kGroupPadTop   = 23;   // group line to the first row
const short kGroupPadBottom = 14;  // last ink to the group line
const short kIntroBase     = kGroupTop + 32;                  // first intro baseline
const short kCaptionPitch  = 13;
const short kCaptionDrop   = 17;   // check box or list bottom to its caption
/* A caption reads against the label above it, not against the control beside
 * it, so field and pop-up captions hang off the label's baseline. Measuring
 * from the control instead pushed them five pixels low, because a field frame
 * is ten pixels deeper than a check box glyph. */
const short kCaptionBelowLabel = 20;
const short kCaptionLift   = 17;   // last caption baseline to the next row
const short kGapAfterCheck = 6;
const short kGapAfterField = 11;
const short kCheckHeight   = 16;   // control rect; the glyph is inset 2
const short kFieldHeight   = 22;   // the framed field
const short kPopupHeight   = 20;
const short kListHeight    = 86;
const short kListLabelDrop = 17;   // the list's label sits above its frame
const short kFieldAscent   = 14;   // field frame top to its text baseline
const short kPopupAscent   = 13;
const short kButtonGap     = 10;   // group frame to the buttons
const short kButtonHeight  = 20;
const short kButtonWidth   = 70;
const short kButtonSpacing = 10;
const short kMarginBottom  = 12;

/*
 * TextEdit erases with the port's background, and this window's background is
 * the platinum dialog brush. Anything that draws into the whitelist -- typing,
 * clicking, scrolling, updating -- has to hand it white first, or the text
 * lands on grey as soon as the view scrolls.
 */
struct White {
    RGBColor saved = {};
    White()
    {
        GetBackColor(&saved);
        RGBColor white = { 0xFFFF, 0xFFFF, 0xFFFF };
        RGBBackColor(&white);
    }
    ~White() { RGBBackColor(&saved); }
};

struct Item {
    ControlHandle control;         // null for the whitelist, which owns its TE
    TEHandle text;
    Rect box;                      // check box: the control rect
                                   // field and list: the framed rectangle
    short labelBase;               // Charcoal baseline, 0 when the control draws it
    short hintBase;                // Geneva baseline of the first caption line
    char original[kValueCapacity];
    bool overflow;                 // Never silently save a truncated value.
};

void Pascal(const char *s, size_t n, Str255 p)
{
    if (n > 255) n = 255;
    p[0] = static_cast<unsigned char>(n);
    std::memcpy(p + 1, s, n);
}

void Pascal(const char *s, Str255 p)
{
    Pascal(s, std::strlen(s), p);
}

short Lines(const char *s)
{
    short n = s[0] ? 1 : 0;
    for (; *s; ++s) if (*s == '\n') ++n;
    return n;
}

void Font(ControlHandle c, short font = kControlFontSmallSystemFont)
{
    ControlFontStyleRec style = {};
    style.flags = kControlUseFontMask;
    style.font = font;
    if (font == kControlFontBigSystemFont) {
        Str255 name;
        short charcoal = 0;
        Pascal("Charcoal", name);
        GetFNum(name, &charcoal);
        if (charcoal) {
            style.flags |= kControlUseSizeMask | kControlUseFaceMask;
            style.font = charcoal;
            style.size = 12;
            style.style = 0;
        }
    }
    SetControlFontStyle(c, &style);
}

bool Editable(const Field &f)
{
    return f.kind == Text || f.kind == Number || f.kind == Date || f.kind == List;
}

/* Every field but the whitelist lives in an Appearance edit-text control. */
bool Framed(const Field &f)
{
    return f.kind == Text || f.kind == Number || f.kind == Date;
}

void ReadValue(const Item &item, const Field &field, char *value)
{
    if (field.kind == List) {
        Size n = item.text ? (*item.text)->teLength : 0;
        if (n < 0 || n >= kValueCapacity) n = kValueCapacity - 1;
        if (n > 0) {
            CharsHandle chars = TEGetText(item.text);
            std::memcpy(value, *chars, n);
        }
        value[n] = 0;
    } else if (Editable(field)) {
        Size n = 0;
        GetControlData(item.control, kControlEntireControl,
                       kControlEditTextTextTag, kValueCapacity - 1, value, &n);
        if (n < 0 || n >= kValueCapacity) n = 0;
        value[n] = 0;
    } else if (field.kind == Check) {
        std::strcpy(value, GetControlValue(item.control) ? "1" : "0");
    } else {
        short n = GetControlValue(item.control);
        if (n < 1 || n > 3) n = 1;
        std::strcpy(value, (field.kind == Provider ? kProviders : kRedirects)[n - 1]);
    }
}

void LoadValues(Item *items)
{
    GWConfig_Load();
    for (int i = 0; i < kFieldCount; ++i) {
        Item &item = items[i];
        const Field &f = kFields[i];
        char value[kValueCapacity] = {};
        item.overflow = false;
        if (f.kind == List) {
            char entry[kValueCapacity];
            for (int n = 0; GWConfig_GetNthSplit(f.key, n, entry, sizeof(entry)); ++n) {
                size_t used = std::strlen(value), len = std::strlen(entry);
                if (used + len + (used ? 1 : 0) > kListLimit) {
                    item.overflow = true;
                    break;
                }
                if (used) std::strcat(value, ";");
                std::strcat(value, entry);
            }
        } else if (f.kind == Check || f.kind == Number || f.kind == Date) {
            long fallback = std::strtol(f.fallback, nullptr, 10);
            if (!std::strcmp(f.key, "smtp_starttls"))
                fallback = GWConfig_Num("smtp_upstream_port", 587) != 465;
            long n = GWConfig_Num(f.key, fallback);
            if (f.kind == Check) n = n != 0;
            std::snprintf(value, sizeof(value), "%ld", n);
        } else {
            const char *current = GWConfig_Str(f.key, f.fallback);
            std::strncpy(value, current, sizeof(value) - 1);
        }
        std::strcpy(item.original, value);
        if (f.kind == List) {
            if (item.text) {
                TESetText(value, std::strlen(value), item.text);
                (*item.text)->destRect = (*item.text)->viewRect;
                TECalText(item.text);
                TESetSelect(0, 0, item.text);
            }
        } else if (Editable(f)) {
            SetControlData(item.control, kControlEntireControl,
                           kControlEditTextTextTag, std::strlen(value), value);
            // The framed fields are single-line and scroll horizontally.
            if (item.text) {
                (*item.text)->crOnly = -1;
                TECalText(item.text);
                TEAutoView(true, item.text);
                TESetSelect(0, 0, item.text);
                TESelView(item.text);
            }
        } else if (f.kind == Check) {
            SetControlValue(item.control, value[0] == '1');
        } else {
            const char *const *choices = f.kind == Provider ? kProviders : kRedirects;
            short chosen = 1;
            for (short n = 0; n < 3; ++n)
                if (!gw_stricmp(value, choices[n])) chosen = n + 1;
            SetControlValue(item.control, chosen);
        }
    }
}

class Preferences {
public:
    DialogPtr dialog = nullptr;
    WindowPtr window = nullptr;
    ControlHandle selector = nullptr, save = nullptr;
    ControlHandle cancel = nullptr, revert = nullptr, scroll = nullptr;
    MenuHandle menus[3] = {};
    Item items[kFieldCount] = {};
    short pane = 0, focus = -1, listIndex = -1;
    short paneHeight[kPaneCount] = {};   // window height, per pane
    short paneBottom[kPaneCount] = {};   // group frame bottom, per pane
    short tallest = 0;
    Rect bounds = { 0, 0, 0, kWindowWidth };
    Rect paneFrame = { kGroupTop, kGroupLeft, 0, kGroupRight };
    Rect listFrame = {};
    Rect scrollFrame = {};
    Rect selectorFrame = {};

    ControlHandle Control(const Rect &rect, const char *label, short proc,
                          short minimum = 0, short maximum = 1,
                          short font = kControlFontSmallSystemFont)
    {
        Str255 title;
        Pascal(label, title);
        ControlHandle c = NewControl(window, &rect, title, false,
                                      0, minimum, maximum, proc, 0);
        if (c) Font(c, font);
        return c;
    }

    void LabelFont()
    {
        Str255 name;
        short charcoal = 0;
        Pascal("Charcoal", name); GetFNum(name, &charcoal);
        TextFont(charcoal); TextSize(12); TextFace(0);
    }

    void HintFont()
    {
        TextFont(3); TextSize(9); TextFace(0);
    }

    /* Walk every pane once and record where each row, caption and frame goes. */
    void Layout()
    {
        for (short p = 0; p < kPaneCount; ++p) {
            short y = kGroupTop + kGroupPadTop;
            short ink = y;
            if (kIntro[p][0]) {
                short last = kIntroBase + kCaptionPitch * (Lines(kIntro[p]) - 1);
                y = last + kCaptionLift;
                ink = last;
            }
            // A caption already carries the row on to the next one; only a
            // row that ends in its control needs a gap added.
            bool gap = false;
            Kind previous = Check;
            for (int i = 0; i < kFieldCount; ++i) {
                const Field &f = kFields[i];
                if (f.pane != p) continue;
                Item &item = items[i];
                if (gap)
                    y += (previous == Check ? kGapAfterCheck : kGapAfterField);
                previous = f.kind;
                item.labelBase = 0;
                item.hintBase = 0;
                short bottom;      // the row's last drawn line
                short caption;     // baseline of its first caption line
                switch (f.kind) {
                case Check:
                    item.box.top = y; item.box.left = kRowLeft;
                    item.box.bottom = static_cast<short>(y + kCheckHeight);
                    item.box.right = static_cast<short>(kGroupRight - 6);
                    bottom = static_cast<short>(y + kCheckHeight - 2);
                    caption = static_cast<short>(bottom + kCaptionDrop);
                    break;
                case List:
                    item.labelBase = static_cast<short>(y + 11);
                    y = static_cast<short>(y + kListLabelDrop);
                    item.box.top = y; item.box.left = static_cast<short>(kRowLeft - 3);
                    item.box.bottom = static_cast<short>(y + kListHeight);
                    item.box.right = static_cast<short>(kFieldRight - 15);
                    bottom = item.box.bottom;
                    caption = static_cast<short>(bottom + kCaptionDrop);
                    break;
                case Redirect:
                case Provider:
                    item.labelBase = static_cast<short>(y + kPopupAscent);
                    item.box.top = y; item.box.left = static_cast<short>(kFieldLeft - 3);
                    item.box.bottom = static_cast<short>(y + kPopupHeight);
                    item.box.right = static_cast<short>(kFieldRight + 3);
                    bottom = item.box.bottom;
                    caption = static_cast<short>(item.labelBase + kCaptionBelowLabel);
                    break;
                default:
                    item.labelBase = static_cast<short>(y + kFieldAscent);
                    item.box.top = y; item.box.left = static_cast<short>(kFieldLeft - 3);
                    item.box.bottom = static_cast<short>(y + kFieldHeight);
                    item.box.right = static_cast<short>(kFieldRight + 3);
                    bottom = item.box.bottom;
                    caption = static_cast<short>(item.labelBase + kCaptionBelowLabel);
                    break;
                }
                if (f.hint[0]) {
                    item.hintBase = caption;
                    short last = static_cast<short>(caption +
                                 kCaptionPitch * (Lines(f.hint) - 1));
                    y = static_cast<short>(last + kCaptionLift);
                    ink = last;
                    gap = false;
                } else {
                    y = item.box.bottom;
                    ink = bottom;
                    gap = true;
                }
            }
            paneBottom[p] = static_cast<short>(ink + kGroupPadBottom);
            paneHeight[p] = static_cast<short>(paneBottom[p] + kButtonGap +
                                               kButtonHeight + kMarginBottom);
            if (paneHeight[p] > tallest) tallest = paneHeight[p];
        }
    }

    /* As wide as the longest pane name, plus the pop-up's own furniture. */
    short SelectorWidth()
    {
        short widest = 0;
        LabelFont();
        for (short n = 0; n < kPaneCount; ++n) {
            Str255 p;
            Pascal(kPanes[n], p);
            short w = StringWidth(p);
            if (w > widest) widest = w;
        }
        return static_cast<short>(widest + 34);
    }

    bool Open()
    {
        Layout();
        bounds.bottom = tallest;
        Rect screen = qd.screenBits.bounds;
        Rect position = bounds;
        // Placed for the tallest pane so that resizing never walks the window
        // off the screen; the title bar has to clear the menu bar as well.
        short left = static_cast<short>(screen.left +
                                        (screen.right - screen.left - kWindowWidth) / 2);
        short top = static_cast<short>(screen.top +
                                       (screen.bottom - screen.top - tallest) / 2);
        if (top < screen.top + 46) top = static_cast<short>(screen.top + 46);
        OffsetRect(&position, left, top);
        Handle layout = GetResource('DITL', 209);
        if (!layout || HandToHand(&layout) != noErr) return false;
        Str255 title;
        Pascal("Gateway Preferences", title);
        // A document window with a close box, the same defproc About Gateway
        // uses. The Mac OS 9 windows this copies -- QuickTime Settings, the
        // Internet control panel -- are document windows too, which is where
        // the recessed Platinum frame and the title bar's close and collapse
        // boxes come from; neither dialog defproc draws them.
        // Invisible until the first pane has sized it; the close box is on.
        dialog = NewColorDialog(nullptr, &position, title, false,
                                noGrowDocProc,
                                reinterpret_cast<WindowPtr>(-1L),
                                true, 0, layout);
        if (!dialog) { DisposeHandle(layout); return false; }
        window = GetDialogWindow(dialog);
        SetPort(window);
        SetThemeWindowBackground(window, kThemeBrushDialogBackgroundActive, false);
        HintFont();
        ControlHandle root;
        if (CreateRootControl(window, &root) != noErr) return false;
        for (short n = 0; n < 3; ++n) {
            menus[n] = GetMenu(200 + n);
            if (!menus[n]) return false;
            InsertMenu(menus[n], -1);
        }
        Rect r;
        r.top = kSelectorTop; r.left = kRowLeft;
        r.bottom = kSelectorTop + kSelectorHeight;
        r.right = static_cast<short>(kRowLeft + SelectorWidth());
        selectorFrame = r;
        selector = Control(r, "", kControlPopupButtonProc | kControlPopupFixedWidthVariant, 200, 0,
                           kControlFontBigSystemFont);
        r.top = 0; r.bottom = kButtonHeight;
        r.right = kGroupRight; r.left = static_cast<short>(kGroupRight - kButtonWidth);
        save = Control(r, "Save", kControlPushButtonProc, 0, 1, kControlFontBigSystemFont);
        OffsetRect(&r, static_cast<short>(-(kButtonWidth + kButtonSpacing)), 0);
        cancel = Control(r, "Cancel", kControlPushButtonProc, 0, 1, kControlFontBigSystemFont);
        OffsetRect(&r, static_cast<short>(-(kButtonWidth + kButtonSpacing)), 0);
        revert = Control(r, "Undo", kControlPushButtonProc, 0, 1, kControlFontBigSystemFont);
        if (!selector || !revert || !cancel || !save) return false;
        Boolean yes = true;
        SetControlData(save, kControlEntireControl, kControlPushButtonDefaultTag,
                       sizeof(yes), &yes);
        SetControlValue(selector, 1);
        for (int i = 0; i < kFieldCount; ++i) {
            const Field &f = kFields[i];
            Item &item = items[i];
            if (f.kind == List) {
                listIndex = i;
                listFrame = item.box;
                // The whitelist is a plain TextEdit record. The Appearance
                // edit-text CDEF is single-line: wrapping the TE it owns is
                // what crashed this pane as soon as a line was typed.
                LabelFont();
                Rect view = item.box;
                InsetRect(&view, 3, 3);
                item.text = TENew(&view, &view);
                if (!item.text) return false;
                (*item.text)->crOnly = 0;          // wrap on the view's width
                // TESelView only scrolls while automatic viewing is on, and
                // typing has to keep the caret in sight.
                TEAutoView(true, item.text);
                HintFont();
                // Flush with the frame's top and bottom and sharing its
                // right-hand pixel, so the two read as a single recessed well.
                scrollFrame.top = listFrame.top;
                scrollFrame.bottom = listFrame.bottom;
                scrollFrame.left = static_cast<short>(listFrame.right - 1);
                scrollFrame.right = static_cast<short>(listFrame.right + 15);
                scroll = Control(scrollFrame, "", kControlScrollBarProc);
                if (!scroll) return false;
                continue;
            }
            Rect box = item.box;
            short proc = kControlCheckBoxAutoToggleProc;
            short minimum = 0, maximum = 1;
            if (Framed(f)) {
                // The native edit CDEF frames outside its text rectangle.
                InsetRect(&box, 3, 3);
                proc = kControlEditTextProc;
            } else if (f.kind == Redirect || f.kind == Provider) {
                proc = kControlPopupButtonProc | kControlPopupFixedWidthVariant;
                minimum = f.kind == Redirect ? 201 : 202;
                maximum = 0;
            }
            item.control = Control(box, f.kind == Check ? f.label : "",
                                   proc, minimum, maximum,
                                   kControlFontBigSystemFont);
            if (!item.control) return false;
            if (Framed(f)) {
                Size actual;
                if (GetControlData(item.control, kControlEntireControl,
                                   kControlEditTextTEHandleTag,
                                   sizeof(item.text), &item.text, &actual) != noErr ||
                    !item.text) return false;
            }
        }
        LoadValues(items);
        ShowControl(selector);
        ShowControl(save); ShowControl(cancel); ShowControl(revert);
        SwitchPane(0);
        ShowWindow(window); SelectWindow(window);
        return true;
    }

    void SwitchPane(short next)
    {
        ClearKeyboardFocus(window);
        if (focus == listIndex && listIndex >= 0 && items[listIndex].text)
            TEDeactivate(items[listIndex].text);
        focus = -1;
        for (int i = 0; i < kFieldCount; ++i)
            if (items[i].control) HideControl(items[i].control);
        HideControl(scroll);
        pane = next;
        SetControlValue(selector, pane + 1);
        Resize();
        for (int i = 0; i < kFieldCount; ++i)
            if (kFields[i].pane == pane && items[i].control) ShowControl(items[i].control);
        if (listIndex >= 0 && kFields[listIndex].pane == pane) {
            ShowControl(scroll); SyncScroll();
        }
        InvalRect(&bounds);
    }

    /* The window is exactly as tall as the pane on show. */
    void Resize()
    {
        paneFrame.bottom = paneBottom[pane];
        short height = paneHeight[pane];
        short top = static_cast<short>(paneFrame.bottom + kButtonGap);
        if (height != bounds.bottom) {
            bounds.bottom = height;
            SizeWindow(window, kWindowWidth, height, true);
        }
        MoveControl(revert, static_cast<short>(kGroupRight - 3 * kButtonWidth - 2 * kButtonSpacing), top);
        MoveControl(cancel, static_cast<short>(kGroupRight - 2 * kButtonWidth - kButtonSpacing), top);
        MoveControl(save, static_cast<short>(kGroupRight - kButtonWidth), top);
    }

    /* The whitelist's focus ring is drawn outside its frame, so redraw with
     * room for it whenever focus arrives at or leaves the list. */
    void InvalList()
    {
        if (listIndex < 0 || kFields[listIndex].pane != pane) return;
        Rect ring = ListRing();
        InsetRect(&ring, -4, -4);
        InvalRect(&ring);
    }

    /* The whitelist and its scroll bar are one field: the focus ring goes
     * round both, not down the seam between them. The well's highlight runs
     * along its bottom and right edges, and a light pixel between the frame
     * and the ring reads as a gap where the shadow above and to the left
     * reads as part of the frame, so the ring closes up by one there. */
    Rect ListRing() const
    {
        Rect ring = listFrame;
        ring.right = static_cast<short>(scrollFrame.right - 1);
        ring.bottom = static_cast<short>(listFrame.bottom - 1);
        return ring;
    }

    void Focus(short i)
    {
        if (focus == listIndex && listIndex >= 0 && items[listIndex].text)
            TEDeactivate(items[listIndex].text);
        focus = i;
        if (i == listIndex) {
            ClearKeyboardFocus(window);
            TEActivate(items[i].text);
        } else {
            SetKeyboardFocus(window, items[i].control, kControlEditTextPart);
        }
        InvalList();
    }

    void Line(short x, short y, const char *text, size_t n)
    {
        Str255 p; Pascal(text, n, p); MoveTo(x, y); DrawString(p);
    }

    /* Caption text, one DrawString per line so the leading is exact. */
    void Caption(short x, short base, const char *text)
    {
        while (*text) {
            const char *stop = std::strchr(text, '\n');
            size_t n = stop ? static_cast<size_t>(stop - text) : std::strlen(text);
            Line(x, base, text, n);
            base = static_cast<short>(base + kCaptionPitch);
            if (!stop) break;
            text = stop + 1;
        }
    }

    void Pen(unsigned short level)
    {
        RGBColor c; c.red = c.green = c.blue = level; RGBForeColor(&c);
    }

    void DrawList()
    {
        Item &item = items[listIndex];
        White white;
        Rect interior = listFrame;
        InsetRect(&interior, 1, 1);
        Pen(0xFFFF); PaintRect(&interior);
        Pen(0x0000); FrameRect(&listFrame);
        // A recessed well: shadow above and left, highlight below. The shadow
        // runs on over the scroll bar so the two share one top edge.
        Pen(0x7D7D);
        MoveTo(static_cast<short>(listFrame.left - 1), static_cast<short>(listFrame.top - 1));
        LineTo(static_cast<short>(scrollFrame.right - 1), static_cast<short>(listFrame.top - 1));
        MoveTo(static_cast<short>(listFrame.left - 1), static_cast<short>(listFrame.top - 1));
        LineTo(static_cast<short>(listFrame.left - 1), static_cast<short>(listFrame.bottom - 1));
        Pen(0xFFFF);
        MoveTo(listFrame.left, listFrame.bottom);
        LineTo(scrollFrame.right, listFrame.bottom);
        Pen(0x0000);
        TEUpdate(&(*item.text)->viewRect, item.text);
        if (focus == listIndex) {
            Rect ring = ListRing();
            DrawThemeFocusRect(&ring, true);
        }
    }

    void Draw()
    {
        EraseRect(&bounds);
        // The pane's frame, with its selector replacing the title.
        DrawThemePrimaryGroup(&paneFrame, kThemeStateActive);
        // The group's top line runs behind the selector; clear it first.
        Rect selectorGround = selectorFrame;
        InsetRect(&selectorGround, -3, -3);
        EraseRect(&selectorGround);
        DrawControls(window);
        if (kIntro[pane][0]) {
            HintFont();
            Caption(kRowLeft, kIntroBase, kIntro[pane]);
        }
        for (int i = 0; i < kFieldCount; ++i) {
            const Field &f = kFields[i];
            if (f.pane != pane) continue;
            const Item &item = items[i];
            if (item.labelBase) {
                LabelFont();
                Line(kRowLeft, item.labelBase, f.label, std::strlen(f.label));
            }
            if (item.hintBase) {
                HintFont();
                Caption(f.kind == Check ? kCheckTextLeft : kRowLeft,
                        item.hintBase, f.hint);
            }
        }
        if (listIndex >= 0 && kFields[listIndex].pane == pane) DrawList();
        HintFont();
    }

    void SyncScroll()
    {
        White white;
        TEHandle te = items[listIndex].text;
        int height = (*te)->viewRect.bottom - (*te)->viewRect.top;
        int line = (*te)->lineHeight > 0 ? (*te)->lineHeight : 12;
        int maximum = (*te)->nLines * line - height;
        if (maximum < 0) maximum = 0;
        int offset = (*te)->viewRect.top - (*te)->destRect.top;
        if (offset > maximum) { TEScroll(0, offset - maximum, te); offset = maximum; }
        if (offset < 0) { TEScroll(0, offset, te); offset = 0; }
        SetControlMaximum(scroll, maximum);
        SetControlValue(scroll, offset);
    }

    void Scroll(short delta)
    {
        White white;
        int old = GetControlValue(scroll);
        int value = old + delta;
        if (value < 0) value = 0;
        if (value > GetControlMaximum(scroll)) value = GetControlMaximum(scroll);
        TEScroll(0, old - value, items[listIndex].text);
        SetControlValue(scroll, value);
    }

    static pascal void ScrollAction(ControlHandle control, short part)
    {
        Preferences *self = reinterpret_cast<Preferences *>(GetControlReference(control));
        int line = (*self->items[self->listIndex].text)->lineHeight;
        if (line < 1) line = 12;
        short delta = 0;
        if (part == kControlUpButtonPart) delta = -line;
        if (part == kControlDownButtonPart) delta = line;
        if (part == kControlPageUpPart) delta = -60;
        if (part == kControlPageDownPart) delta = 60;
        self->Scroll(delta);
    }

    bool Save()
    {
        char value[kValueCapacity];
        // Validate every pane before writing anything.
        for (short i = 0; i < kFieldCount; ++i) {
            const Field &f = kFields[i];
            ReadValue(items[i], f, value);
            bool valid = true;
            if (items[i].overflow) {
                // An oversized on-disk list can be kept, but not truncated by Save.
                valid = !std::strcmp(value, items[i].original);
            }
            if (f.kind == Number || f.kind == Date) {
                unsigned long n = 0;
                valid = value[0] != 0;
                for (const char *p = value; valid && *p; ++p) {
                    if (*p < '0' || *p > '9' || n > 214748364UL) valid = false;
                    else { n = n * 10 + (*p - '0'); if (n > 2147483647UL) valid = false; }
                }
                if (f.kind == Date)
                    valid = valid && (std::strlen(value) == 4 || std::strlen(value) == 6 || std::strlen(value) == 8);
                else if (std::strstr(f.key, "port"))
                    valid = valid && n <= 65535 && (n > 0 || !std::strcmp(f.key, "wayback_port"));
                else if (std::strcmp(f.key, "max_body_mb") && std::strcmp(f.key, "wayback_tolerance"))
                    valid = valid && n > 0;
            }
            if (!valid) {
                SwitchPane(f.pane); Focus(i); SysBeep(1);
                return false;
            }
        }
        // Write edits across all panes. Preserve untouched provider defaults and
        // any refresh token rotated by the live core while this window was open.
        for (int i = 0; i < kFieldCount; ++i) {
            ReadValue(items[i], kFields[i], value);
            if (!std::strcmp(value, items[i].original)) continue;
            if (kFields[i].kind == List) gw_prefs_normalize_list(value);
            if (!GWConfig_Set(kFields[i].key, value)) {
                SysBeep(1);
                return false;
            }
            std::strcpy(items[i].original, value);
        }
        GWConfig_Load(); GW_LoadSettings();
        return true;
    }

    ~Preferences()
    {
        if (listIndex >= 0 && items[listIndex].text) TEDispose(items[listIndex].text);
        // Controls and their TextEdit records belong to the dialog window.
        if (dialog) DisposeDialog(dialog);
        for (short i = 0; i < 3; ++i) if (menus[i]) {
            DeleteMenu(200 + i); DisposeMenu(menus[i]);
        }
    }
};
} // namespace

void GWSettings_Run(int (*serviceEvent)(void *, void *), void *context)
{
    GrafPtr savedPort;
    GetPort(&savedPort);
    RegisterAppearanceClient();
    // Heap allocation is intentional: no global constructors on Retro68 PPC.
    Preferences *p = new (std::nothrow) Preferences;
    if (!p || !p->Open()) {
        delete p; SetPort(savedPort); UnregisterAppearanceClient(); SysBeep(1);
        return;
    }
    SetControlReference(p->scroll, reinterpret_cast<long>(p));
    ControlActionUPP scrollAction = NewControlActionUPP(Preferences::ScrollAction);
    bool done = false;
    while (!done) {
        EventRecord event;
        WaitNextEvent(everyEvent, &event, 5, nullptr);
        GW_Poll();
        SetPort(p->window);
        IdleControls(p->window);
        if (p->focus >= 0 && p->focus == p->listIndex)
            TEIdle(p->items[p->listIndex].text);
        if (event.what == updateEvt && reinterpret_cast<WindowPtr>(event.message) == p->window) {
            BeginUpdate(p->window); p->Draw(); EndUpdate(p->window);
        } else if (event.what == mouseDown) {
            WindowPtr hitWindow;
            short part = FindWindow(event.where, &hitWindow);
            if (hitWindow != p->window) { SysBeep(1); continue; }
            if (part == inGoAway) {
                if (TrackGoAway(p->window, event.where)) done = true;
                continue;
            }
            if (part == inDrag) {
                Rect limit = qd.screenBits.bounds; InsetRect(&limit, 4, 4);
                DragWindow(p->window, event.where, &limit);
                continue;
            }
            if (part != inContent) continue;
            Point point = event.where; GlobalToLocal(&point);
            bool edited = false;
            for (short i = 0; i < kFieldCount; ++i) {
                if (kFields[i].pane != p->pane || !Editable(kFields[i])) continue;
                Rect hitBox = p->items[i].box;
                // The whitelist shares its right-hand frame pixel with the
                // scroll bar; leave that column to the scroll bar.
                if (kFields[i].kind == List) InsetRect(&hitBox, 1, 1);
                if (!PtInRect(point, &hitBox)) continue;
                p->Focus(i);
                if (kFields[i].kind == List) {
                    { White white;
                      TEClick(point, (event.modifiers & shiftKey) != 0,
                              p->items[i].text); }
                    p->SyncScroll();
                } else {
                    // Track the click as well as changing focus: this places the caret.
                    HandleControlClick(p->items[i].control, point, event.modifiers, nullptr);
                }
                edited = true;
                break;
            }
            if (edited) continue;
            ControlHandle hit = nullptr;
            part = FindControl(point, p->window, &hit);
            if (!hit || !part) continue;
            if (hit == p->scroll) {
                if (part == kControlIndicatorPart) {
                    int old = GetControlValue(hit);
                    TrackControl(hit, point, nullptr);
                    White white;
                    TEScroll(0, old - GetControlValue(hit), p->items[p->listIndex].text);
                } else {
                    TrackControl(hit, point, scrollAction);
                }
                continue;
            }
            if (!HandleControlClick(hit, point, event.modifiers, nullptr)) continue;
            if (hit == p->selector) {
                short selected = GetControlValue(hit) - 1;
                if (selected >= 0 && selected < kPaneCount) p->SwitchPane(selected);
            } else if (hit == p->cancel) done = true;
            else if (hit == p->save) done = p->Save();
            else if (hit == p->revert) {
                LoadValues(p->items);
                p->SwitchPane(p->pane);
            }
        } else if (event.what == keyDown || event.what == autoKey) {
            char ch = event.message & charCodeMask;
            bool command = (event.modifiers & cmdKey) != 0;
            bool inList = p->focus >= 0 && p->focus == p->listIndex;
            if (ch == 27 || (command && ch == '.')) { done = true; continue; }
            if (ch == 3 || (ch == '\r' && !inList)) { done = p->Save(); continue; }
            if (ch == '\t') {
                int step = event.modifiers & shiftKey ? -1 : 1;
                int start = p->focus >= 0 ? p->focus : (step > 0 ? kFieldCount - 1 : 0);
                for (int n = 1; n <= kFieldCount; ++n) {
                    short i = (start + step * n + kFieldCount) % kFieldCount;
                    if (kFields[i].pane == p->pane && Editable(kFields[i])) {
                        p->Focus(i); break;
                    }
                }
                continue;
            }
            if (p->focus < 0) continue;
            Item &item = p->items[p->focus];
            const Field &field = kFields[p->focus];
            TEHandle te = item.text;
            RGBColor savedBack = {};
            if (inList) {
                GetBackColor(&savedBack);
                RGBColor white = { 0xFFFF, 0xFFFF, 0xFFFF };
                RGBBackColor(&white);
            }
            int limit = field.kind == List ? kListLimit : kValueCapacity - 1;
            int remaining = limit - ((*te)->teLength - ((*te)->selEnd - (*te)->selStart));
            if (command) {
                if (ch == 'a') TESetSelect(0, (*te)->teLength, te);
                else if (ch == 'c' || ch == 'x') {
                    TECopy(te);
                    ZeroScrap(); TEToScrap();
                    if (ch == 'x') TEDelete(te);
                } else if (ch == 'v') {
                    Handle scrap = NewHandle(0);
                    if (!scrap) {
                        SysBeep(1);
                        if (inList) RGBBackColor(&savedBack);
                        continue;
                    }
                    SInt32 offset = 0;
                    long count = GetScrap(scrap, 'TEXT', &offset);
                    bool valid = count >= 0 && count <= remaining;
                    if (valid) {
                        HLock(scrap);
                        for (long n = 0; n < count; ++n) {
                            unsigned char c = static_cast<unsigned char>((*scrap)[n]);
                            if (field.kind == Number || field.kind == Date) {
                                if (c < '0' || c > '9') valid = false;
                            } else if (field.kind != List && (c == '\r' || c == '\n')) valid = false;
                            if (c == 0) valid = false;
                            if (field.kind == List && c == '\n') (*scrap)[n] = '\r';
                        }
                        if (valid) { TEDelete(te); TEInsert(*scrap, count, te); }
                        HUnlock(scrap);
                    }
                    DisposeHandle(scrap);
                    if (!valid) {
                        SysBeep(1);
                        if (inList) RGBBackColor(&savedBack);
                        continue;
                    }
                }
            } else {
                bool numeric = field.kind == Number || field.kind == Date;
                unsigned char key = static_cast<unsigned char>(ch);
                bool navigation = key == 8 || (key >= 28 && key <= 31) || key == 127;
                bool newline = inList && key == 13;
                if (!navigation && (remaining <= 0 || (!newline &&
                    ((numeric && (ch < '0' || ch > '9')) || key < 32)))) {
                    SysBeep(1);
                    if (inList) RGBBackColor(&savedBack);
                    continue;
                }
                if (inList) {
                    if (key == 30 || key == 31) {
                        // Scrolling must not be undone by TESelView below.
                        p->Scroll(static_cast<short>(key == 30 ? -(*te)->lineHeight
                                                               : (*te)->lineHeight));
                        RGBBackColor(&savedBack);
                        continue;
                    }
                    if (key == 28 || key == 29) {
                        // Classic TextEdit leaves the arrow keys to the caller.
                        short at = key == 28 ? (*te)->selStart : (*te)->selEnd;
                        if ((*te)->selStart == (*te)->selEnd) {
                            if (key == 28 && at > 0) --at;
                            if (key == 29 && at < (*te)->teLength) ++at;
                        }
                        TESetSelect(at, at, te);
                    } else {
                        TEKey(ch, te);
                    }
                } else {
                    HandleControlKey(item.control, (event.message & keyCodeMask) >> 8,
                                     key, event.modifiers);
                }
            }
            TESelView(te);
            if (inList) {
                p->SyncScroll();
                RGBBackColor(&savedBack);
            } else {
                Draw1Control(item.control);
            }
        } else {
            if (serviceEvent(context, &event)) done = true;
        }
    }
    DisposeControlActionUPP(scrollAction);
    delete p;
    SetPort(savedPort);
    UnregisterAppearanceClient();
}
