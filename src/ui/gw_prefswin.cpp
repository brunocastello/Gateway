/*
 * gw_prefswin.cpp - the Preferences window on Mac OS 9.
 *
 * Apple's Universal Interfaces, not Multiversal, and that is the point of the
 * file. The look this window copies -- the Internet control panel -- is drawn
 * by the Appearance Manager: the background is a theme brush, the entry
 * fields are kControlEditTextProc, the popups kControlPopupButtonProc, the
 * group box kControlGroupBoxTextTitleProc. Multiversal defines none of it, so
 * anything built on it renders the System 7 look however the rectangles are
 * arranged. An earlier version of this file learned that the long way.
 *
 * CLAUDE.md rule 3 already has the mechanism: Apple's headers go to the
 * translation units that need them and nowhere else. This is one of those;
 * src/main.cpp stays on Multiversal and the two meet only through
 * gw_prefswin.h, which carries no Toolbox type at all.
 *
 * Modeless, because a modal loop would stop GW_Poll() being called and every
 * proxy session would time out while the window was open.
 *
 * Values live in a shadow copy: one pane is on screen at a time, so a save
 * that read the controls would write the visible pane and revert the rest.
 */

#include "gw_prefswin.h"

#include <Appearance.h>
#include <ControlDefinitions.h>
#include <Controls.h>
#include <Events.h>
#include <Fonts.h>
#include <MacWindows.h>
#include <Menus.h>
#include <Quickdraw.h>
#include <TextEdit.h>

#include <stdio.h>
#include <string.h>

#include "../gw_config.h"
#include "../portable/gw_log.h"
#include "../portable/gw_prefsform.h"
#include "../portable/gw_util.h"

namespace {

const short kWinWidth    = 506;

const short kMargin      = 12;
const short kPopupTop    = 12;
const short kPopupHeight = 20;

const short kBoxTop      = 44;
const short kBoxLeft     = kMargin;
const short kBoxRight    = kWinWidth - kMargin;

const short kPaneLeft    = kBoxLeft + 14;
const short kPaneRight   = kBoxRight - 14;

/*
 * The field column starts a third across and stops short of the pane's right
 * edge, as the Identity box in the Internet control panel has it, rather than
 * being a fixed width hung off the right.
 */
const short kEntryLeft   = kPaneLeft + 158;
const short kEntryWidth  = 228;
const short kEntryHeight = 16;
const short kRowHeight   = 21;
const short kHintHeight  = 12;
const short kListHeight  = 92;
const short kButtonW     = 74;
const short kButtonH     = 20;
const short kScrollW     = 16;

const short kGroupMenuID    = 200;
const short kChoiceMenuBase = 201;

const short kMaxRows   = 24;
const short kMaxFields = 64;
const short kValueMax  = 256;
const short kListMax   = 3072;

/*
 * A secret on file that has not been retyped. A token is pasted rather than
 * edited, so the field shows this: leave it and the stored value is kept,
 * type anything and that is the new value.
 */
const char *const kKeptSecret = "\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5";

void ToPascal(const char *src, Str255 dst)
{
    size_t n = strlen(src);

    if (n > 255) n = 255;
    dst[0] = (unsigned char)n;
    memcpy(dst + 1, src, n);
}

struct Row {
    const GWPrefField *field;
    int                index;
    ControlHandle      ctl;     /* edit text, checkbox or popup  */
    ControlHandle      label;
    ControlHandle      hint;
    ControlHandle      scroll;  /* list fields only              */
    short              menuID;
    int                altCount;
};

class PrefsWindow {
public:
    PrefsWindow()
        : mWindow(0), mRoot(0), mGroupPopup(0), mGroupBox(0), mSave(0),
          mRevert(0), mGroup(0), mRowCount(0)
    {
        memset(mValue, 0, sizeof(mValue));
        memset(mList, 0, sizeof(mList));
        memset(mRows, 0, sizeof(mRows));
        memset(mNote, 0, sizeof(mNote));
    }

