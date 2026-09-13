/*
 * gw_prefswin.cpp - the Preferences window on Mac OS 9.
 *
 * A modeless Dialog Manager dialog. Every field in it is a real dialog item,
 * so the system draws it and DialogSelect tracks it -- which is where text
 * selection, the caret, tabbing between fields and Cut/Copy/Paste come from.
 * An earlier version drew the fields itself with TextEdit and had none of
 * those, because they are not things a field has; they are things the Dialog
 * Manager does for a field it owns.
 *
 * The item list is built in memory from src/portable/gw_prefsform.c rather
 * than compiled as a DITL resource, so the fields stay declared in one place
 * and a preference added to the table appears in the window.
 *
 * Modeless is not a style choice. ModalDialog() runs its own event loop, so
 * GW_Poll() would stop being called and every proxy session would time out
 * while somebody read the labels. main.cpp offers this each event first.
 *
 * Values live in a shadow copy. One group is on screen at a time, so a save
 * that read the items would write the visible group and revert the other six.
 */

#include "gw_prefswin.h"

#include <Dialogs.h>
#include <Events.h>
#include <Fonts.h>
#include <Menus.h>
/*
 * Multiverse.h carries the managers Multiversal gives no header of their own:
 * the Control Manager, as main.cpp notes, and the Scrap Manager. There is no
 * Scrap.h to include.
 */
#include <Multiverse.h>
#include <Quickdraw.h>
#include <TextEdit.h>
#include <Windows.h>

#include <cstdio>
#include <cstring>

#include "../gw_config.h"
#include "../gw_core.h"
#include "../portable/gw_log.h"
#include "../portable/gw_prefsform.h"
#include "../portable/gw_util.h"

namespace {

const short kWinWidth     = 476;

const short kPopupTop     = 12;
const short kPopupHeight  = 20;

const short kBoxTop       = kPopupTop + kPopupHeight + 12;
const short kBoxLeft      = 10;
const short kBoxRight     = kWinWidth - 10;

const short kPaneLeft     = kBoxLeft + 12;
const short kPaneRight    = kBoxRight - 12;

const short kRowHeight    = 22;
const short kHintHeight   = 13;
const short kFieldWidth   = 176;
const short kEntryLeft    = kPaneRight - kFieldWidth;
const short kEntryHeight  = 16;
const short kListHeight   = 76;
const short kButtonWidth  = 74;
const short kButtonHeight = 20;

const unsigned short kPlatinum = 0xDDDD;

const short kFontGeneva   = 3;

const short kGroupMenuID    = 200;
const short kChoiceMenuBase = 201;

const short kMaxRows   = 24;
const short kMaxFields = 64;
const short kValueMax  = 256;
const short kListMax   = 3072;

/*
 * Dialog item types by value, the way main.cpp spells the window procs:
 * ctrlItem 4 plus btnCtrl 0 or chkCtrl 1, statText 8, editText 16.
 */
const short kBtnItem  = 4;
const short kChkItem  = 5;
const short kTextItem = 8;
const short kEditItem = 16;

/* Fixed items first, so Save is item 1 and can be the default button. */
const short kItemSave   = 1;
const short kItemRevert = 2;

void ToPascal(const char *src, Str255 dst)
{
    size_t n = std::strlen(src);

    if (n > 255) n = 255;
    dst[0] = static_cast<unsigned char>(n);
    std::memcpy(dst + 1, src, n);
}

void FromPascal(const unsigned char *src, char *dst, size_t cap)
{
    size_t n = src[0];

    if (cap == 0) return;
    if (n > cap - 1) n = cap - 1;
    std::memcpy(dst, src + 1, n);
    dst[n] = '\0';
}

/*
 * A secret on file that has not been retyped. A token is pasted rather than
 * edited, so the field shows this: leave it and the stored value is kept,
 * type anything and that is the new value.
 */
const char *const kKeptSecret = "\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5";

struct Row {
    const GWPrefField *field;
    int                index;    /* into the shadow table                 */
    short              item;     /* its dialog item, or 0                 */
    Rect               popup;    /* choice fields: where the box is       */
    short              menuID;
    short              choice;   /* 1-based item in that menu             */
    int                altCount;
};

/*
 * A dialog item list, built in memory.
 *
 * The format is a count followed by items, each a placeholder long, a rect, a
 * type byte, a length byte and that many bytes of text, padded so the next
 * one starts even. The Dialog Manager reads exactly this whether it came from
 * a resource or from here, and building it here is what keeps the field table
 * the only list of fields.
 */
class ItemList {
public:
    ItemList() : mH(0), mLen(0), mCount(0)
    {
        mH = NewHandle(2);
        if (mH != 0) {
            **reinterpret_cast<short **>(mH) = -1;
            mLen = 2;
        }
    }

