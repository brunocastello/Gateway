/*
 * gw_prefswin.cpp - the Preferences window on Mac OS 9.
 *
 * A modeless Dialog Manager dialog laid out after the Internet control panel.
 * Nothing in it is drawn by hand: the entry fields and the host list are
 * editText items, the checkboxes are chkCtrl items, the buttons are btnCtrl
 * with the system's default ring, the popups are popup controls and the list
 * has a scroll bar control. DialogSelect does the tracking, which is where
 * selection, the caret, tabbing and Cut/Copy/Paste come from.
 *
 * The item list is built in memory from src/portable/gw_prefsform.c, so the
 * fields stay declared in one place and a preference added to the table shows
 * up in the window.
 *
 * Modeless is not a style choice. ModalDialog() runs its own event loop, so
 * GW_Poll() would stop being called and every proxy session would time out
 * while somebody read the labels.
 *
 * Values live in a shadow copy. One group is on screen at a time, so a save
 * that read the items would write the visible group and revert the rest.
 */

#include "gw_prefswin.h"

#include <Dialogs.h>
#include <Events.h>
#include <Fonts.h>
#include <Menus.h>
/*
 * Multiverse.h carries the managers Multiversal gives no header of their own:
 * the Control Manager, as main.cpp notes, and the Scrap Manager.
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

const short kWinWidth     = 486;

const short kPopupTop     = 12;
const short kPopupHeight  = 20;

const short kBoxTop       = kPopupTop + kPopupHeight + 12;
const short kBoxLeft      = 10;
const short kBoxRight      = kWinWidth - 10;

const short kPaneLeft     = kBoxLeft + 12;
const short kPaneRight    = kBoxRight - 12;

const short kRowHeight    = 23;
const short kHintHeight   = 13;
const short kFieldWidth   = 188;
const short kEntryLeft    = kPaneRight - kFieldWidth;
const short kEntryHeight  = 18;
const short kListHeight   = 76;
const short kButtonWidth  = 74;
const short kButtonHeight = 20;
const short kScrollWidth  = 16;

const unsigned short kPlatinum = 0xDDDD;

const short kFontGeneva   = 3;

/*
 * The control definitions, by value the way main.cpp spells the window procs:
 * the popup is CDEF 63 and the scroll bar CDEF 16.
 */
const short kPopupMenuProc = 1008;
const short kScrollBarProc = 16;

const short kGroupMenuID    = 200;
const short kChoiceMenuBase = 201;

const short kMaxRows   = 24;
const short kMaxFields = 64;
const short kValueMax  = 256;
const short kListMax   = 3072;

/*
 * Dialog item types by value: ctrlItem 4 plus btnCtrl 0 or chkCtrl 1,
 * statText 8, editText 16.
 */
const short kBtnItem  = 4;
const short kChkItem  = 5;
const short kTextItem = 8;
const short kEditItem = 16;

const short kItemSave   = 1;
const short kItemRevert = 2;

/* Scroll bar part codes, spelled out as main.cpp spells them. */
const short kInUpButton   = 20;
const short kInDownButton = 21;
const short kInPageUp     = 22;
const short kInPageDown   = 23;
const short kInThumb      = 129;

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
    int                index;     /* into the shadow table                */
    short              item;      /* the editText or chkCtrl item, or 0   */
    short              labelItem; /* its statText, for right-aligning     */
    ControlHandle      popup;     /* choice fields                        */
    Rect               popupRect;
    ControlHandle      scroll;    /* list fields                          */
    Rect               scrollRect;
    short              menuID;
    int                altCount;
};