    bool Open();
    void Close();
    bool IsOpen() const { return mWindow != 0; }
    bool HandleEvent(EventRecord *ev);
    void Idle();
    void EditCommand(int cmd);
    bool CanEdit() const;

private:
    void LoadValues();
    void BuildPane(int group);
    void DropPane();
    void ReadPaneBack();
    void Save();
    void Revert();
    void SetText(ControlHandle c, const char *s);
    void GetText(ControlHandle c, char *out, size_t cap);
    void SyncScroll(Row *r);
    void ScrollTo(Row *r);
    TEHandle FieldTE(Row *r);
    ControlHandle Focus() const;

    WindowPtr     mWindow;
    ControlHandle mRoot;
    ControlHandle mGroupPopup;
    ControlHandle mGroupBox;
    ControlHandle mSave;
    ControlHandle mRevert;
    int           mGroup;
    Row           mRows[kMaxRows];
    int           mRowCount;

    char mValue[kMaxFields][kValueMax];
    char mList[kListMax];
    char mNote[128];
};

PrefsWindow *gPrefs = 0;

/* ------------------------------------------------------------------ */

void PrefsWindow::SetText(ControlHandle c, const char *s)
{
    if (c == 0) return;
    SetControlData(c, kControlEntireControl, kControlEditTextTextTag,
                   (Size)strlen(s), (Ptr)s);
}

void PrefsWindow::GetText(ControlHandle c, char *out, size_t cap)
{
    Size actual = 0;

    if (cap > 0) out[0] = '\0';
    if (c == 0) return;
    if (GetControlData(c, kControlEntireControl, kControlEditTextTextTag,
                       (Size)(cap - 1), (Ptr)out, &actual) != noErr)
        return;
    if (actual < 0) actual = 0;
    if ((size_t)actual > cap - 1) actual = (Size)(cap - 1);
    out[actual] = '\0';
}

ControlHandle PrefsWindow::Focus() const
{
    ControlHandle c = 0;

    if (mWindow == 0) return 0;
    GetKeyboardFocus(mWindow, &c);
    return c;
}

bool PrefsWindow::CanEdit() const
{
    ControlHandle c = Focus();
    int           i;

    if (c == 0) return false;
    for (i = 0; i < mRowCount; i++)
        if (mRows[i].ctl == c && mRows[i].field != 0 &&
            mRows[i].field->kind != kGWFieldFlag &&
            mRows[i].field->kind != kGWFieldChoice)
            return true;
    return false;
}

/* ------------------------------------------------------------------ */

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
                n = strlen(pattern);
                if (used + n + 2 >= sizeof(mList)) break;
                memcpy(mList + used, pattern, n);
                used += n;
                mList[used++] = '\r';
            }
            mList[used] = '\0';
            continue;
        }
        if (f[i].kind == kGWFieldSecret) {
            const char *v = GWConfig_Str(f[i].key, "");

            strcpy(mValue[i], (v != 0 && v[0] != '\0') ? kKeptSecret : "");
            continue;
        }
        {
            const char *v = GWConfig_Str(f[i].key, f[i].def);

            if (v == 0) v = "";
            gw_copy_n(mValue[i], kValueMax, v, strlen(v));
        }
    }
}

void PrefsWindow::ReadPaneBack()
{
    int i;

    for (i = 0; i < mRowCount; i++) {
        Row *r = &mRows[i];

        if (r->field == 0 || r->ctl == 0) continue;

        switch (r->field->kind) {
        case kGWFieldFlag:
            strcpy(mValue[r->index], GetControlValue(r->ctl) ? "1" : "0");
            break;

        case kGWFieldChoice: {
            MenuHandle menu = GetMenuHandle(r->menuID);
            short      item = GetControlValue(r->ctl);

            if (menu != 0 && item >= 1 && item <= r->altCount) {
                Str255 s;
                size_t n;

                GetMenuItemText(menu, item, s);
                n = s[0];
                if (n > kValueMax - 1) n = kValueMax - 1;
                memcpy(mValue[r->index], s + 1, n);
                mValue[r->index][n] = '\0';
            }
            break;
        }

        case kGWFieldList:
            GetText(r->ctl, mList, sizeof(mList));
            break;

        default:
            GetText(r->ctl, mValue[r->index], kValueMax);
            break;
        }
    }
}

/* ------------------------------------------------------------------ */

