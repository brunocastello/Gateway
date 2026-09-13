/*
 * gw_prefswin.h - the Mac OS 9 Preferences window, as main.cpp sees it.
 *
 * No Toolbox types cross this header. gw_prefswin.cpp is compiled against
 * Apple's Universal Interfaces so it can call the Appearance Manager -- the
 * same arrangement CLAUDE.md rule 3 uses for the Open Transport translation
 * units -- while main.cpp stays on Multiversal. The two must not meet inside
 * one file, so the event is passed as a pointer and cast on the other side.
 *
 * Modeless: the window is driven from the one WaitNextEvent loop rather than
 * running its own, so the proxy keeps working while it is open.
 */
#ifndef GW_PREFSWIN_H
#define GW_PREFSWIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Edit menu commands, in the order they appear in the menu. */
enum {
    kGWEditCut = 1,
    kGWEditCopy,
    kGWEditPaste,
    kGWEditClear,
    kGWEditSelectAll
};

void GWPrefsWin_Open(void);
void GWPrefsWin_Close(void);
int  GWPrefsWin_IsOpen(void);

/* `event` is an EventRecord *. 1 when the window took it. */
int  GWPrefsWin_HandleEvent(void *event);

/* Once per pass of the main loop, for the caret. */
void GWPrefsWin_Idle(void);

/*
 * 1 when an entry field has the keyboard focus, which is the only time Cut,
 * Copy, Paste and the rest mean anything. The Edit menu is enabled from this.
 */
int  GWPrefsWin_CanEdit(void);
void GWPrefsWin_EditCommand(int cmd);

#ifdef __cplusplus
}
#endif

#endif /* GW_PREFSWIN_H */