/*
 * A dialog item list, built in memory: a count, then items, each a
 * placeholder long, a rect, a type byte, a length byte and that many bytes of
 * text, padded so the next starts even. The Dialog Manager reads exactly this
 * whether it came from a resource or from here, and building it here keeps
 * the field table the only list of fields.
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
        : mDialog(0), mGroupCtl(0), mGroup(0), mRowCount(0), mDirty(false),
          mFirstEdit(0)
    {
        std::memset(mValue, 0, sizeof(mValue));
        std::memset(mList, 0, sizeof(mList));
        std::memset(mRows, 0, sizeof(mRows));
        std::memset(mNote, 0, sizeof(mNote));
        mWhere.h = -1;
        mWhere.v = -1;
    }

    bool Open();
    void Close();
    bool IsOpen() const { return mDialog != 0; }
    WindowPtr Window() const { return reinterpret_cast<WindowPtr>(mDialog); }

    bool HandleEvent(EventRecord &event);
    void Idle() {}
    void EditCommand(int cmd);
    bool CanEdit() const { return mDialog != 0 && mFirstEdit != 0; }

private:
    void  LoadValues();
    void  BuildGroup(int group);
    void  ReadGroupBack();
    void  Draw();
    void  Save();
    void  Revert();
    void  MakeControls(int group);
    void  DropControls();
    void  AlignLabels();
    short PopChoice(ControlHandle ctl, short menuID);
    void  SyncScroll(Row *r);
    void  ScrollList(Row *r, short part);
    void  GetItemText(short item, char *out, size_t cap);
    TEHandle DialogTE() const;

    DialogPtr     mDialog;
    ControlHandle mGroupCtl;
    int           mGroup;
    Point         mWhere;
    Row           mRows[kMaxRows];
    int           mRowCount;
    bool          mDirty;
    short         mFirstEdit;

    char          mValue[kMaxFields][kValueMax];
    char          mList[kListMax];
    char          mNote[128];
};

PrefsWindow *gPrefs = 0;

/* ------------------------------------------------------------------ */

TEHandle PrefsWindow::DialogTE() const
{
    if (mDialog == 0) return 0;
    return reinterpret_cast<DialogPeek>(mDialog)->textH;
}

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
            short      item = (r->popup != 0) ? GetControlValue(r->popup) : 0;

            if (menu != 0 && item >= 1 && item <= r->altCount) {
                Str255 s;

                GetMenuItemText(menu, item, s);
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

void PrefsWindow::DropControls()
{
    int i;

    if (mGroupCtl != 0) { DisposeControl(mGroupCtl); mGroupCtl = 0; }
    for (i = 0; i < mRowCount; i++) {
        if (mRows[i].popup != 0)  { DisposeControl(mRows[i].popup);  }
        if (mRows[i].scroll != 0) { DisposeControl(mRows[i].scroll); }
        mRows[i].popup = 0;
        mRows[i].scroll = 0;
        if (mRows[i].menuID != 0) {
            DeleteMenu(mRows[i].menuID);
            DisposeMenu(GetMenuHandle(mRows[i].menuID));
            mRows[i].menuID = 0;
        }
    }
}

/*
 * The popups and the list's scroll bar, made after the dialog exists because
 * a control needs a window. They are real controls: the popup is CDEF 63, the
 * same definition the Internet control panel's "Active Set" uses, and the
 * scroll bar is CDEF 16.
 */
void PrefsWindow::MakeControls(int group)
{
    int                count = 0;
    const GWPrefField *f = gw_prefsform_fields(&count);
    int                i, row = 0;
    Rect               box;
    Str255             title;

    if (mDialog == 0) return;
    if (count > kMaxFields) count = kMaxFields;

    /* The pane popup, at the top, as "Active Set" is. */
    SetRect(&box, 88, kPopupTop, 300,
            static_cast<short>(kPopupTop + kPopupHeight));
    ToPascal("", title);
    mGroupCtl = NewControl(reinterpret_cast<WindowPtr>(mDialog), &box, title,
                           true, static_cast<short>(group + 1),
                           kGroupMenuID, 0, kPopupMenuProc, 0);
    /* Set again after the fact: the value handed to NewControl does not stick
     * on this definition, which is why the popup kept saying "Modules" while
     * the pane below it was Wayback. */
    if (mGroupCtl != 0)
        SetControlValue(mGroupCtl, static_cast<short>(group + 1));

    for (i = 0; i < count && row < mRowCount; i++) {
        Row *r;

        if (f[i].group != group) continue;
        r = &mRows[row++];

        if (f[i].kind == kGWFieldChoice) {
            const char *p = f[i].choices;
            MenuHandle  menu;
            short       id = static_cast<short>(kChoiceMenuBase + row);
            short       chosen = 1;

            ToPascal(f[i].label, title);
            menu = NewMenu(id, title);
            if (menu == 0) continue;
            r->altCount = 0;
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
                    chosen = static_cast<short>(r->altCount);
                ToPascal(buf, title);
                AppendMenu(menu, title);
                p = (end != 0) ? end + 1 : 0;
            }
            InsertMenu(menu, -1);
            r->menuID = id;

            ToPascal("", title);
            r->popup = NewControl(reinterpret_cast<WindowPtr>(mDialog),
                                  &r->popupRect, title, true, chosen,
                                  id, 0, kPopupMenuProc, 0);
            continue;
        }

        if (f[i].kind == kGWFieldList) {
            r->scroll = NewControl(reinterpret_cast<WindowPtr>(mDialog),
                                   &r->scrollRect, title, true, 0, 0, 0,
                                   kScrollBarProc, 0);
            SyncScroll(r);
        }
    }
}