void PrefsWindow::DropPane()
{
    int i;

    for (i = 0; i < mRowCount; i++) {
        if (mRows[i].ctl != 0)    DisposeControl(mRows[i].ctl);
        if (mRows[i].label != 0)  DisposeControl(mRows[i].label);
        if (mRows[i].hint != 0)   DisposeControl(mRows[i].hint);
        if (mRows[i].scroll != 0) DisposeControl(mRows[i].scroll);
        if (mRows[i].menuID != 0) {
            DeleteMenu(mRows[i].menuID);
            DisposeMenu(GetMenuHandle(mRows[i].menuID));
        }
    }
    if (mGroupBox != 0) { DisposeControl(mGroupBox); mGroupBox = 0; }
    memset(mRows, 0, sizeof(mRows));
    mRowCount = 0;
}

/*
 * A static text control in the small system font, which is what the Internet
 * control panel labels its fields with. Right-aligned against the field
 * column for a field's own label, left for a hint.
 */
/*
 * The system font, which is what Mac OS 9 puts in a dialog and what the
 * Internet control panel uses. kControlFontSmallSystemFont is Geneva 10 and
 * made every panel read as shrunken beside the reference.
 */
void SystemFont(ControlHandle c)
{
    ControlFontStyleRec style;

    if (c == 0) return;
    style.flags = kControlUseFontMask;
    style.font = kControlFontSmallSystemFont;
    SetControlFontStyle(c, &style);
}

/* The one exception: the pane selector keeps the larger font it has now. */
void BigFont(ControlHandle c)
{
    ControlFontStyleRec style;

    if (c == 0) return;
    style.flags = kControlUseFontMask;
    style.font = kControlFontBigSystemFont;
    SetControlFontStyle(c, &style);
}

ControlHandle MakeLabel(WindowPtr w, const Rect *r, const char *text,
                        SInt16 just, Boolean small)
{
    ControlFontStyleRec style;
    ControlHandle       c;
    Str255              s;

    ToPascal("", s);
    c = NewControl(w, r, s, true, 0, 0, 0, kControlStaticTextProc, 0);
    if (c == 0) return 0;

    /*
     * A static text control does not display its title. The title is what
     * NewControl takes and what a checkbox draws; this one keeps its text in
     * kControlStaticTextTextTag, which is why every label in the window came
     * out blank while the checkboxes beside them were fine.
     */
    SetControlData(c, kControlEntireControl, kControlStaticTextTextTag,
                   (Size)strlen(text), (Ptr)text);

    style.flags = kControlUseFontMask | kControlUseJustMask;
    style.font = small ? kControlFontSmallSystemFont
                       : kControlFontSmallSystemFont;
    style.just = just;
    SetControlFontStyle(c, &style);
    return c;
}

