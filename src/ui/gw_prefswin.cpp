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
#include <Multiverse.h>          /* the Control Manager; see main.cpp */
#include <Quickdraw.h>
#include <Scrap.h>
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

const short kWinWidth   = 476;
const short kWinHeight  = 344;

const short kSideWidth  = 116;      /* the group column                     */
const short kPaneLeft   = kSideWidth + 10;
const short kPaneTop    = 10;
const short kPaneRight  = kWinWidth - 12;

const short kRowHeight  = 20;       /* a field with no hint                 */
const short kHintHeight = 13;
const short kFieldWidth = 168;
const short kEntryLeft  = kPaneRight - kFieldWidth;

const short kButtonBottom = kWinHeight - 12;
const short kButtonTop    = kButtonBottom - 20;

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
    ControlHandle      alt[4];     /* choice fields: one control per choice  */
    int                altCount;
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
        std::memset(mGroupCtl, 0, sizeof(mGroupCtl));
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

    WindowPtr     mWindow;
    int           mGroup;
    Row           mRows[kMaxRows];
    int           mRowCount;
    int           mFocus;
    bool          mDirty;
    ControlHandle mGroupCtl[kGWGroupCount];
    ControlHandle mSave;
    ControlHandle mRevert;

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
        SetRect(&r, 10, static_cast<short>(kButtonTop - 2),
                static_cast<short>(kPaneRight - 150), kButtonBottom);
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