    Handle Release() { Handle h = mH; mH = 0; return h; }

    short Add(short type, const Rect *r, const char *text)
    {
        unsigned char len = 0;
        long          need;
        Ptr           p;

        if (mH == 0) return 0;
        if (text != 0) {
            size_t n = std::strlen(text);

            len = static_cast<unsigned char>(n > 255 ? 255 : n);
        }
        need = 4 + 8 + 1 + 1 + len;
        if ((need & 1) != 0) need++;

        SetHandleSize(mH, mLen + need);
        if (MemError() != noErr) return 0;

        p = *mH + mLen;
        std::memset(p, 0, static_cast<size_t>(need));
        p += 4;
        reinterpret_cast<short *>(p)[0] = r->top;
        reinterpret_cast<short *>(p)[1] = r->left;
        reinterpret_cast<short *>(p)[2] = r->bottom;
        reinterpret_cast<short *>(p)[3] = r->right;
        p += 8;
        *p++ = static_cast<char>(type);
        *p++ = static_cast<char>(len);
        if (len > 0) std::memcpy(p, text, len);

        mLen += need;
        mCount++;
        **reinterpret_cast<short **>(mH) = static_cast<short>(mCount - 1);
        return mCount;
    }

private:
    Handle mH;
    long   mLen;
    short  mCount;
};

class PrefsWindow {
public:
    PrefsWindow()
        : mDialog(0), mGroup(0), mGroupItem(1), mRowCount(0), mDirty(false),
          mFirstEdit(0)
    {
        std::memset(mValue, 0, sizeof(mValue));
        std::memset(mList, 0, sizeof(mList));
        std::memset(mRows, 0, sizeof(mRows));
        std::memset(mNote, 0, sizeof(mNote));
        SetRect(&mGroupPopup, 0, 0, 0, 0);
        mWhere.h = -1;
        mWhere.v = -1;
    }

    bool Open();
    void Close();
    bool IsOpen() const { return mDialog != 0; }
    WindowPtr Window() const { return reinterpret_cast<WindowPtr>(mDialog); }

    bool HandleEvent(EventRecord &event);
    void Idle() {}      /* DialogSelect blinks the caret from null events */
    void EditCommand(int cmd);
    bool CanEdit() const { return mDialog != 0 && mFirstEdit != 0; }

private:
    void  LoadValues();
    void  BuildGroup(int group);
    void  ReadGroupBack();
    void  Draw();
    void  Save();
    void  Revert();
    short TrackPopup(const Rect *box, short menuID, short current);
    void  DrawPopupBox(const Rect *box, short menuID, short item);
    void  MakeChoiceMenus(int group);
    void  DropChoiceMenus();
    void  GetItemText(short item, char *out, size_t cap);

    DialogPtr mDialog;
    int       mGroup;
    short     mGroupItem;
    Rect      mGroupPopup;
    Point     mWhere;        /* keeps its place across a group change */
    Row       mRows[kMaxRows];
    int       mRowCount;
    bool      mDirty;
    short     mFirstEdit;

