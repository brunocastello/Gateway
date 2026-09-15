/* Classic Appearance Manager preferences. Universal Interfaces stay private
 * to this translation unit; main.cpp continues to use Multiversal. */
#include <Appearance.h>
#include <Aliases.h>
#include <AppleEvents.h>
#include <Folders.h>
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
    const char *key, *label, *fallback, *hint;
};
/* Binding order matches the slots in DITL 210-217. */
const Field kFields[] = {
    { 0, Check, "http_enabled", "Web proxy", "1", "Browse modern sites through the web proxy." },
    { 0, Check, "mail_enabled", "Mail", "1", "Connect a mail client to IMAP, POP and SMTP." },
    { 0, Check, "wayback_enabled", "Wayback proxy", "1", "Browse archived pages from the Wayback Machine." },
    { 1, Number, "http_port", "Port:", "8765", "" },
    { 1, Check, "rewrite_https", "Rewrite https:// links to http://", "1", "" },
    { 1, Check, "connect_mitm", "Terminate TLS for typed https:// URLs", "0", "" },
    { 1, Redirect, "follow_redirects", "Follow redirects:", "auto", "Automatic follows HTTPS redirects." },
    { 1, Number, "max_body_mb", "Largest response (MB):", "0", "0 for no limit." },
    { 1, Number, "max_connects", "Connections opening at once:", "8", "" },
    { 1, Number, "max_sessions", "Concurrent sessions:", "12", "Shared by Web proxy and Wayback; mail has its own limit." },
    { 2, Number, "wayback_port", "Port:", "8888", "0 disables the archive listener." },
    { 2, Date, "wayback_date", "Era (YYYYMMDD):", "20011231", "Also accepts YYYY or YYYYMM." },
    { 2, Number, "wayback_tolerance", "Days newer allowed:", "730", "0 accepts any date." },
    { 2, Number, "wayback_connects", "Connections opening at once:", "1", "The archive refuses bursts." },
    { 2, Check, "wayback_api", "Find the nearest available snapshot", "1", "" },
    { 3, Check, "wayback_geocities", "Send geocities.com to oocities.org", "1", "" },
    { 3, Check, "wayback_cache", "Let the browser keep snapshots", "1", "" },
    { 3, Check, "wayback_settings", "Serve the browser settings page", "1", "" },
    { 3, Check, "wayback_ct_encoding", "Strip charset from Content-Type", "1", "" },
    { 3, Check, "wayback_quick_images", "Quick images (compatibility setting)", "1", "" },
    { 3, List, "wayback_live", "Whitelist", "", "" },
    { 4, Provider, "provider", "Provider:", "outlook", "" },
    { 4, Text, "oauth_user", "Address:", "", "" },
    { 4, Text, "local_password", "Password for the mail client:", "", "" },
    { 4, Number, "imap_port", "IMAP port:", "1993", "" },
    { 4, Number, "pop_port", "POP port:", "1995", "" },
    { 4, Number, "smtp_port", "SMTP port:", "1587", "" },
    { 5, Text, "imap_host", "IMAP host:", "", "" },
    { 5, Number, "imap_upstream_port", "IMAP port:", "993", "" },
    { 5, Text, "pop_host", "POP host:", "", "" },
    { 5, Number, "pop_upstream_port", "POP port:", "995", "" },
    { 5, Text, "smtp_host", "SMTP host:", "", "" },
    { 5, Number, "smtp_upstream_port", "SMTP port:", "587", "" },
    { 5, Check, "smtp_starttls", "Use STARTTLS on the SMTP port", "1", "" },
    { 6, Text, "oauth_host", "Token host:", "", "" },
    { 6, Text, "oauth_path", "Token path:", "", "" },
    { 6, Text, "oauth_scope", "Scope:", "", "" },
    { 6, Text, "oauth_client_id", "Client ID:", "", "" },
    { 6, Text, "oauth_client_secret", "Client secret:", "", "" },
    { 6, Text, "refresh_token", "Refresh token:", "", "" },
    { 7, Check, "show_window", "Show the log window at launch", "1", "" },
    { 7, Check, "log_file", "Also write the log to a file", "0", "" },
};
const int kFieldCount = sizeof(kFields) / sizeof(kFields[0]);
const char *const kRedirects[] = { "auto", "always", "never" };
const char *const kProviders[] = { "outlook", "gmail", "custom" };
const int kValueCapacity = 2048;
const int kListLimit = 2000;