void PrefsWindow::BuildPane(int group)
{
    int                count = 0;
    const GWPrefField *f = gw_prefsform_fields(&count);
    int                i;
    short              v;
    Rect               r;
    Str255             s;

    DropPane();
    mGroup = group;
    if (count > kMaxFields) count = kMaxFields;

    v = (short)(kBoxTop + 20);
    for (i = 0; i < count && mRowCount < kMaxRows; i++) {
        Row *row;

        if (f[i].group != group) continue;
        row = &mRows[mRowCount];
        row->field = &f[i];
        row->index = i;

        switch (f[i].kind) {
        case kGWFieldFlag: {
            SetRect(&r, kPaneLeft, v, kPaneRight, (short)(v + 17));
            ToPascal(f[i].label, s);
            row->ctl = NewControl(mWindow, &r, s, true,
                                  mValue[i][0] == '1' ? 1 : 0, 0, 1,
                                  kControlCheckBoxProc, 0);
            SystemFont(row->ctl);
            break;
        }

        case kGWFieldChoice: {
            const char *p = f[i].choices;
            MenuHandle  menu;
            short       id = (short)(kChoiceMenuBase + mRowCount);
            short       chosen = 1;

            SetRect(&r, kPaneLeft, (short)(v + 3),
                    (short)(kEntryLeft - 6), (short)(v + 15));
            row->label = MakeLabel(mWindow, &r, f[i].label, teFlushRight, false);

            ToPascal(f[i].label, s);
            menu = NewMenu(id, s);
            if (menu != 0) {
                while (p != 0 && *p != '\0') {
                    const char *end = strchr(p, '|');
                    size_t      n = (end != 0) ? (size_t)(end - p) : strlen(p);
                    char        buf[32];

                    if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
                    memcpy(buf, p, n);
                    buf[n] = '\0';
                    row->altCount++;
                    if (gw_stricmp(buf, mValue[i]) == 0)
                        chosen = (short)row->altCount;
                    ToPascal(buf, s);
                    AppendMenu(menu, s);
                    p = (end != 0) ? end + 1 : 0;
                }
                InsertMenu(menu, kInsertHierarchicalMenu);
                row->menuID = id;

                SetRect(&r, kEntryLeft, v,
                        (short)(kEntryLeft + kEntryWidth),
                        (short)(v + kEntryHeight + 1));
                ToPascal("", s);
                row->ctl = NewControl(mWindow, &r, s, true, chosen, id, 0,
                                      (short)(kControlPopupButtonProc +
                                       kControlPopupFixedWidthVariant), 0);
                if (row->ctl != 0) {
                    SetControlValue(row->ctl, chosen);
                    SystemFont(row->ctl);
                }
            }
            break;
        }

        case kGWFieldList:
            SetRect(&r, kPaneLeft, v, kPaneRight, (short)(v + 18));
            row->label = MakeLabel(mWindow, &r, f[i].label, teFlushLeft, false);

            SetRect(&r, kPaneLeft, (short)(v + 20),
                    (short)(kPaneRight - kScrollW + 1),
                    (short)(v + 20 + kListHeight));
            ToPascal("", s);
            row->ctl = NewControl(mWindow, &r, s, true, 0, 0, 0,
                                  kControlEditTextProc, 0);
            SetText(row->ctl, mList);
            SystemFont(row->ctl);
            {
                TEHandle te = FieldTE(row);

                if (te != 0) (*te)->crOnly = -1;   /* one host per line */
            }

            SetRect(&r, (short)(kPaneRight - kScrollW), (short)(v + 20),
                    kPaneRight, (short)(v + 20 + kListHeight));
            row->scroll = NewControl(mWindow, &r, s, true, 0, 0, 0,
                                     kControlScrollBarLiveProc, 0);
            SyncScroll(row);
            break;

        default:
            SetRect(&r, kPaneLeft, (short)(v + 3),
                    (short)(kEntryLeft - 6), (short)(v + 15));
            row->label = MakeLabel(mWindow, &r, f[i].label, teFlushRight, false);

            SetRect(&r, kEntryLeft, v,
                    (short)(kEntryLeft + kEntryWidth),
                    (short)(v + kEntryHeight));
            ToPascal("", s);
            row->ctl = NewControl(mWindow, &r, s, true, 0, 0, 0,
                                  f[i].kind == kGWFieldSecret
                                      ? kControlEditTextPasswordProc
                                      : kControlEditTextProc, 0);
            SetText(row->ctl, mValue[i]);
            SystemFont(row->ctl);
            {
                /* One line, no wrapping. The Scope value is long and was
                 * folding onto a second line inside a field one line tall. */
                TEHandle te = FieldTE(row);

                if (te != 0) (*te)->crOnly = -1;
            }
            break;
        }

        if (f[i].kind == kGWFieldList)
            v = (short)(v + 20 + kListHeight + 8);
        else
            v = (short)(v + kRowHeight);

        if (f[i].hint != 0) {
            /* The whole pane, in Geneva: the long ones were running off the
             * right and losing their last word. */
            SetRect(&r, (f[i].kind == kGWFieldFlag ||
                         f[i].kind == kGWFieldList) ? kPaneLeft : kEntryLeft,
                    (short)(v - 4), kPaneRight, (short)(v + 9));
            row->hint = MakeLabel(mWindow, &r, f[i].hint, teFlushLeft, true);
            v = (short)(v + kHintHeight);
        }
        v = (short)(v + 2);
        mRowCount++;
    }

    /* The group box, drawn by the theme rather than by a FrameRect with a
     * gap painted back into its top rule. */
    SetRect(&r, kBoxLeft, kBoxTop, kBoxRight, (short)(v + 10));
    ToPascal(gw_prefsform_group_name(group), s);
    mGroupBox = NewControl(mWindow, &r, s, true, 0, 0, 1,
                           kControlGroupBoxTextTitleProc, 0);
    {
        ControlFontStyleRec style;

        style.flags = kControlUseFontMask;
        style.font = kControlFontSmallBoldSystemFont;
        if (mGroupBox != 0) SetControlFontStyle(mGroupBox, &style);
    }

    {
        short h = (short)(v + 12 + 16 + kButtonH + kMargin);

        SizeWindow(mWindow, kWinWidth, h, true);
        if (mSave != 0)
            MoveControl(mSave, (short)(kBoxRight - kButtonW - 4),
                        (short)(h - kMargin - kButtonH - 4));
        if (mRevert != 0)
            MoveControl(mRevert, (short)(kBoxRight - 2 * kButtonW - 18),
                        (short)(h - kMargin - kButtonH - 4));
    }

    /*
     * No field is focused to begin with. Focusing one on every pane put the
     * ring round a field nobody had clicked, on every panel, and made it look
     * as though the window were always mid-edit. A click or Tab sets it.
     */
}