    char      mValue[kMaxFields][kValueMax];
    char      mList[kListMax];
    char      mNote[128];
};

PrefsWindow *gPrefs = 0;

/* ------------------------------------------------------------------ */

void PrefsWindow::GetItemText(short item, char *out, size_t cap)
{
    short  type;
    Handle h;
    Rect   r;
    Str255 s;

    if (cap > 0) out[0] = '\0';
    if (mDialog == 0 || item < 1) return;
    GetDialogItem(mDialog, item, &type, &h, &r);
    if (h == 0) return;
    GetDialogItemText(h, s);
    FromPascal(s, out, cap);
}

void PrefsWindow::LoadValues()
{
    int count = 0;
    const GWPrefField *f = gw_prefsform_fields(&count);
    int i;

    if (count > kMaxFields) count = kMaxFields;

    for (i = 0; i < count; i++) {
        if (f[i].kind == kGWFieldList) {
            char   pattern[256];
            int    k;
            size_t used = 0;

            mList[0] = '\0';
            for (k = 0; k < 128; k++) {
                size_t n;

                if (!GWConfig_GetNth(f[i].key, k, pattern, sizeof(pattern)))
                    break;
                if (pattern[0] == '\0') continue;
                n = std::strlen(pattern);
                if (used + n + 2 >= sizeof(mList)) break;
                std::memcpy(mList + used, pattern, n);
                used += n;
                mList[used++] = '\r';
            }
            mList[used] = '\0';
            continue;
        }
        if (f[i].kind == kGWFieldSecret) {
            const char *v = GWConfig_Str(f[i].key, "");

            std::strcpy(mValue[i], (v != 0 && v[0] != '\0') ? kKeptSecret : "");
            continue;
        }
        {
            const char *v = GWConfig_Str(f[i].key, f[i].def);

            if (v == 0) v = "";
            gw_copy_n(mValue[i], kValueMax, v, std::strlen(v));
        }
    }
    mDirty = false;
}

void PrefsWindow::ReadGroupBack()
{
    int i;

    if (mDialog == 0) return;

    for (i = 0; i < mRowCount; i++) {
        Row *r = &mRows[i];

        if (r->field == 0) continue;

        switch (r->field->kind) {
        case kGWFieldFlag: {
            short  type;
            Handle h;
            Rect   box;

            GetDialogItem(mDialog, r->item, &type, &h, &box);
            if (h != 0)
                std::strcpy(mValue[r->index],
                            GetControlValue(reinterpret_cast<ControlHandle>(h))
                                ? "1" : "0");
            break;
        }

        case kGWFieldChoice: {
            MenuHandle menu = GetMenuHandle(r->menuID);

            if (menu != 0 && r->choice >= 1 && r->choice <= r->altCount) {
                Str255 s;

                GetMenuItemText(menu, r->choice, s);
                FromPascal(s, mValue[r->index], kValueMax);
            }
            break;
        }

        case kGWFieldList:
            GetItemText(r->item, mList, sizeof(mList));
            break;

        default:
            GetItemText(r->item, mValue[r->index], kValueMax);
            break;
        }
    }
}

/* ------------------------------------------------------------------ */

void PrefsWindow::DropChoiceMenus()
{
    int i;

    for (i = 0; i < mRowCount; i++) {
        if (mRows[i].menuID == 0) continue;
        DeleteMenu(mRows[i].menuID);
        DisposeMenu(GetMenuHandle(mRows[i].menuID));
        mRows[i].menuID = 0;
    }
}

void PrefsWindow::MakeChoiceMenus(int group)
{
    int                count = 0;
    const GWPrefField *f = gw_prefsform_fields(&count);
    int                i, row = 0;

    if (count > kMaxFields) count = kMaxFields;
    for (i = 0; i < count && row < mRowCount; i++) {
        Row        *r;
        const char *p;
        MenuHandle  menu;
        Str255      title;
        short       id;

        if (f[i].group != group) continue;
        r = &mRows[row++];
        if (f[i].kind != kGWFieldChoice) continue;

        id = static_cast<short>(kChoiceMenuBase + row);
        ToPascal(f[i].label, title);
        menu = NewMenu(id, title);
        if (menu == 0) continue;

        r->choice = 1;
        r->altCount = 0;
        p = f[i].choices;
        while (p != 0 && *p != '\0') {
            const char *end = std::strchr(p, '|');
            size_t      n = (end != 0) ? static_cast<size_t>(end - p)
                                       : std::strlen(p);
            char        buf[32];

            if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
            std::memcpy(buf, p, n);
            buf[n] = '\0';
            r->altCount++;
            if (gw_stricmp(buf, mValue[i]) == 0)
                r->choice = static_cast<short>(r->altCount);
            ToPascal(buf, title);
            AppendMenu(menu, title);
            p = (end != 0) ? end + 1 : 0;
        }
        InsertMenu(menu, -1);
        r->menuID = id;
    }
}

/*
 * Build the dialog for one group.
 *
 * Made anew rather than having its items rearranged: an item list cannot be
 * resized in place, and a window whose contents change completely is not a
 * window that gained a few items. Its position is kept so it does not walk
 * across the screen when the popup is used.
 */
void PrefsWindow::BuildGroup(int group)
{
    int                count = 0;
    const GWPrefField *f = gw_prefsform_fields(&count);
    int                i;
    short              v, height;
    ItemList           items;
    Rect               r, bounds;
    Str255             title;

    if (mDialog != 0) {
        GrafPtr port = reinterpret_cast<GrafPtr>(mDialog);
        Point   tl;

        SetPort(port);
        tl.h = port->portRect.left;
        tl.v = port->portRect.top;
        LocalToGlobal(&tl);
        mWhere = tl;
        DropChoiceMenus();
        DisposeDialog(mDialog);
        mDialog = 0;
    }

    std::memset(mRows, 0, sizeof(mRows));
    mRowCount = 0;
    mFirstEdit = 0;
    mGroup = group;
    if (count > kMaxFields) count = kMaxFields;

    SetRect(&r, 0, 0, kButtonWidth, kButtonHeight);
    items.Add(kBtnItem, &r, "Save");
    items.Add(kBtnItem, &r, "Revert");

    v = static_cast<short>(kBoxTop + 12);
    for (i = 0; i < count && mRowCount < kMaxRows; i++) {
        Row *row;

        if (f[i].group != group) continue;
        row = &mRows[mRowCount];
        row->field = &f[i];
        row->index = i;

        switch (f[i].kind) {
        case kGWFieldFlag:
            SetRect(&r, kPaneLeft, v, kPaneRight,
                    static_cast<short>(v + 16));
            row->item = items.Add(kChkItem, &r, f[i].label);
            break;

        case kGWFieldChoice:
            /* The label is static text; the popup is drawn and tracked here,
             * an item list having no way to carry one without a CNTL. */
            SetRect(&r, kPaneLeft, static_cast<short>(v + 3),
                    static_cast<short>(kEntryLeft - 8),
                    static_cast<short>(v + 16));
            items.Add(kTextItem, &r, f[i].label);
            SetRect(&row->popup, kEntryLeft, v, kPaneRight,
                    static_cast<short>(v + 19));
            break;

        case kGWFieldList:
            SetRect(&r, kPaneLeft, v, kPaneRight,
                    static_cast<short>(v + 13));
            items.Add(kTextItem, &r, f[i].label);
            SetRect(&r, kPaneLeft, static_cast<short>(v + 15), kPaneRight,
                    static_cast<short>(v + 15 + kListHeight));
            row->item = items.Add(kEditItem, &r, mList);
            if (mFirstEdit == 0) mFirstEdit = row->item;
            break;

        default:
            SetRect(&r, kPaneLeft, static_cast<short>(v + 3),
                    static_cast<short>(kEntryLeft - 8),
                    static_cast<short>(v + 16));
            items.Add(kTextItem, &r, f[i].label);
            SetRect(&r, kEntryLeft, v, kPaneRight,
                    static_cast<short>(v + kEntryHeight));
            row->item = items.Add(kEditItem, &r, mValue[i]);
            if (mFirstEdit == 0) mFirstEdit = row->item;
            break;
        }

        if (f[i].kind == kGWFieldList)
            v = static_cast<short>(v + 15 + kListHeight + 3);
        else
            v = static_cast<short>(v + kRowHeight);

        if (f[i].hint != 0) {
            SetRect(&r, kPaneLeft, static_cast<short>(v - 5), kPaneRight,
                    static_cast<short>(v + 8));
            items.Add(kTextItem, &r, f[i].hint);
            v = static_cast<short>(v + kHintHeight);
        }
        v = static_cast<short>(v + 2);
        mRowCount++;
    }

    height = static_cast<short>(v + 10 + 12 + kButtonHeight + 12);

    if (mWhere.h < 0) {
        mWhere.h = static_cast<short>((qd.screenBits.bounds.right -
                                       qd.screenBits.bounds.left -
                                       kWinWidth) / 2);
        mWhere.v = static_cast<short>((qd.screenBits.bounds.bottom -
                                       qd.screenBits.bounds.top -
                                       height) / 3);
        if (mWhere.v < 40) mWhere.v = 40;
    }
    SetRect(&bounds, mWhere.h, mWhere.v,
            static_cast<short>(mWhere.h + kWinWidth),
            static_cast<short>(mWhere.v + height));

    ToPascal("Gateway Preferences", title);
    mDialog = NewDialog(0, &bounds, title, true, noGrowDocProc,
                        reinterpret_cast<WindowPtr>(-1), true, 0,
                        items.Release());
    if (mDialog == 0) return;

    SetPort(reinterpret_cast<GrafPtr>(mDialog));
    TextFont(kFontGeneva);
    TextSize(9);

    /* The buttons, now that the height is known. */
    {
        short  type;
        Handle h;
        Rect   box;
        short  top = static_cast<short>(height - 12 - kButtonHeight);

        GetDialogItem(mDialog, kItemSave, &type, &h, &box);
        SetRect(&box, static_cast<short>(kBoxRight - kButtonWidth - 4), top,
                static_cast<short>(kBoxRight - 4),
                static_cast<short>(top + kButtonHeight));
        if (h != 0)
            MoveControl(reinterpret_cast<ControlHandle>(h), box.left, box.top);
        SetDialogItem(mDialog, kItemSave, type, h, &box);

        GetDialogItem(mDialog, kItemRevert, &type, &h, &box);
        SetRect(&box, static_cast<short>(kBoxRight - 2 * kButtonWidth - 14),
                top, static_cast<short>(kBoxRight - kButtonWidth - 14),
                static_cast<short>(top + kButtonHeight));
        if (h != 0)
            MoveControl(reinterpret_cast<ControlHandle>(h), box.left, box.top);
        SetDialogItem(mDialog, kItemRevert, type, h, &box);
    }

    /* The system draws the ring around item 1, so nothing here has to. */
    SetDialogDefaultItem(mDialog, kItemSave);

    MakeChoiceMenus(group);

    for (i = 0; i < mRowCount; i++) {
        short  type;
        Handle h;
        Rect   box;

        if (mRows[i].field == 0) continue;
        if (mRows[i].field->kind != kGWFieldFlag) continue;
        GetDialogItem(mDialog, mRows[i].item, &type, &h, &box);
        if (h != 0)
            SetControlValue(reinterpret_cast<ControlHandle>(h),
                            mValue[mRows[i].index][0] == '1' ? 1 : 0);
    }

    SetRect(&mGroupPopup, 82, kPopupTop, 272,
            static_cast<short>(kPopupTop + kPopupHeight));
    mGroupItem = static_cast<short>(group + 1);

    /* Something selected to begin with, so the caret is where a person
     * expects it and the Edit menu has an item to act on. */
    if (mFirstEdit != 0) SelectDialogItemText(mDialog, mFirstEdit, 0, 0);
}

/* ------------------------------------------------------------------ */

short PrefsWindow::TrackPopup(const Rect *box, short menuID, short current)
{
    MenuHandle menu = GetMenuHandle(menuID);
    Point      tl;
    long       choice;

    if (menu == 0) return 0;
    tl.h = static_cast<short>(box->left + 1);
    tl.v = static_cast<short>(box->top + 1);
    LocalToGlobal(&tl);
    choice = PopUpMenuSelect(menu, tl.v, tl.h, current);
    if (choice == 0) return 0;
    return static_cast<short>(choice & 0xFFFF);
}

/*
 * The popup box, to the Platinum shape.
 *
 * These two -- the group popup and the choice popups -- are the only things
 * in the window this file draws, because an item list cannot carry a popup
 * without a CNTL resource and a CNTL per choice field would be a second list
 * of choices to keep in step with the table.
 */
void PrefsWindow::DrawPopupBox(const Rect *box, short menuID, short item)
{
    MenuHandle menu = GetMenuHandle(menuID);
    Rect       r = *box;
    Str255     s;
    RGBColor   black, white, platinum, shadow;
    short      x, y, k;

    black.red = black.green = black.blue = 0;
    white.red = white.green = white.blue = 0xFFFF;
    platinum.red = platinum.green = platinum.blue = kPlatinum;
    shadow.red = shadow.green = shadow.blue = 0x6666;

    RGBBackColor(&platinum);
    EraseRect(&r);

    RGBForeColor(&shadow);
    MoveTo(static_cast<short>(r.left + 2), static_cast<short>(r.bottom - 1));
    LineTo(static_cast<short>(r.right - 1), static_cast<short>(r.bottom - 1));
    MoveTo(static_cast<short>(r.right - 1), static_cast<short>(r.top + 2));
    LineTo(static_cast<short>(r.right - 1), static_cast<short>(r.bottom - 1));

    r.right = static_cast<short>(r.right - 2);
    r.bottom = static_cast<short>(r.bottom - 2);

    RGBForeColor(&white);
    MoveTo(static_cast<short>(r.left + 1), static_cast<short>(r.bottom - 1));
    LineTo(static_cast<short>(r.left + 1), static_cast<short>(r.top + 1));
    LineTo(static_cast<short>(r.right - 1), static_cast<short>(r.top + 1));

    RGBForeColor(&black);
    FrameRect(&r);

    if (menu != 0 && item >= 1) {
        GetMenuItemText(menu, item, s);
        MoveTo(static_cast<short>(r.left + 9), static_cast<short>(r.top + 13));
        DrawString(s);
    }

    x = static_cast<short>(r.right - 18);
    y = static_cast<short>(r.top + 7);
    for (k = 0; k < 5; k++) {
        MoveTo(static_cast<short>(x + k), static_cast<short>(y + k));
        LineTo(static_cast<short>(x + 8 - k), static_cast<short>(y + k));
    }
    RGBBackColor(&platinum);
}

void PrefsWindow::Draw()
{
    GrafPtr  port;
    Rect     box;
    Str255   s;
    RGBColor platinum, black;
    short    bottom, w;
    int      i;

    if (mDialog == 0) return;
    port = reinterpret_cast<GrafPtr>(mDialog);
    SetPort(port);

    platinum.red = platinum.green = platinum.blue = kPlatinum;
    black.red = black.green = black.blue = 0;
    RGBBackColor(&platinum);
    RGBForeColor(&platinum);
    box = port->portRect;
    PaintRect(&box);
    RGBForeColor(&black);

    TextFont(kFontGeneva);
    TextSize(9);
    TextFace(normal);

    MoveTo(kBoxLeft, static_cast<short>(kPopupTop + 14));
    ToPascal("Settings for:", s);
    DrawString(s);

    /* The group box, with its name let into the top rule as TCP/IP does. */
    bottom = static_cast<short>(port->portRect.bottom - 12 - kButtonHeight -
                                12);
    SetRect(&box, kBoxLeft, kBoxTop, kBoxRight, bottom);
    FrameRect(&box);
    ToPascal(gw_prefsform_group_name(mGroup), s);
    w = StringWidth(s);
    SetRect(&box, static_cast<short>(kBoxLeft + 8), kBoxTop,
            static_cast<short>(kBoxLeft + 14 + w),
            static_cast<short>(kBoxTop + 1));
    RGBForeColor(&platinum);
    PaintRect(&box);
    RGBForeColor(&black);
    MoveTo(static_cast<short>(kBoxLeft + 11), static_cast<short>(kBoxTop + 4));
    DrawString(s);

    DrawPopupBox(&mGroupPopup, kGroupMenuID, mGroupItem);
    for (i = 0; i < mRowCount; i++)
        if (mRows[i].menuID != 0)
            DrawPopupBox(&mRows[i].popup, mRows[i].menuID, mRows[i].choice);

    if (mNote[0] != '\0') {
        MoveTo(kBoxLeft,
               static_cast<short>(port->portRect.bottom - 12 -
                                  kButtonHeight + 14));
        ToPascal(mNote, s);
        DrawString(s);
    }

    DrawDialog(mDialog);
}

/* ------------------------------------------------------------------ */

bool PrefsWindow::Open()
{
    if (mDialog != 0) {
        SelectWindow(reinterpret_cast<WindowPtr>(mDialog));
        return true;
    }

    LoadValues();
    mNote[0] = '\0';

    /* The group menu outlives a group change; the choice menus do not. */
    if (GetMenuHandle(kGroupMenuID) == 0) {
        MenuHandle menu;
        Str255     title;
        int        g;

        ToPascal("Settings", title);
        menu = NewMenu(kGroupMenuID, title);
        if (menu != 0) {
            for (g = 0; g < kGWGroupCount; g++) {
                ToPascal(gw_prefsform_group_name(g), title);
                AppendMenu(menu, title);
            }
            InsertMenu(menu, -1);
        }
    }

    BuildGroup(0);
    return mDialog != 0;
}

void PrefsWindow::Close()
{
    if (mDialog == 0) return;
    DropChoiceMenus();
    DisposeDialog(mDialog);
    mDialog = 0;
    DeleteMenu(kGroupMenuID);
    DisposeMenu(GetMenuHandle(kGroupMenuID));
    mRowCount = 0;
    mFirstEdit = 0;
}

/* ------------------------------------------------------------------ */

bool PrefsWindow::HandleEvent(EventRecord &event)
{
    WindowPtr mine;
    DialogPtr which;
    short     hit;

    if (mDialog == 0) return false;
    mine = reinterpret_cast<WindowPtr>(mDialog);

    if (event.what == mouseDown) {
        WindowPtr win;
        short     part = FindWindow(event.where, &win);

        if (win != mine) return false;

        if (part == inGoAway) {
            if (TrackGoAway(mine, event.where)) Close();
            return true;
        }
        if (part == inDrag) {
            Rect limit = qd.screenBits.bounds;

            InsetRect(&limit, 4, 4);
            DragWindow(mine, event.where, &limit);
            return true;
        }
        if (part == inContent) {
            Point where = event.where;
            int   i;

            if (FrontWindow() != mine) {
                SelectWindow(mine);
                return true;
            }

            /* The popups are ours; every other click is the dialog's. */
            SetPort(reinterpret_cast<GrafPtr>(mDialog));
            GlobalToLocal(&where);

            if (PtInRect(where, &mGroupPopup)) {
                short item = TrackPopup(&mGroupPopup, kGroupMenuID,
                                        mGroupItem);

                if (item >= 1 && item <= kGWGroupCount &&
                    item - 1 != mGroup) {
                    ReadGroupBack();
                    BuildGroup(static_cast<short>(item - 1));
                    Draw();
                }
                return true;
            }
            for (i = 0; i < mRowCount; i++) {
                short item;

                if (mRows[i].menuID == 0) continue;
                if (!PtInRect(where, &mRows[i].popup)) continue;
                item = TrackPopup(&mRows[i].popup, mRows[i].menuID,
                                  mRows[i].choice);
                if (item >= 1 && item <= mRows[i].altCount) {
                    mRows[i].choice = item;
                    mDirty = true;
                    InvalRect(&mRows[i].popup);
                }
                return true;
            }
        }
    }

    if (event.what == updateEvt &&
        reinterpret_cast<WindowPtr>(event.message) == mine) {
        BeginUpdate(mine);
        Draw();
        EndUpdate(mine);
        return true;
    }

    /*
     * Everything else belongs to the dialog. DialogSelect is what gives the
     * entry fields their selection, their caret and tabbing between them,
     * which is the whole reason they are dialog items.
     */
    if (IsDialogEvent(&event)) {
        if (DialogSelect(&event, &which, &hit) && which == mDialog) {
            if (hit == kItemSave) Save();
            else if (hit == kItemRevert) Revert();
            else mDirty = true;
        }
        return true;
    }
    return false;
}

void PrefsWindow::EditCommand(int cmd)
{
    if (mDialog == 0) return;
    switch (cmd) {
    case kGWEditCut:   DialogCut(mDialog);    mDirty = true; break;
    case kGWEditCopy:  DialogCopy(mDialog);                  break;
    case kGWEditPaste: DialogPaste(mDialog);  mDirty = true; break;
    case kGWEditClear: DialogDelete(mDialog); mDirty = true; break;
    case kGWEditSelectAll:
        if (mFirstEdit != 0)
            SelectDialogItemText(mDialog, mFirstEdit, 0, 32767);
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */

void PrefsWindow::Revert()
{
    LoadValues();
    BuildGroup(mGroup);
    std::strcpy(mNote, "Reverted to what is in the file.");
    Draw();
}

/*
 * Write what changed, and only what changed, so every line nobody touched
 * keeps its comment, its spacing and the key spelling whoever typed it used.
 */
void PrefsWindow::Save()
{
    int                count = 0;
    const GWPrefField *f = gw_prefsform_fields(&count);
    int                i, written = 0, failed = 0, restart = 0;

    ReadGroupBack();
    if (count > kMaxFields) count = kMaxFields;

    for (i = 0; i < count; i++) {
        char        clean[kValueMax];
        const char *why = 0;
        const char *current;

        if (f[i].kind == kGWFieldList) {
            const char *vals[128];
            static char store[kListMax];
            int         n = 0;
            char       *p;

            std::memcpy(store, mList, sizeof(store));
            store[kListMax - 1] = '\0';
            p = store;
            while (*p != '\0' && n < 128) {
                char *end = p;

                while (*end != '\0' && *end != '\r' && *end != '\n') end++;
                if (*end != '\0') *end++ = '\0';
                while (*p == ' ' || *p == '\t') p++;
                if (*p != '\0') vals[n++] = p;
                p = end;
            }
            if (GWConfig_SetList(f[i].key, vals, n)) written++;
            else failed++;
            continue;
        }

        if (f[i].kind == kGWFieldSecret &&
            std::strcmp(mValue[i], kKeptSecret) == 0)
            continue;

        if (!gw_prefsform_validate(&f[i], mValue[i], clean, sizeof(clean),
                                   &why)) {
            gw_log("preferences: %s", why != 0 ? why : "a value was refused");
            failed++;
            continue;
        }

        current = GWConfig_Str(f[i].key,
                               f[i].kind == kGWFieldSecret ? "" : f[i].def);
        if (current == 0) current = "";
        if (!gw_prefsform_changed(current, clean)) continue;

        if (GWConfig_Set(f[i].key, clean)) {
            written++;
            if (f[i].needs_restart) restart++;
        } else {
            failed++;
        }
    }

    if (failed > 0)
        std::sprintf(mNote, "%d saved, %d refused -- see the log.",
                     written, failed);
    else if (restart > 0)
        std::sprintf(mNote, "Saved. %d take effect on restart.", restart);
    else if (written > 0)
        std::strcpy(mNote, "Saved.");
    else
        std::strcpy(mNote, "Nothing had changed.");
    gw_log("preferences: %s", mNote);

    mDirty = false;
    LoadValues();
    BuildGroup(mGroup);
    Draw();
}

}  /* namespace */

/* ------------------------------------------------------------------ */

void GWPrefsWin_Open(void)
{
    if (gPrefs == 0) gPrefs = new PrefsWindow();
    if (gPrefs != 0) gPrefs->Open();
}

void GWPrefsWin_Close(void)
{
    if (gPrefs != 0) gPrefs->Close();
}

int GWPrefsWin_IsOpen(void)
{
    return (gPrefs != 0 && gPrefs->IsOpen()) ? 1 : 0;
}

WindowPtr GWPrefsWin_Window(void)
{
    return (gPrefs != 0) ? gPrefs->Window() : 0;
}

int GWPrefsWin_HandleEvent(EventRecord *event)
{
    if (gPrefs == 0 || event == 0) return 0;
    return gPrefs->HandleEvent(*event) ? 1 : 0;
}

void GWPrefsWin_Idle(void)
{
    if (gPrefs != 0) gPrefs->Idle();
}

int GWPrefsWin_CanEdit(void)
{
    return (gPrefs != 0 && gPrefs->IsOpen() && gPrefs->CanEdit()) ? 1 : 0;
}

void GWPrefsWin_EditCommand(int cmd)
{
    if (gPrefs != 0) gPrefs->EditCommand(cmd);
}