/*
 * Point the scroll bar at what the list holds: the range is however many
 * lines do not fit, and the value is how far down the view has been moved.
 */
/*
 * Open a popup and record what was chosen.
 *
 * The control definition draws a correct popup and does not track a click in
 * one -- proven twice now, on a monochrome port and a colour one. So it keeps
 * the drawing, which is the part it does well and the part that makes this
 * look like the system's own, and the menu is opened with PopUpMenuSelect,
 * which is the call the definition would have made.
 */
short PrefsWindow::PopChoice(ControlHandle ctl, short menuID)
{
    MenuHandle menu = GetMenuHandle(menuID);
    Rect       r;
    Point      tl;
    long       choice;
    short      item;

    if (ctl == 0 || menu == 0) return 0;
    r = (**ctl).contrlRect;
    tl.h = static_cast<short>(r.left + 1);
    tl.v = static_cast<short>(r.top + 1);
    LocalToGlobal(&tl);

    choice = PopUpMenuSelect(menu, tl.v, tl.h, GetControlValue(ctl));
    if (choice == 0) return 0;
    item = static_cast<short>(choice & 0xFFFF);
    SetControlValue(ctl, item);
    return item;
}

void PrefsWindow::SyncScroll(Row *r)
{
    TEHandle te = DialogTE();
    short    lines, shown, most, top;

    if (r == 0 || r->scroll == 0 || te == 0) return;

    lines = (*te)->nLines;
    shown = static_cast<short>(((*te)->viewRect.bottom -
                                (*te)->viewRect.top) / (*te)->lineHeight);
    if (shown < 1) shown = 1;
    most = static_cast<short>(lines - shown);
    if (most < 0) most = 0;
    top = static_cast<short>(((*te)->viewRect.top - (*te)->destRect.top) /
                             (*te)->lineHeight);
    if (top < 0) top = 0;
    if (top > most) top = most;

    SetControlMaximum(r->scroll, most);
    SetControlValue(r->scroll, top);
    HiliteControl(r->scroll, most > 0 ? 0 : 255);
}

void PrefsWindow::ScrollList(Row *r, short part)
{
    TEHandle te = DialogTE();
    short    was, now, most;

    if (r == 0 || r->scroll == 0 || te == 0) return;

    was = GetControlValue(r->scroll);
    most = GetControlMaximum(r->scroll);
    now = was;

    switch (part) {
    case kInUpButton:   now = static_cast<short>(was - 1); break;
    case kInDownButton: now = static_cast<short>(was + 1); break;
    case kInPageUp:     now = static_cast<short>(was - 4); break;
    case kInPageDown:   now = static_cast<short>(was + 4); break;
    case kInThumb:      now = GetControlValue(r->scroll);  break;
    default: break;
    }
    if (now < 0) now = 0;
    if (now > most) now = most;
    SetControlValue(r->scroll, now);

    if (now != was)
        TEScroll(0, static_cast<short>((was - now) * (*te)->lineHeight), te);
}

/*
 * Right-align each label against its field, the way the Internet control
 * panel does. A statText item draws from the left of its rectangle, so the
 * rectangle is moved rather than the text: measured here, after the dialog
 * exists and its font is set, because StringWidth needs the port.
 */