/* The TextEdit record inside an edit text control, which is what the scroll
 * bar has to move. The control keeps it under kControlEditTextTEHandleTag. */
TEHandle PrefsWindow::FieldTE(Row *r)
{
    Size     actual = 0;
    TEHandle te = 0;

    if (r == 0 || r->ctl == 0) return 0;
    if (GetControlData(r->ctl, kControlEntireControl,
                       kControlEditTextTEHandleTag,
                       sizeof(te), (Ptr)&te, &actual) != noErr)
        return 0;
    return te;
}

/*
 * Move the field to wherever the scroll bar now says. Setting the bar's range
 * was never enough on its own: a click tracked the thumb and changed the
 * value and nothing read it, so the list would not scroll.
 */
void PrefsWindow::ScrollTo(Row *r)
{
    TEHandle te = FieldTE(r);
    short    want, have;

    if (te == 0 || r->scroll == 0) return;
    want = GetControlValue(r->scroll);
    have = (short)(((*te)->viewRect.top - (*te)->destRect.top) /
                   (*te)->lineHeight);
    if (want != have)
        TEScroll(0, (short)((have - want) * (*te)->lineHeight), te);
}

void PrefsWindow::SyncScroll(Row *r)
{
    TEHandle te = FieldTE(r);
    short    lines, shown, most;

    if (r == 0 || r->scroll == 0 || te == 0) return;

    lines = (*te)->nLines;
    shown = (short)(((*te)->viewRect.bottom - (*te)->viewRect.top) /
                    (*te)->lineHeight);
    if (shown < 1) shown = 1;
    most = (short)(lines - shown);
    if (most < 0) most = 0;
    SetControlMaximum(r->scroll, most);
    HiliteControl(r->scroll, most > 0 ? 0 : 255);
}

/* ------------------------------------------------------------------ */