void PrefsWindow::TearDownGroup()
{
    int i;

    for (i = 0; i < mRowCount; i++) {
        int k;

        if (mRows[i].te != 0) {
            TEDeactivate(mRows[i].te);
            TEDispose(mRows[i].te);
        }
        if (mRows[i].control != 0) DisposeControl(mRows[i].control);
        for (k = 0; k < mRows[i].altCount; k++)
            if (mRows[i].alt[k] != 0) DisposeControl(mRows[i].alt[k]);
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
            int k;

            for (k = 0; k < r->altCount; k++) {
                if (r->alt[k] != 0 && GetControlValue(r->alt[k]) != 0) {
                    Str255 title;

                    GetControlTitle(r->alt[k], title);
                    gw_copy_n(mValue[r->index], kValueMax,
                              reinterpret_cast<const char *>(title + 1),
                              title[0]);
                    break;
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
    short              v = kPaneTop;

    TearDownGroup();
    mGroup = group;
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
            short       x = kEntryLeft;

            while (p != 0 && *p != '\0' && r->altCount < 4) {
                const char *end = std::strchr(p, '|');
                size_t      n = (end != 0) ? static_cast<size_t>(end - p)
                                           : std::strlen(p);
                char        buf[32];
                short       w;

                if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
                std::memcpy(buf, p, n);
                buf[n] = '\0';
                w = static_cast<short>(20 + TextWidth(buf, 0,
                                                 static_cast<short>(n)));
                SetRect(&box, x, v, static_cast<short>(x + w),
                        static_cast<short>(v + 16));
                ToPascal(buf, title);
                r->alt[r->altCount] =
                    NewControl(mWindow, &box, title, true,
                               (gw_stricmp(buf, mValue[i]) == 0) ? 1 : 0,
                               0, 1, radioButProc, kRefChoice);
                r->altCount++;
                x = static_cast<short>(x + w + 4);
                p = (end != 0) ? end + 1 : 0;
            }
            break;
        }

        case kGWFieldList: {
            Rect dest;

            SetRect(&box, kPaneLeft, static_cast<short>(v + 14),
                    kPaneRight, static_cast<short>(v + 14 + 76));
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
            v = static_cast<short>(v + 14 + 76 + 4);
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
            FrameRect(&r->hit);
            TEUpdate(&r->hit, r->te);
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
    Rect    r;
    Str255  s;
    GrafPtr save;

    if (mWindow == 0) return;
    GetPort(&save);
    SetPort(reinterpret_cast<GrafPtr>(mWindow));

    SetRect(&r, 0, 0, kWinWidth, kWinHeight);
    EraseRect(&r);

    /* The group column, with a rule between it and the pane. */
    MoveTo(static_cast<short>(kSideWidth), 8);
    LineTo(static_cast<short>(kSideWidth),
           static_cast<short>(kButtonTop - 10));

    MoveTo(10, static_cast<short>(kButtonTop - 10));
    LineTo(static_cast<short>(kWinWidth - 10),
           static_cast<short>(kButtonTop - 10));

    DrawPane();

    if (mNote[0] != '\0') {
        TextFont(kFontGeneva);
        TextSize(9);
        MoveTo(12, static_cast<short>(kButtonTop + 14));
        ToPascal(mNote, s);
        DrawString(s);
        TextFont(0);
        TextSize(0);
    }

    UpdateControls(mWindow, reinterpret_cast<GrafPtr>(mWindow)->visRgn);
    SetPort(save);
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
                              qd.screenBits.bounds.top - kWinHeight) / 3);
    SetRect(&bounds, left, top, static_cast<short>(left + kWinWidth),
            static_cast<short>(top + kWinHeight));

    ToPascal("Gateway Preferences", title);
    mWindow = NewWindow(0, &bounds, title, true, noGrowDocProc,
                        reinterpret_cast<WindowPtr>(-1), true, 0);
    if (mWindow == 0) return false;

    SetPort(reinterpret_cast<GrafPtr>(mWindow));
    TextFont(kFontGeneva);
    TextSize(9);

    LoadValues();
    std::strcpy(mNote, "");

    for (g = 0; g < kGWGroupCount; g++) {
        Rect box;

        SetRect(&box, 10, static_cast<short>(kPaneTop + g * 18),
                static_cast<short>(kSideWidth - 8),
                static_cast<short>(kPaneTop + g * 18 + 16));
        ToPascal(gw_prefsform_group_name(g), title);
        mGroupCtl[g] = NewControl(mWindow, &box, title, true,
                                  (g == 0) ? 1 : 0, 0, 1,
                                  radioButProc, kRefGroup);
    }

    {
        Rect box;

        SetRect(&box, static_cast<short>(kPaneRight - 74), kButtonTop,
                kPaneRight, kButtonBottom);
        ToPascal("Save", title);
        mSave = NewControl(mWindow, &box, title, true, 0, 0, 1,
                           pushButProc, kRefSave);

        SetRect(&box, static_cast<short>(kPaneRight - 156), kButtonTop,
                static_cast<short>(kPaneRight - 82), kButtonBottom);
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
    std::memset(mGroupCtl, 0, sizeof(mGroupCtl));
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

    GlobalToLocal(&where);

    part = FindControl(where, mWindow, &ctl);
    if (part != 0 && ctl != 0) {
        long ref = (**ctl).contrlRfCon;

        if (TrackControl(ctl, where, 0) == 0) return true;

        if (ref == kRefGroup) {
            int g;

            ReadGroupBack();
            for (g = 0; g < kGWGroupCount; g++)
                if (mGroupCtl[g] != 0)
                    SetControlValue(mGroupCtl[g], (mGroupCtl[g] == ctl) ? 1 : 0);
            for (g = 0; g < kGWGroupCount; g++)
                if (mGroupCtl[g] == ctl) BuildGroup(g);
            InvalRect(&reinterpret_cast<GrafPtr>(mWindow)->portRect);
            return true;
        }

        if (ref == kRefCheck) {
            SetControlValue(ctl, GetControlValue(ctl) != 0 ? 0 : 1);
            mDirty = true;
            return true;
        }

        if (ref == kRefChoice) {
            /* One of a set: find the row it belongs to and clear its siblings. */
            for (i = 0; i < mRowCount; i++) {
                int k, mine = 0;

                for (k = 0; k < mRows[i].altCount; k++)
                    if (mRows[i].alt[k] == ctl) mine = 1;
                if (!mine) continue;
                for (k = 0; k < mRows[i].altCount; k++)
                    SetControlValue(mRows[i].alt[k],
                                    (mRows[i].alt[k] == ctl) ? 1 : 0);
                break;
            }
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
