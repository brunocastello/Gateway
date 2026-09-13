/*
 * gw_prefswin.cpp - the Preferences window on Mac OS 9.
 *
 * Multiversal only, exactly like src/main.cpp: this file reaches Gateway
 * through the plain-C headers in src/ and touches no Open Transport.
 *
 * Modeless, and that is not a style choice. ModalDialog() runs its own event
 * loop, so GW_Poll() would stop being called and every proxy session would
 * sit there until it timed out while somebody read the labels. The window is
 * driven from the one WaitNextEvent loop in main.cpp, which is CLAUDE.md
 * rule 6 applied to the user interface.
 *
 * The fields come from src/portable/gw_prefsform.c. Nothing about which
 * preferences exist, what they are called or what they may hold is decided
 * here -- this file knows how to draw a checkbox, an entry field and a list,
 * and which of the three a field wants.
 *
 * Values live in a shadow copy rather than in the controls. Only one group is
 * on screen at a time, so the controls for the other six do not exist; a save
 * that read the controls would write the visible group and silently revert
 * the rest.
 */

#include "gw_prefswin.h"

#include <Events.h>
#include <Fonts.h>
#include <Menus.h>
/*
 * Multiverse.h is where Multiversal puts the managers it gives no header of
 * their own -- the Control Manager, as main.cpp notes, and the Scrap Manager
 * for ZeroScrap(). There is no Scrap.h to include and adding one does not
 * fail to find a declaration, it fails to find the file.
 */
#include <Multiverse.h>
#include <Quickdraw.h>
#include <TextEdit.h>          /* TEToScrap, TEFromScrap */
#include <Windows.h>

#include <cstdio>
#include <cstring>

#include "../gw_config.h"
#include "../gw_core.h"
#include "../portable/gw_log.h"
#include "../portable/gw_prefsform.h"
#include "../portable/gw_util.h"

namespace {

/*
 * Laid out after the TCP/IP control panel: the popup that chooses what you
 * are looking at on top, and a framed box of that page's settings below it.
 * Nothing down the side, which is what forced "Mail upstream" to be drawn as
 * "Mail upstrea".
 */
const short kWinWidth   = 468;

const short kPopupTop   = 10;
const short kPopupHeight = 20;

const short kBoxTop     = kPopupTop + kPopupHeight + 10;
const short kBoxLeft    = 10;
const short kBoxRight   = kWinWidth - 10;

const short kPaneLeft   = kBoxLeft + 12;
const short kPaneRight  = kBoxRight - 12;

const short kRowHeight  = 20;       /* a field with no hint                 */
const short kHintHeight = 12;
const short kFieldWidth = 168;
const short kEntryLeft  = kPaneRight - kFieldWidth;
const short kListHeight = 72;

const short kButtonHeight = 20;

const unsigned short kPlatinum = 0xDDDD;

/*
 * The popup CDEF, by value, the way main.cpp spells the window and scroll bar
 * procs. popupMenuProc is CDEF 1008; the control's minimum carries the menu
 * id and its maximum the width to reserve for the title.
 */
const short kPopupMenuProc = 1008;
const short kGroupMenuID   = 200;
const short kChoiceMenuBase = 201;

const short kFontGeneva = 3;

const short kMaxRows    = 24;       /* fields drawn in one group            */
const short kMaxFields  = 64;
const short kValueMax   = 256;
const short kListMax    = 3072;

/* Controls the window owns, by kind, so a click can be told what it hit. */
const long kRefGroup  = 'GRUP';
const long kRefCheck  = 'CHEK';
const long kRefChoice = 'CHOI';
const long kRefSave   = 'SAVE';
const long kRefRevert = 'RVRT';

void ToPascal(const char *src, Str255 dst)
{
    size_t n = std::strlen(src);

    if (n > 255) n = 255;
    dst[0] = static_cast<unsigned char>(n);
    std::memcpy(dst + 1, src, n);
}

/*
 * A secret that is on file but has not been retyped.
 *
 * TextEdit has no password mode, and giving one to it would mean keeping the
 * real characters somewhere while showing others, then reconciling the two on
 * every keystroke. A token is pasted rather than edited anyway, so the field
 * shows this instead: leave it alone and the stored value is kept, type
 * anything and what you typed is the new value. The string cannot collide
 * with a real secret because no provider issues one made of bullets.
 */
const char *const kKeptSecret = "\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5";  /* •••••••• */

struct Row {
    const GWPrefField *field;
    int                index;      /* into the shadow value table            */
    Rect               hit;        /* checkbox: unused. entry: the TE frame  */
    ControlHandle      control;    /* flag and choice fields                 */
    short              menuID;     /* choice fields: the popup's menu        */
    int                altCount;   /* how many items it has                  */
    TEHandle           te;         /* entry and list fields                  */
    short              labelV;
    short              hintV;
};

class PrefsWindow {
public:
    PrefsWindow()
        : mWindow(0), mGroup(0), mRowCount(0), mFocus(-1), mDirty(false)
    {
        std::memset(mValue, 0, sizeof(mValue));
        std::memset(mList, 0, sizeof(mList));
        std::memset(mRows, 0, sizeof(mRows));
        mGroupPopup = 0;
        mSave = 0;
        mRevert = 0;
        mWinHeight = 300;
        mBoxBottom = 0;
        mButtonTop = 0;
    }