bool PrefsWindow::Open()
{
    Rect   bounds;
    Str255 s;
    int    g;

    if (mWindow != 0) {
        SelectWindow(mWindow);
        return true;
    }

    LoadValues();
    mNote[0] = '\0';

    SetRect(&bounds, 0, 0, kWinWidth, 320);
    OffsetRect(&bounds,
               (short)((qd.screenBits.bounds.right - kWinWidth) / 2), 60);
    ToPascal("Gateway Preferences", s);
    mWindow = NewCWindow(0, &bounds, s, false, noGrowDocProc,
                         (WindowPtr)-1, true, 0);
    if (mWindow == 0) return false;

    /*
     * The Platinum ground, from the theme rather than painted. This one call
     * is the difference between a window that looks like Mac OS 9 and a grey
     * rectangle that approximates one.
     */
    SetThemeWindowBackground(mWindow, kThemeBrushDialogBackgroundActive,
                             false);
    CreateRootControl(mWindow, &mRoot);
    SetPort(mWindow);

    {
        Rect r;

        SetRect(&r, kMargin, (short)(kPopupTop + 3), 96,
                (short)(kPopupTop + 20));
        {
            ControlHandle lab = MakeLabel(mWindow, &r, "Settings for:",
                                          teFlushRight, false);

            BigFont(lab);          /* the one control that keeps it */
        }

        if (GetMenuHandle(kGroupMenuID) == 0) {
            MenuHandle menu;

            ToPascal("Settings", s);
            menu = NewMenu(kGroupMenuID, s);
            if (menu != 0) {
                for (g = 0; g < kGWGroupCount; g++) {
                    ToPascal(gw_prefsform_group_name(g), s);
                    AppendMenu(menu, s);
                }
                InsertMenu(menu, kInsertHierarchicalMenu);
            }
        }
        SetRect(&r, 104, kPopupTop, 262, (short)(kPopupTop + kPopupHeight));
        ToPascal("", s);
        mGroupPopup = NewControl(mWindow, &r, s, true, 1, kGroupMenuID, 0,
                                 (short)(kControlPopupButtonProc +
                                       kControlPopupFixedWidthVariant), 0);
        if (mGroupPopup != 0) {
            SetControlValue(mGroupPopup, 1);
            BigFont(mGroupPopup);
        }

        SetRect(&r, 0, 0, kButtonW, kButtonH);
        ToPascal("Save", s);
        mSave = NewControl(mWindow, &r, s, true, 0, 0, 1,
                           kControlPushButtonProc, 0);
        ToPascal("Revert", s);
        mRevert = NewControl(mWindow, &r, s, true, 0, 0, 1,
                             kControlPushButtonProc, 0);
        SystemFont(mSave);
        SystemFont(mRevert);
        /* The system's own default ring, not one drawn round the button. */
        if (mSave != 0) {
            Boolean on = true;

            SetControlData(mSave, kControlEntireControl,
                           kControlPushButtonDefaultTag,
                           sizeof(on), (Ptr)&on);
        }
    }

    BuildPane(0);
    ShowWindow(mWindow);
    return true;
}

void PrefsWindow::Close()
{
    if (mWindow == 0) return;
    DropPane();
    DisposeWindow(mWindow);          /* takes the remaining controls */
    mWindow = 0;
    mRoot = mGroupPopup = mSave = mRevert = 0;
    DeleteMenu(kGroupMenuID);
    DisposeMenu(GetMenuHandle(kGroupMenuID));
}

/* ------------------------------------------------------------------ */