struct Item {
    ControlHandle control;
    TEHandle text;                 // Owned by the Appearance edit control.
    Rect box;                      // Outer field box from its DITL slot.
    char original[kValueCapacity];
    bool overflow;                 // Never silently save a truncated value.
};

void Pascal(const char *s, Str255 p)
{
    size_t n = std::strlen(s);
    if (n > 255) n = 255;
    p[0] = static_cast<unsigned char>(n);
    std::memcpy(p + 1, s, n);
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

void ReadValue(const Item &item, const Field &field, char *value)
{
    if (Editable(field)) {
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
        if (Editable(f)) {
            SetControlData(item.control, kControlEntireControl,
                           kControlEditTextTextTag, std::strlen(value), value);
            // Single-line controls scroll horizontally. Only the whitelist wraps.
            if (item.text) {
                (*item.text)->crOnly = f.kind == List ? 0 : -1;
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
    const char *notice = "";
    Rect bounds = { 0, 0, 400, 460 };
    // The old 16px outer / 30px control margins are reduced by one third.
    Rect paneFrame = { 32, 11, 354, 449 };
    Rect logLink = { 169, 38, 184, 220 };
    static const short kRowLeft = 20;
    static const short kCheckTextLeft = 38;

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

    bool Open()
    {
        Rect screen = qd.screenBits.bounds;
        Rect position = bounds;
        OffsetRect(&position, (screen.right - 460) / 2,
                   screen.bottom > 460 ? (screen.bottom - 400) / 2 : 40);
        Handle layout = GetResource('DITL', 209);
        if (!layout || HandToHand(&layout) != noErr) return false;
        Str255 title;
        Pascal("Gateway Preferences", title);
        dialog = NewColorDialog(nullptr, &position, title, false,
                                movableDBoxProc, reinterpret_cast<WindowPtr>(-1L),
                                false, 0, layout);
        if (!dialog) { DisposeHandle(layout); return false; }
        window = GetDialogWindow(dialog);
        SetPort(window);
        SetThemeWindowBackground(window, kThemeBrushDialogBackgroundActive, false);
        TextFont(3); TextSize(9);
        ControlHandle root;
        if (CreateRootControl(window, &root) != noErr) return false;
        for (short n = 0; n < 3; ++n) {
            menus[n] = GetMenu(200 + n);
            if (!menus[n]) return false;
            InsertMenu(menus[n], -1);
        }
        Rect r = { 22, 20, 42, 202 };
        selector = Control(r, "", kControlPopupButtonProc | kControlPopupFixedWidthVariant, 200, 0,
                           kControlFontBigSystemFont);
        r = { 370, 219, 390, 289 };
        revert = Control(r, "Revert", kControlPushButtonProc, 0, 1,
                         kControlFontBigSystemFont);
        r = { 370, 299, 390, 369 };
        cancel = Control(r, "Cancel", kControlPushButtonProc, 0, 1,
                         kControlFontBigSystemFont);
        r = { 370, 379, 390, 449 };
        save = Control(r, "Save", kControlPushButtonProc, 0, 1,
                       kControlFontBigSystemFont);
        if (!selector || !revert || !cancel || !save) return false;
        Boolean yes = true;
        SetControlData(save, kControlEntireControl, kControlPushButtonDefaultTag,
                       sizeof(yes), &yes);
        SetControlValue(selector, 1);
        short slot = 1;
        for (short p = 0; p < 8; ++p) {
            Handle ditl = GetResource('DITL', 210 + p);
            if (!ditl) return false;
            AppendDITL(dialog, ditl, overlayDITL);
            ReleaseResource(ditl);
            for (int i = 0; i < kFieldCount; ++i) {
                const Field &f = kFields[i];
                if (f.pane != p) continue;
                Item &item = items[i];
                short type; Handle unused;
                GetDialogItem(dialog, slot++, &type, &unused, &item.box);
                Rect box = item.box;
                short proc = kControlCheckBoxAutoToggleProc;
                short minimum = 0, maximum = 1;
                if (Editable(f)) {
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
                                       !Editable(f) ? kControlFontBigSystemFont :
                                                         kControlFontSmallSystemFont);
                if (!item.control) return false;
                if (Editable(f)) {
                    Size actual;
                    if (GetControlData(item.control, kControlEntireControl,
                                       kControlEditTextTEHandleTag,
                                       sizeof(item.text), &item.text, &actual) != noErr ||
                        !item.text) return false;
                }
                if (f.kind == List) {
                    listIndex = i;
                    r = item.box;
                    // Share the editor's right-hand frame pixel with the scrollbar.
                    // Both controls use the same outer top and bottom edges.
                    --r.top; // Match the edit CDEF's top frame pixel on OS 9.
                    r.left = r.right - 1;
                    r.right = r.left + 16;
                    scroll = Control(r, "", kControlScrollBarProc);
                    if (!scroll) return false;
                }
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
        focus = -1;
        for (int i = 0; i < kFieldCount; ++i) HideControl(items[i].control);
        HideControl(scroll);
        pane = next;
        SetControlValue(selector, pane + 1);
        for (int i = 0; i < kFieldCount; ++i)
            if (kFields[i].pane == pane) ShowControl(items[i].control);
        if (pane == 3) {
            ShowControl(scroll); SyncScroll();
            if (items[listIndex].overflow)
                notice = "List exceeds 2000 characters. Edit the preferences file first.";
        }
        InvalRect(&bounds);
    }

    void Focus(short i)
    {
        focus = i;
        SetKeyboardFocus(window, items[i].control, kControlEditTextPart);
    }

    void Line(short x, short y, const char *text)
    {
        Str255 p; Pascal(text, p); MoveTo(x, y); DrawString(p);
    }

    void CaptionFont()
    {
        Str255 name;
        short charcoal = 0;
        Pascal("Charcoal", name); GetFNum(name, &charcoal);
        TextFont(charcoal); TextSize(12); TextFace(0);
    }

    void Description(short left, short top, const char *text, short height = 26)
    {
        TextFont(3); TextSize(9); TextFace(0);
        Rect rect = { top, left, static_cast<short>(top + height), 440 };
        TETextBox(text, std::strlen(text), &rect, teJustLeft);
    }

    void Draw()
    {
        EraseRect(&bounds);
        // The Window Manager owns the native outer window frame. Only the
        // pane is framed here, with its selector replacing the title.
        DrawThemePrimaryGroup(&paneFrame, kThemeStateActive);
        Rect selectorGround = { 20, 17, 44, 205 };
        EraseRect(&selectorGround);
        DrawControls(window);
        for (int i = 0; i < kFieldCount; ++i) {
            const Field &f = kFields[i];
            if (f.pane != pane) continue;
            const Rect &box = items[i].box;
            if (f.kind != Check) {
                CaptionFont();
                Line(f.kind == List ? box.left : kRowLeft,
                     f.kind == List ? box.top - 10 : box.top + 12, f.label);
            }
            if (f.hint[0])
                Description(f.kind == Check ? kCheckTextLeft : box.left,
                            box.bottom + 4, f.hint);
        }
        TextFont(3); TextSize(9); TextFace(0);
        switch (pane) {
        case 0:
            Line(kRowLeft, 55, "Stop and start Gateway after changing listeners.");
            break;
        case 1:
            Line(kCheckTextLeft, 114, "For browsers without modern TLS support.");
            Line(kCheckTextLeft, 151, "Requires the Gateway CA in the browser. Choose one mode.");
            break;
        case 2: Line(kCheckTextLeft, 239, "Off requests the configured era directly."); break;
        case 3:
            Line(kCheckTextLeft, 167, "Accepted for compatibility; it has no effect.");
            Line(kRowLeft, 309, "Separate sites with ; or new lines. Plain names include subdomains.");
            Line(kRowLeft, 322, "Maximum 2000 characters. Remove a site to archive it again.");
            break;
        case 4:
            Description(244, 120, "Checked locally; never sent upstream.");
            break;
        case 5:
            Line(kRowLeft, 55, "Empty host fields use the selected provider's defaults.");
            Line(kCheckTextLeft, 243, "Port 465 uses TLS immediately; other ports default to STARTTLS.");
            break;
        case 6:
            Line(kRowLeft, 55, "Obtain the refresh token outside Gateway, then paste it here.");
            Line(kRowLeft, 68, "Long values scroll horizontally. Tokens may rotate while running.");
            break;
        case 7:
            Line(kCheckTextLeft, 82, "Off launches without a window, menu bar or Application");
            Line(kCheckTextLeft, 95, "menu entry. Send a Quit Apple event to stop Gateway.");
            Line(kCheckTextLeft, 144, "The window keeps the last 200 lines.");
            Line(kCheckTextLeft, 157, "The log file keeps everything.");
            {
                RGBColor previous, blue = { 0, 0, 0xCCCC };
                GetForeColor(&previous); RGBForeColor(&blue); TextFace(underline);
                Str255 label; Pascal("Open log folder in Finder", label);
                logLink.right = logLink.left + StringWidth(label);
                Line(logLink.left, 180, "Open log folder in Finder");
                TextFace(0); RGBForeColor(&previous);
            }
            break;
        }
        if (notice[0]) Line(13, 365, notice);
    }

    void OpenLogFolder()
    {
        // Open the same folder GWPlat_OpenLog uses, even before file logging
        // has been enabled. The no-reply event keeps the cooperative loop free.
        short volume;
        long directory, createdDirectory;
        Str255 name; Pascal("Gateway", name);
        FSSpec folder;
        OSErr err = FindFolder(kOnSystemDisk, kApplicationSupportFolderType,
                              kCreateFolder, &volume, &directory);
        if (err == noErr) {
            err = DirCreate(volume, directory, name, &createdDirectory);
            if (err == dupFNErr) err = noErr;
        }
        if (err == noErr) err = FSMakeFSSpec(volume, directory, name, &folder);
        AliasHandle alias = nullptr;
        if (err == noErr) err = NewAliasMinimal(&folder, &alias);
        AEAddressDesc target = { typeNull, nullptr };
        AppleEvent event = { typeNull, nullptr }, reply = { typeNull, nullptr };
        AEDescList objects = { typeNull, nullptr };
        OSType finder = 'MACS';
        if (err == noErr) err = AECreateDesc(typeApplSignature, &finder, sizeof(finder), &target);
        if (err == noErr) err = AECreateAppleEvent(kCoreEventClass, kAEOpenDocuments,
                &target, kAutoGenerateReturnID, kAnyTransactionID, &event);
        if (err == noErr) err = AECreateList(nullptr, 0, false, &objects);
        if (err == noErr) {
            HLock(reinterpret_cast<Handle>(alias));
            err = AEPutPtr(&objects, 0, typeAlias, *alias,
                          GetHandleSize(reinterpret_cast<Handle>(alias)));
            HUnlock(reinterpret_cast<Handle>(alias));
        }
        if (err == noErr) err = AEPutParamDesc(&event, keyDirectObject, &objects);
        if (err == noErr) err = AESend(&event, &reply, kAENoReply | kAECanSwitchLayer,
                                     kAENormalPriority, kAEDefaultTimeout, nullptr, nullptr);
        AEDisposeDesc(&objects); AEDisposeDesc(&event);
        AEDisposeDesc(&reply); AEDisposeDesc(&target);
        if (alias) DisposeHandle(reinterpret_cast<Handle>(alias));
        if (err != noErr) {
            notice = "Could not open the log folder in Finder.";
            SysBeep(1); InvalRect(&bounds);
        }
    }

    void SyncScroll()
    {
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
                notice = items[i].overflow ? "List exceeds 2000 characters. Edit the preferences file first." :
                         "Check the selected value before saving.";
                InvalRect(&bounds);
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
                notice = "Could not write preferences. Check the disk and try Save again.";
                SysBeep(1); InvalRect(&bounds);
                return false;
            }
            std::strcpy(items[i].original, value);
        }
        GWConfig_Load(); GW_LoadSettings();
        return true;
    }

    ~Preferences()
    {
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
        if (event.what == updateEvt && reinterpret_cast<WindowPtr>(event.message) == p->window) {
            BeginUpdate(p->window); p->Draw(); EndUpdate(p->window);
        } else if (event.what == mouseDown) {
            WindowPtr hitWindow;
            short part = FindWindow(event.where, &hitWindow);
            if (hitWindow != p->window) { SysBeep(1); continue; }
            if (part == inDrag) {
                Rect limit = qd.screenBits.bounds; InsetRect(&limit, 4, 4);
                DragWindow(p->window, event.where, &limit);
                continue;
            }
            if (part != inContent) continue;
            Point point = event.where; GlobalToLocal(&point);
            if (p->pane == 7 && PtInRect(point, &p->logLink)) {
                p->OpenLogFolder();
                continue;
            }
            bool edited = false;
            for (short i = 0; i < kFieldCount; ++i) {
                if (kFields[i].pane != p->pane || !Editable(kFields[i])) continue;
                if (!PtInRect(point, &p->items[i].box)) continue;
                p->Focus(i);
                // Track the click as well as changing focus: this places the caret.
                HandleControlClick(p->items[i].control, point, event.modifiers, nullptr);
                if (kFields[i].kind == List) p->SyncScroll();
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
                    TEScroll(0, old - GetControlValue(hit), p->items[p->listIndex].text);
                } else {
                    TrackControl(hit, point, scrollAction);
                }
                continue;
            }
            if (!HandleControlClick(hit, point, event.modifiers, nullptr)) continue;
            if (hit == p->selector) {
                short selected = GetControlValue(hit) - 1;
                if (selected >= 0 && selected < 8) p->SwitchPane(selected);
            } else if (hit == p->cancel) done = true;
            else if (hit == p->save) done = p->Save();
            else if (hit == p->revert) {
                ClearKeyboardFocus(p->window); p->focus = -1;
                LoadValues(p->items); p->SyncScroll();
                p->notice = "Preferences reloaded from disk.";
                InvalRect(&p->bounds);
            }
        } else if (event.what == keyDown || event.what == autoKey) {
            char ch = event.message & charCodeMask;
            bool command = (event.modifiers & cmdKey) != 0;
            if (ch == 27 || (command && ch == '.')) { done = true; continue; }
            if (ch == '\r' || ch == 3) { done = p->Save(); continue; }
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
                    if (!scrap) { SysBeep(1); continue; }
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
                    if (!valid) { SysBeep(1); continue; }
                }
            } else {
                bool numeric = field.kind == Number || field.kind == Date;
                unsigned char key = static_cast<unsigned char>(ch);
                bool navigation = key == 8 || (key >= 28 && key <= 31) || key == 127;
                if (!navigation && (remaining <= 0 ||
                    (numeric && (ch < '0' || ch > '9')) || key < 32)) {
                    SysBeep(1); continue;
                }
                HandleControlKey(item.control, (event.message & keyCodeMask) >> 8,
                                 key, event.modifiers);
            }
            TESelView(te);
            Draw1Control(item.control);
            if (field.kind == List) p->SyncScroll();
        } else {
            if (serviceEvent(context, &event)) done = true;
        }
    }
    DisposeControlActionUPP(scrollAction);
    delete p;
    SetPort(savedPort);
    UnregisterAppearanceClient();
}