void PrefsWindow::AlignLabels()
{
    int i;

    if (mDialog == 0) return;
    SetPort(reinterpret_cast<GrafPtr>(mDialog));
    TextFont(kFontGeneva);
    TextSize(9);

    for (i = 0; i < mRowCount; i++) {
        short  type;
        Handle h;
        Rect   box;
        Str255 s;
        short  w;

        if (mRows[i].field == 0) continue;

        /*
         * Checkboxes are left the full width of the pane. Measuring them here
         * cut "Web proxy" to "Web pro": StringWidth answers in the port's
         * font and the control draws its title in the system font, which is
         * wider, so every title lost its tail. The white bars that shrinking
         * them was meant to cure were the port's background, and that is
         * fixed where it belongs.
         */
        if (mRows[i].field->kind == kGWFieldFlag) continue;

        if (mRows[i].labelItem == 0) continue;
        if (mRows[i].field->kind == kGWFieldList) continue;

        GetDialogItem(mDialog, mRows[i].labelItem, &type, &h, &box);
        ToPascal(mRows[i].field->label, s);
        w = StringWidth(s);
        box.right = static_cast<short>(kEntryLeft - 8);
        box.left = static_cast<short>(box.right - w - 2);
        if (box.left < kPaneLeft) box.left = kPaneLeft;
        SetDialogItem(mDialog, mRows[i].labelItem, type, h, &box);
    }
}

/* ------------------------------------------------------------------ */

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
        DropControls();
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

    v = static_cast<short>(kBoxTop + 14);
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
            SetRect(&r, kPaneLeft, static_cast<short>(v + 3),
                    static_cast<short>(kEntryLeft - 8),
                    static_cast<short>(v + 17));
            row->labelItem = items.Add(kTextItem, &r, f[i].label);
            SetRect(&row->popupRect, kEntryLeft, v, kPaneRight,
                    static_cast<short>(v + kEntryHeight + 3));
            break;

        case kGWFieldList:
            SetRect(&r, kPaneLeft, v, kPaneRight,
                    static_cast<short>(v + 13));
            row->labelItem = items.Add(kTextItem, &r, f[i].label);
            /* The field, with the scroll bar's width kept clear on the
             * right, exactly as the Signature box in Internet has it. */
            SetRect(&r, static_cast<short>(kPaneLeft + 4),
                    static_cast<short>(v + 19),
                    static_cast<short>(kPaneRight - kScrollWidth - 3),
                    static_cast<short>(v + 15 + kListHeight));
            row->item = items.Add(kEditItem, &r, mList);
            if (mFirstEdit == 0) mFirstEdit = row->item;
            SetRect(&row->scrollRect,
                    static_cast<short>(kPaneRight - kScrollWidth),
                    static_cast<short>(v + 15),
                    kPaneRight,
                    static_cast<short>(v + 15 + kListHeight + 1));
            break;

        default:
            SetRect(&r, kPaneLeft, static_cast<short>(v + 3),
                    static_cast<short>(kEntryLeft - 8),
                    static_cast<short>(v + 17));
            row->labelItem = items.Add(kTextItem, &r, f[i].label);
            SetRect(&r, static_cast<short>(kEntryLeft + 4),
                    static_cast<short>(v + 3),
                    static_cast<short>(kPaneRight - 4),
                    static_cast<short>(v + kEntryHeight));
            row->item = items.Add(kEditItem, &r, mValue[i]);
            if (mFirstEdit == 0) mFirstEdit = row->item;
            break;
        }

        if (f[i].kind == kGWFieldList)
            v = static_cast<short>(v + 15 + kListHeight + 12);
        else
            v = static_cast<short>(v + kRowHeight);

        if (f[i].hint != 0) {
            /* Under the field it describes, not at the far left of the pane
             * where it read as belonging to nothing. */
            SetRect(&r, (f[i].kind == kGWFieldFlag ||
                         f[i].kind == kGWFieldList)
                            ? kPaneLeft : kEntryLeft,
                    static_cast<short>(v - 5), kPaneRight,
                    static_cast<short>(v + 9));
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

    /*
     * The Dialog Manager draws editText items in its own font and statText in
     * the port's, which is why every panel had small labels beside large
     * field text. This settles both on Geneva, as the Internet control panel
     * has them.
     */
    SetDialogFont(kFontGeneva);

    ToPascal("Gateway Preferences", title);
    /* NewColorDialog: a classic GrafPort is monochrome and RGBForeColor on
     * one does nothing, which is what lost the Platinum once already. */
    mDialog = NewColorDialog(0, &bounds, title, true, noGrowDocProc,
                             reinterpret_cast<WindowPtr>(-1), true, 0,
                             items.Release());
    if (mDialog == 0) return;

    SetPort(reinterpret_cast<GrafPtr>(mDialog));
    TextFont(kFontGeneva);
    TextSize(9);
    {
        /*
         * Set once, on the port, rather than only inside Draw(). Every
         * control erases its own rectangle with the port's background before
         * it draws itself, and a checkbox's rectangle is as wide as the
         * caller made it -- which is where the white bars across the rows
         * came from.
         */
        RGBColor platinum;

        platinum.red = platinum.green = platinum.blue = kPlatinum;
        RGBBackColor(&platinum);
    }

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

    SetDialogDefaultItem(mDialog, kItemSave);

    MakeControls(group);
    AlignLabels();

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

    if (mFirstEdit != 0) SelectDialogItemText(mDialog, mFirstEdit, 0, 0);
}