bool PrefsWindow::HandleEvent(EventRecord *ev)
{
    if (mWindow == 0 || ev == 0) return false;

    switch (ev->what) {
    case mouseDown: {
        WindowPtr win;
        short     part = FindWindow(ev->where, &win);

        if (win != mWindow) return false;
        if (part == inGoAway) {
            if (TrackGoAway(mWindow, ev->where)) Close();
            return true;
        }
        if (part == inDrag) {
            Rect limit = qd.screenBits.bounds;

            InsetRect(&limit, 4, 4);
            DragWindow(mWindow, ev->where, &limit);
            return true;
        }
        if (part != inContent) return false;
        if (FrontWindow() != mWindow) {
            SelectWindow(mWindow);
            return true;
        }
        {
            Point         where = ev->where;
            ControlHandle hit;
            int           i;

            SetPort(mWindow);
            GlobalToLocal(&where);

            /*
             * HandleControlClick, not TrackControl: it is the Appearance
             * entry point, and it is what makes an edit text control take a
             * click as a selection and a popup open its menu.
             */
            /*
             * FindControl, not FindControlUnderMouse. With a root control in
             * the window the latter answers the root -- which contains
             * everything and does nothing when clicked -- so every click was
             * delivered to a control that ignores them and the window looked
             * dead.
             */
            {
                short part = FindControl(where, mWindow, &hit);

                if (part == 0 || hit == 0 || hit == mRoot) return true;
            }
            /*
             * Focus first. An edit text control will not place a caret for a
             * click unless it already has the keyboard focus, which is why
             * the fields were not editable once the pane stopped focusing one
             * on the way in.
             */
            for (i = 0; i < mRowCount; i++) {
                if (mRows[i].ctl != hit || mRows[i].field == 0) continue;
                if (mRows[i].field->kind == kGWFieldFlag) break;
                if (mRows[i].field->kind == kGWFieldChoice) break;
                SetKeyboardFocus(mWindow, hit, kControlEditTextPart);
                break;
            }
            HandleControlClick(hit, where, ev->modifiers, 0);

            if (hit == mGroupPopup) {
                short g = (short)(GetControlValue(hit) - 1);

                if (g >= 0 && g < kGWGroupCount && g != mGroup) {
                    ReadPaneBack();
                    BuildPane(g);
                    InvalRect(&mWindow->portRect);
                }
                return true;
            }
            if (hit == mSave)   { Save();   return true; }
            if (hit == mRevert) { Revert(); return true; }

            /*
             * A checkbox is tracked by HandleControlClick and toggled by its
             * owner -- the toolbox hilites it and reports the hit and leaves
             * the value alone, which is why none of them could be unticked.
             */
            for (i = 0; i < mRowCount; i++) {
                if (mRows[i].ctl != hit || mRows[i].field == 0) continue;
                if (mRows[i].field->kind != kGWFieldFlag) break;
                SetControlValue(hit, GetControlValue(hit) ? 0 : 1);
                return true;
            }
            for (i = 0; i < mRowCount; i++) {
                if (mRows[i].scroll == 0) continue;
                if (hit == mRows[i].scroll) ScrollTo(&mRows[i]);
                SyncScroll(&mRows[i]);
            }
            return true;
        }
    }

    case keyDown:
    case autoKey: {
        char ch = (char)(ev->message & charCodeMask);
        char code = (char)((ev->message & keyCodeMask) >> 8);

        if (FrontWindow() != mWindow) return false;
        if (ev->modifiers & cmdKey) return false;   /* the menu bar's */

        if (ch == '\t') {
            AdvanceKeyboardFocus(mWindow);
            return true;
        }
        if (ch == '\r' || ch == 3) { Save(); return true; }
        {
            ControlHandle c = Focus();
            int           i;

            if (c != 0) HandleControlKey(c, code, ch, ev->modifiers);
            for (i = 0; i < mRowCount; i++)
                if (mRows[i].scroll != 0) SyncScroll(&mRows[i]);
        }
        return true;
    }

    case updateEvt:
        if ((WindowPtr)ev->message != mWindow) return false;
        BeginUpdate(mWindow);
        SetPort(mWindow);
        EraseRect(&mWindow->portRect);   /* the theme brush fills it */
        UpdateControls(mWindow, mWindow->visRgn);
        if (mNote[0] != '\0') {
            Str255 s;

            TextFont(applFont);
            TextSize(9);
            MoveTo(kMargin, (short)(mWindow->portRect.bottom - kMargin - 6));
            ToPascal(mNote, s);
            DrawString(s);
        }
        EndUpdate(mWindow);
        return true;

    case activateEvt:
        if ((WindowPtr)ev->message != mWindow) return false;
        return true;

    default:
        return false;
    }
}

void PrefsWindow::Idle()
{
    if (mWindow == 0) return;
    if (FrontWindow() == mWindow) IdleControls(mWindow);
}

void PrefsWindow::EditCommand(int cmd)
{
    ControlHandle c = Focus();

    if (c == 0) return;
    switch (cmd) {
    case kGWEditCut:       HandleControlKey(c, 0, 'x', cmdKey); break;
    case kGWEditCopy:      HandleControlKey(c, 0, 'c', cmdKey); break;
    case kGWEditPaste:     HandleControlKey(c, 0, 'v', cmdKey); break;
    case kGWEditClear:     HandleControlKey(c, 0, 8, 0);        break;
    case kGWEditSelectAll: HandleControlKey(c, 0, 'a', cmdKey); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */

void PrefsWindow::Revert()
{
    LoadValues();
    BuildPane(mGroup);
    strcpy(mNote, "Reverted to what is in the file.");
    InvalRect(&mWindow->portRect);
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

    ReadPaneBack();
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

            memcpy(store, mList, sizeof(store));
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
            strcmp(mValue[i], kKeptSecret) == 0)
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
        sprintf(mNote, "%d saved, %d refused -- see the log.", written, failed);
    else if (restart > 0)
        sprintf(mNote, "Saved. %d take effect on restart.", restart);
    else if (written > 0)
        strcpy(mNote, "Saved.");
    else
        strcpy(mNote, "Nothing had changed.");
    gw_log("preferences: %s", mNote);

    LoadValues();
    BuildPane(mGroup);
    InvalRect(&mWindow->portRect);
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

int GWPrefsWin_HandleEvent(void *event)
{
    if (gPrefs == 0 || event == 0) return 0;
    return gPrefs->HandleEvent((EventRecord *)event) ? 1 : 0;
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