    bool Open();
    void Close();
    bool IsOpen() const { return mWindow != 0; }
    WindowPtr Window() const { return mWindow; }

    bool HandleEvent(EventRecord &event);
    void Idle();
    void EditCommand(int cmd);
    bool CanEdit() const { return mFocus >= 0 && mRows[mFocus].te != 0; }

private:
    void LoadValues();
    void BuildGroup(int group);
    void TearDownGroup();
    void ReadGroupBack();
    void Draw();
    void DrawPane();
    bool HandleClick(Point where);
    void HandleKey(char ch);
    void SetFocus(int row);
    void Save();
    void Revert();
    void Note(const char *text);
    short MeasureGroup(int group) const;
    void  LayoutWindow(int group);

    WindowPtr     mWindow;
    int           mGroup;
    Row           mRows[kMaxRows];
    int           mRowCount;
    int           mFocus;
    bool          mDirty;
    ControlHandle mGroupPopup;
    ControlHandle mSave;
    ControlHandle mRevert;
    short         mWinHeight;
    short         mBoxBottom;
    short         mButtonTop;

    char          mValue[kMaxFields][kValueMax];
    char          mList[kListMax];
    char          mNote[128];
};

PrefsWindow *gPrefs = 0;

/* ------------------------------------------------------------------ */

void PrefsWindow::Note(const char *text)
{
    std::strncpy(mNote, text, sizeof(mNote) - 1);
    mNote[sizeof(mNote) - 1] = '\0';
    if (mWindow != 0) {
        Rect r;
        SetRect(&r, kBoxLeft, mButtonTop,
                static_cast<short>(kBoxRight - 160),
                static_cast<short>(mButtonTop + kButtonHeight));
        InvalRect(&r);
    }
}

/*
 * Read the file into the shadow copy.
 *
 * Every field, not only the visible group: the window is a view of the whole
 * file and a save compares against what was read here, so a value nobody
 * looked at has to be present and unchanged rather than absent and rewritten.
 */
void PrefsWindow::LoadValues()
{
    int count = 0;
    const GWPrefField *f = gw_prefsform_fields(&count);
    int i;

    if (count > kMaxFields) count = kMaxFields;

    for (i = 0; i < count; i++) {
        if (f[i].kind == kGWFieldList) {
            char  pattern[256];
            int   k;
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
                mList[used++] = '\r';        /* TextEdit's line ending */
            }
            mList[used] = '\0';
            continue;
        }

        if (f[i].kind == kGWFieldSecret) {
            const char *v = GWConfig_Str(f[i].key, "");

            /* Present but not shown; absent stays empty so it reads as unset. */
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

/* ------------------------------------------------------------------ */

/*
 * How tall the box has to be for one group.
 *
 * The window is sized to the page rather than to the largest page, because
 * the largest is Wayback -- ten fields and a list -- and making every other
 * page that tall would leave five of them mostly empty. Measuring is the same
 * walk BuildGroup does, which is why the two step by the same amounts.
 */
short PrefsWindow::MeasureGroup(int group) const
{
    int                count = 0;
    const GWPrefField *f = gw_prefsform_fields(&count);
    int                i, rows = 0;
    short              v = 0;

    if (count > kMaxFields) count = kMaxFields;
    for (i = 0; i < count && rows < kMaxRows; i++) {
        if (f[i].group != group) continue;
        if (f[i].kind == kGWFieldList)
            v = static_cast<short>(v + 13 + kListHeight + 3);
        else
            v = static_cast<short>(v + kRowHeight);
        if (f[i].hint != 0) v = static_cast<short>(v + kHintHeight);
        v = static_cast<short>(v + 2);
        rows++;
    }
    return v;
}

/*
 * Put the window, its box and its buttons where this group needs them, and
 * resize the window to match. The Wayback page ran off the bottom and drew
 * its host list over the Save button because the height was a constant.
 */
void PrefsWindow::LayoutWindow(int group)
{
    short inner = MeasureGroup(group);
    short h;

    if (inner < 60) inner = 60;
    mBoxBottom = static_cast<short>(kBoxTop + 10 + inner + 8);
    mButtonTop = static_cast<short>(mBoxBottom + 12);
    h = static_cast<short>(mButtonTop + kButtonHeight + 12);
    mWinHeight = h;

    if (mWindow != 0) {
        SizeWindow(mWindow, kWinWidth, h, true);
        if (mSave != 0)
            MoveControl(mSave, static_cast<short>(kBoxRight - 74), mButtonTop);
        if (mRevert != 0)
            MoveControl(mRevert, static_cast<short>(kBoxRight - 156),
                        mButtonTop);
    }
}

void PrefsWindow::TearDownGroup()
{
    int i;

    for (i = 0; i < mRowCount; i++) {
        if (mRows[i].te != 0) {
            TEDeactivate(mRows[i].te);
            TEDispose(mRows[i].te);
        }
        if (mRows[i].control != 0) DisposeControl(mRows[i].control);
        /* The control is gone; its menu is ours to take out of the list. */
        if (mRows[i].menuID != 0) {
            DeleteMenu(mRows[i].menuID);
            DisposeMenu(GetMenuHandle(mRows[i].menuID));
        }
    }
    std::memset(mRows, 0, sizeof(mRows));
    mRowCount = 0;
    mFocus = -1;
}

/*
 * Take what is on screen back into the shadow copy.
 *
 * Called before the group changes and before a save, because a control holds
 * the only copy of anything typed into it since it was built.
 */
void PrefsWindow::ReadGroupBack()
{
    int i;

    for (i = 0; i < mRowCount; i++) {
        Row *r = &mRows[i];

        if (r->field == 0) continue;

        switch (r->field->kind) {
        case kGWFieldFlag:
            if (r->control != 0)
                std::strcpy(mValue[r->index],
                            GetControlValue(r->control) != 0 ? "1" : "0");
            break;

        case kGWFieldChoice: {
            short item = (r->control != 0) ? GetControlValue(r->control) : 0;

            if (item >= 1 && item <= r->altCount) {
                MenuHandle menu = GetMenuHandle(r->menuID);

                if (menu != 0) {
                    Str255 title;

                    GetMenuItemText(menu, item, title);
                    gw_copy_n(mValue[r->index], kValueMax,
                              reinterpret_cast<const char *>(title + 1),
                              title[0]);
                }
            }
            break;
        }

        case kGWFieldList:
        case kGWFieldText:
        case kGWFieldSecret:
        case kGWFieldNumber:
        default:
            if (r->te != 0) {
                CharsHandle h = TEGetText(r->te);
                long        n = (*r->te)->teLength;
                char       *dst = (r->field->kind == kGWFieldList)
                                      ? mList : mValue[r->index];
                long        cap = (r->field->kind == kGWFieldList)
                                      ? kListMax : kValueMax;

                if (n > cap - 1) n = cap - 1;
                if (h != 0 && n > 0) std::memcpy(dst, *h, n);
                dst[n] = '\0';
            }
            break;
        }
    }
}

void PrefsWindow::BuildGroup(int group)
{
    int                count = 0;
    const GWPrefField *f = gw_prefsform_fields(&count);
    int                i;
    short              v;

    TearDownGroup();
    mGroup = group;
    LayoutWindow(group);
    v = static_cast<short>(kBoxTop + 10);
    if (count > kMaxFields) count = kMaxFields;

    for (i = 0; i < count && mRowCount < kMaxRows; i++) {
        Row  *r;
        Rect  box;
        Str255 title;

        if (f[i].group != group) continue;

        r = &mRows[mRowCount];
        r->field = &f[i];
        r->index = i;
        r->labelV = static_cast<short>(v + 13);

        switch (f[i].kind) {
        case kGWFieldFlag:
            SetRect(&box, kPaneLeft, v, kPaneRight,
                    static_cast<short>(v + 16));
            ToPascal(f[i].label, title);
            r->control = NewControl(mWindow, &box, title, true,
                                    (mValue[i][0] == '1') ? 1 : 0,
                                    0, 1, checkBoxProc, kRefCheck);
            break;

        case kGWFieldChoice: {
            const char *p = f[i].choices;
            MenuHandle  menu;
            short       id = static_cast<short>(kChoiceMenuBase + mRowCount);
            short       chosen = 1;

            /*
             * A menu built here rather than from a resource, for the same
             * reason the fields are a table: the choices belong to the field,
             * and a MENU resource per choice field would be a second list to
             * keep in step with the first.
             */
            ToPascal(f[i].label, title);
            menu = NewMenu(id, title);
            if (menu != 0) {
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
                InsertMenu(menu, -1);       /* -1: a popup, not the menu bar */
                r->menuID = id;

                SetRect(&box, kEntryLeft, v,
                        static_cast<short>(kEntryLeft + kFieldWidth),
                        static_cast<short>(v + 18));
                ToPascal("", title);
                r->control = NewControl(mWindow, &box, title, true,
                                        chosen, id, 0,
                                        kPopupMenuProc, kRefChoice);
            }
            break;
        }

        case kGWFieldList: {
            Rect dest;

            SetRect(&box, kPaneLeft, static_cast<short>(v + 13),
                    kPaneRight,
                    static_cast<short>(v + 13 + kListHeight));
            r->hit = box;
            InsetRect(&box, 3, 3);
            dest = box;
            r->te = TENew(&dest, &box);
            if (r->te != 0) {
                (*r->te)->crOnly = -1;      /* one host per line, no wrapping */
                TESetText(mList, static_cast<long>(std::strlen(mList)), r->te);
            }
            break;
        }

        case kGWFieldText:
        case kGWFieldSecret:
        case kGWFieldNumber:
        default: {
            Rect dest;

            SetRect(&box, kEntryLeft, v, kPaneRight,
                    static_cast<short>(v + 16));
            r->hit = box;
            InsetRect(&box, 3, 2);
            dest = box;
            r->te = TENew(&dest, &box);
            if (r->te != 0)
                TESetText(mValue[i],
                          static_cast<long>(std::strlen(mValue[i])), r->te);
            break;
        }
        }

        if (f[i].kind == kGWFieldList)
            v = static_cast<short>(v + 13 + kListHeight + 3);
        else
            v = static_cast<short>(v + kRowHeight);

        if (f[i].hint != 0) {
            r->hintV = static_cast<short>(v + 9);
            v = static_cast<short>(v + kHintHeight);
        }
        v = static_cast<short>(v + 2);
        mRowCount++;
    }
}

/* ------------------------------------------------------------------ */

void PrefsWindow::DrawPane()
{
    int i;

    for (i = 0; i < mRowCount; i++) {
        Row *r = &mRows[i];
        Str255 s;

        if (r->field == 0) continue;

        TextFont(kFontGeneva);
        TextSize(9);

        /* A checkbox carries its own name; everything else needs one. */
        if (r->field->kind != kGWFieldFlag) {
            TextFace(normal);
            MoveTo(kPaneLeft, r->labelV);
            ToPascal(r->field->label, s);
            DrawString(s);
        }

        if (r->te != 0) {
            RGBColor white, black2;

            /* White inside, grey outside: TextEdit erases its view with the
             * background colour, so the field has to own it while it draws. */
            white.red = white.green = white.blue = 0xFFFF;
            black2.red = black2.green = black2.blue = 0;
            RGBBackColor(&white);
            EraseRect(&r->hit);
            RGBForeColor(&black2);
            FrameRect(&r->hit);
            TEUpdate(&r->hit, r->te);
            {
                RGBColor platinum2;

                platinum2.red = platinum2.green = platinum2.blue = kPlatinum;
                RGBBackColor(&platinum2);
            }
        }

        if (r->field->hint != 0 && r->hintV != 0) {
            TextFace(italic);
            MoveTo(kPaneLeft, r->hintV);
            ToPascal(r->field->hint, s);
            DrawString(s);
            TextFace(normal);
        }

        /*
         * A field whose value waits for a restart says so where it is, not in
         * a paragraph at the bottom that describes six of them at once.
         */
        if (r->field->needs_restart && r->field->kind == kGWFieldFlag) {
            TextFace(italic);
            MoveTo(static_cast<short>(kPaneRight - 74), r->labelV);
            ToPascal("on restart", s);
            DrawString(s);
            TextFace(normal);
        }
    }
    TextFont(0);
    TextSize(0);
}

void PrefsWindow::Draw()
{
    GrafPtr  port;
    Rect     box;
    Str255   s255;
    RGBColor platinum, black;

    if (mWindow == 0) return;
    port = reinterpret_cast<GrafPtr>(mWindow);
    SetPort(port);

    /*
     * Platinum, not white. Mac OS 9 paints a dialog 0xDD grey and a window
     * that stays white reads as an application that did not know, which the
     * About box already worked out -- kPlatinum is its number.
     */
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

    /* The label in front of the group popup, as "Connect via:" is. */
    MoveTo(kBoxLeft, static_cast<short>(kPopupTop + 14));
    ToPascal("Settings for:", s255);
    DrawString(s255);

    /*
     * The box around the page. TCP/IP frames its "Setup" group and writes the
     * name into the top rule; the gap is painted back in so the frame breaks
     * around the text rather than running under it.
     */
    SetRect(&box, kBoxLeft, kBoxTop, kBoxRight, mBoxBottom);
    FrameRect(&box);
    {
        const char *name = gw_prefsform_group_name(mGroup);
        short       w;

        ToPascal(name, s255);
        w = StringWidth(s255);
        SetRect(&box, static_cast<short>(kBoxLeft + 8), kBoxTop,
                static_cast<short>(kBoxLeft + 14 + w),
                static_cast<short>(kBoxTop + 1));
        RGBForeColor(&platinum);
        PaintRect(&box);
        RGBForeColor(&black);
        MoveTo(static_cast<short>(kBoxLeft + 11),
               static_cast<short>(kBoxTop + 4));
        DrawString(s255);
    }

    DrawPane();

    if (mNote[0] != '\0') {
        TextFont(kFontGeneva);
        TextSize(9);
        MoveTo(kBoxLeft, static_cast<short>(mButtonTop + 14));
        ToPascal(mNote, s255);
        DrawString(s255);
    }

    TextFont(0);
    TextSize(0);
    UpdateControls(mWindow, port->visRgn);
}

/* ------------------------------------------------------------------ */

bool PrefsWindow::Open()
{
    Rect    bounds;
    Str255  title;
    short   left, top;
    int     g;

    if (mWindow != 0) {
        SelectWindow(mWindow);
        return true;
    }

    left = static_cast<short>((qd.screenBits.bounds.right -
                               qd.screenBits.bounds.left - kWinWidth) / 2);
    top = static_cast<short>((qd.screenBits.bounds.bottom -
                              qd.screenBits.bounds.top - mWinHeight) / 3);
    SetRect(&bounds, left, top, static_cast<short>(left + kWinWidth),
            static_cast<short>(top + mWinHeight));

    ToPascal("Gateway Preferences", title);
    /*
     * NewCWindow, not NewWindow. A classic GrafPort is monochrome and
     * RGBForeColor on one does nothing at all, so the Platinum ground was
     * painted and discarded and the window stayed white. The About box in
     * main.cpp has always used the colour call, which is why its grey works
     * and this one's did not. The Appearance control definitions want a
     * colour port too.
     */
    mWindow = NewCWindow(0, &bounds, title, true, noGrowDocProc,
                         reinterpret_cast<WindowPtr>(-1), true, 0);
    if (mWindow == 0) return false;

    SetPort(reinterpret_cast<GrafPtr>(mWindow));
    TextFont(kFontGeneva);
    TextSize(9);

    LoadValues();
    std::strcpy(mNote, "");

    /* One popup for the seven pages, built here for the same reason the
     * choice fields build theirs: the groups are the table's, not a
     * resource's. */
    {
        MenuHandle menu;
        Rect       box;

        ToPascal("Settings", title);
        menu = NewMenu(kGroupMenuID, title);
        if (menu != 0) {
            for (g = 0; g < kGWGroupCount; g++) {
                ToPascal(gw_prefsform_group_name(g), title);
                AppendMenu(menu, title);
            }
            InsertMenu(menu, -1);
        }
        SetRect(&box, 74, kPopupTop, 250,
                static_cast<short>(kPopupTop + kPopupHeight));
        ToPascal("", title);
        mGroupPopup = NewControl(mWindow, &box, title, true, 1,
                                 kGroupMenuID, 0, kPopupMenuProc, kRefGroup);
    }

    {
        Rect box;

        SetRect(&box, static_cast<short>(kBoxRight - 74), 0,
                kBoxRight, kButtonHeight);
        ToPascal("Save", title);
        mSave = NewControl(mWindow, &box, title, true, 0, 0, 1,
                           pushButProc, kRefSave);

        SetRect(&box, static_cast<short>(kBoxRight - 156), 0,
                static_cast<short>(kBoxRight - 82), kButtonHeight);
        ToPascal("Revert", title);
        mRevert = NewControl(mWindow, &box, title, true, 0, 0, 1,
                             pushButProc, kRefRevert);
    }

    BuildGroup(0);
    Draw();
    return true;
}

void PrefsWindow::Close()
{
    if (mWindow == 0) return;
    TearDownGroup();
    DisposeWindow(mWindow);         /* takes the remaining controls with it */
    mWindow = 0;
    DeleteMenu(kGroupMenuID);
    DisposeMenu(GetMenuHandle(kGroupMenuID));
    mGroupPopup = 0;
    mSave = 0;
    mRevert = 0;
}

/* ------------------------------------------------------------------ */

void PrefsWindow::SetFocus(int row)
{
    if (mFocus == row) return;
    if (mFocus >= 0 && mFocus < mRowCount && mRows[mFocus].te != 0)
        TEDeactivate(mRows[mFocus].te);
    mFocus = row;
    if (mFocus >= 0 && mFocus < mRowCount && mRows[mFocus].te != 0)
        TEActivate(mRows[mFocus].te);
}

bool PrefsWindow::HandleClick(Point where)
{
    ControlHandle ctl;
    short         part;
    int           i;

    /*
     * GlobalToLocal and FindControl both work in the current port, and the
     * current port is whatever drew last -- which on a busy proxy is the log
     * window, redrawn every time a line arrives. Converting a click against
     * another window's origin puts it somewhere off the controls entirely.
     */
    SetPort(reinterpret_cast<GrafPtr>(mWindow));
    GlobalToLocal(&where);

    part = FindControl(where, mWindow, &ctl);
    if (part != 0 && ctl != 0) {
        long ref = (**ctl).contrlRfCon;

        if (TrackControl(ctl, where, 0) == 0) return true;

        if (ref == kRefGroup) {
            short g;

            /* TrackControl has already moved the popup to what was chosen. */
            g = static_cast<short>(GetControlValue(ctl) - 1);
            if (g >= 0 && g < kGWGroupCount && g != mGroup) {
                ReadGroupBack();
                BuildGroup(g);
                InvalRect(&reinterpret_cast<GrafPtr>(mWindow)->portRect);
            }
            return true;
        }

        if (ref == kRefCheck) {
            SetControlValue(ctl, GetControlValue(ctl) != 0 ? 0 : 1);
            mDirty = true;
            return true;
        }

        if (ref == kRefChoice) {
            /* The popup holds the answer; ReadGroupBack reads it back. */
            mDirty = true;
            return true;
        }

        if (ref == kRefSave)   { Save();   return true; }
        if (ref == kRefRevert) { Revert(); return true; }
        return true;
    }

    for (i = 0; i < mRowCount; i++) {
        if (mRows[i].te == 0) continue;
        if (PtInRect(where, &mRows[i].hit)) {
            SetFocus(i);
            /*
             * A secret still showing its placeholder is replaced rather than
             * edited: selecting into the middle of eight bullets and typing
             * would produce a value that is neither the old one nor a new one.
             */
            if (mRows[i].field->kind == kGWFieldSecret &&
                std::strcmp(mValue[mRows[i].index], kKeptSecret) == 0) {
                TESetText("", 0, mRows[i].te);
                mValue[mRows[i].index][0] = '\0';
                InvalRect(&mRows[i].hit);
            }
            TEClick(where, 0, mRows[i].te);
            return true;
        }
    }
    return false;
}

void PrefsWindow::HandleKey(char ch)
{
    if (ch == '\t') {
        int start = (mFocus < 0) ? -1 : mFocus;
        int i;

        for (i = 1; i <= mRowCount; i++) {
            int n = (start + i) % mRowCount;

            if (mRows[n].te != 0) { SetFocus(n); break; }
        }
        return;
    }

    if (mFocus < 0 || mFocus >= mRowCount || mRows[mFocus].te == 0) return;

    /* A single-line field takes no Return; the list field is made of them. */
    if ((ch == '\r' || ch == '\n') &&
        mRows[mFocus].field->kind != kGWFieldList) {
        Save();
        return;
    }

    TEKey(ch, mRows[mFocus].te);
    mDirty = true;
}

bool PrefsWindow::HandleEvent(EventRecord &event)
{
    if (mWindow == 0) return false;

    switch (event.what) {
    case mouseDown: {
        WindowPtr win;
        short     part = FindWindow(event.where, &win);

        if (win != mWindow) return false;
        switch (part) {
        case inGoAway:
            if (TrackGoAway(mWindow, event.where)) Close();
            return true;
        case inDrag: {
            Rect limit = qd.screenBits.bounds;

            InsetRect(&limit, 4, 4);
            DragWindow(mWindow, event.where, &limit);
            return true;
        }
        case inContent:
            if (FrontWindow() != mWindow) {
                SelectWindow(mWindow);
                return true;
            }
            return HandleClick(event.where);
        default:
            return false;
        }
    }

    case keyDown:
    case autoKey:
        if (FrontWindow() != mWindow) return false;
        if (event.modifiers & cmdKey) return false;   /* the menu bar's */
        HandleKey(static_cast<char>(event.message & charCodeMask));
        return true;

    case updateEvt:
        if (reinterpret_cast<WindowPtr>(event.message) != mWindow)
            return false;
        BeginUpdate(mWindow);
        Draw();
        EndUpdate(mWindow);
        return true;

    case activateEvt:
        if (reinterpret_cast<WindowPtr>(event.message) != mWindow)
            return false;
        return true;

    default:
        return false;
    }
}

void PrefsWindow::Idle()
{
    if (mWindow == 0) return;
    SetPort(reinterpret_cast<GrafPtr>(mWindow));
    if (mFocus >= 0 && mFocus < mRowCount && mRows[mFocus].te != 0 &&
        FrontWindow() == mWindow)
        TEIdle(mRows[mFocus].te);
}

void PrefsWindow::EditCommand(int cmd)
{
    TEHandle te;

    if (!CanEdit()) return;
    te = mRows[mFocus].te;

    switch (cmd) {
    case kGWEditCut:
        TECut(te);
        ZeroScrap();
        TEToScrap();
        mDirty = true;
        break;
    case kGWEditCopy:
        TECopy(te);
        ZeroScrap();
        TEToScrap();
        break;
    case kGWEditPaste:
        TEFromScrap();
        TEPaste(te);
        mDirty = true;
        break;
    case kGWEditClear:
        TEDelete(te);
        mDirty = true;
        break;
    case kGWEditSelectAll:
        TESetSelect(0, (*te)->teLength, te);
        break;
    default:
        break;
    }
    InvalRect(&mRows[mFocus].hit);
}

/* ------------------------------------------------------------------ */

void PrefsWindow::Revert()
{
    LoadValues();
    BuildGroup(mGroup);
    Note("Reverted to what is in the file.");
    InvalRect(&reinterpret_cast<GrafPtr>(mWindow)->portRect);
}

/*
 * Write what changed, and only what changed.
 *
 * Every untouched line keeps its comment, its spacing and the key spelling
 * whoever typed it used, because gw_prefs_set() rewrites one line and copies
 * the rest. A form that wrote all forty-one keys would reformat the file on
 * every save and lose the notes people leave in it.
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
            char        store[kListMax];
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
            if (!GWConfig_SetList(f[i].key, vals, n)) failed++;
            else written++;
            continue;
        }

        /* A secret nobody retyped keeps whatever is on file. */
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

    if (failed > 0) {
        std::sprintf(mNote, "%d saved, %d refused -- see the log.",
                     written, failed);
    } else if (restart > 0) {
        std::sprintf(mNote, "Saved. %d of them take effect on restart.",
                     restart);
    } else if (written > 0) {
        std::strcpy(mNote, "Saved.");
    } else {
        std::strcpy(mNote, "Nothing had changed.");
    }
    gw_log("preferences: %s", mNote);

    mDirty = false;
    LoadValues();
    BuildGroup(mGroup);
    InvalRect(&reinterpret_cast<GrafPtr>(mWindow)->portRect);
}

}  /* namespace */

/* ------------------------------------------------------------------ */
/* The plain-C face main.cpp uses.                                     */
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