/* ------------------------------------------------------------------ */

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

    if (mNote[0] != '\0') {
        MoveTo(kBoxLeft,
               static_cast<short>(port->portRect.bottom - 12 -
                                  kButtonHeight + 14));
        ToPascal(mNote, s);
        DrawString(s);
    }

    DrawDialog(mDialog);

    /*
     * The frame around each entry field and the list. The Dialog Manager
     * draws an editText item's text and not a border, so a dialog that wants
     * the look of the system's own control panels draws one.
     */
    RGBForeColor(&black);
    for (i = 0; i < mRowCount; i++) {
        short  type;
        Handle h;
        Rect   fr;

        if (mRows[i].field == 0 || mRows[i].item == 0) continue;
        if (mRows[i].field->kind == kGWFieldFlag) continue;

        GetDialogItem(mDialog, mRows[i].item, &type, &h, &fr);
        InsetRect(&fr, -3, -3);
        if (mRows[i].field->kind == kGWFieldList)
            fr.right = static_cast<short>(kPaneRight - kScrollWidth + 1);
        FrameRect(&fr);
    }
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
    DropControls();
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
            Point         where = event.where;
            ControlHandle ctl;
            short         cpart;
            int           i;

            if (FrontWindow() != mine) {
                SelectWindow(mine);
                return true;
            }

            SetPort(reinterpret_cast<GrafPtr>(mDialog));
            GlobalToLocal(&where);

            cpart = FindControl(where, mine, &ctl);
            if (cpart != 0 && ctl != 0) {
                /* The pane popup. */
                if (ctl == mGroupCtl) {
                    short g = PopChoice(ctl, kGroupMenuID);

                    if (g >= 1 && g - 1 != mGroup) {
                        ReadGroupBack();
                        BuildGroup(static_cast<short>(g - 1));
                        Draw();
                    }
                    return true;
                }
                /* A choice popup, or a list's scroll bar. */
                for (i = 0; i < mRowCount; i++) {
                    if (ctl == mRows[i].popup) {
                        if (PopChoice(ctl, mRows[i].menuID) > 0) mDirty = true;
                        return true;
                    }
                    if (ctl == mRows[i].scroll) {
                        if (cpart == kInThumb) {
                            TrackControl(ctl, where, 0);
                            ScrollList(&mRows[i], kInThumb);
                        } else if (TrackControl(ctl, where, 0) != 0) {
                            ScrollList(&mRows[i], cpart);
                        }
                        return true;
                    }
                }
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

    if (IsDialogEvent(&event)) {
        if (DialogSelect(&event, &which, &hit) && which == mDialog) {
            if (hit == kItemSave) Save();
            else if (hit == kItemRevert) Revert();
            else mDirty = true;
        }
        /* Typing into the list can have moved it. */
        for (hit = 0; hit < mRowCount; hit++)
            if (mRows[hit].scroll != 0) SyncScroll(&mRows[hit]);
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
