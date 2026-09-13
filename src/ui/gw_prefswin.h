/*
 * gw_prefswin.h - the Mac OS 9 Preferences window, as main.cpp sees it.
 *
 * Modeless: the window is driven from the one WaitNextEvent loop rather than
 * running its own, so the proxy keeps working while it is open. main.cpp
 * offers it each event before handling the event itself, and calls
 * GWPrefsWin_Idle() once a pass for the caret.
 */
#ifndef GW_PREFSWIN_H
#define GW_PREFSWIN_H

#include <Events.h>
#include <Windows.h>

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

void      GWPrefsWin_Open(void);
void      GWPrefsWin_Close(void);
int       GWPrefsWin_IsOpen(void);
WindowPtr GWPrefsWin_Window(void);

/* 1 when the window took the event and main.cpp should not. */
int       GWPrefsWin_HandleEvent(EventRecord *event);

/* Blink the caret. Once per pass of the main loop. */
void      GWPrefsWin_Idle(void);

/*
 * 1 when an entry field has the focus, which is the only time Cut, Copy,
 * Paste and the rest mean anything. The Edit menu is enabled from this, so
 * its items are grey until the window is open with a field selected and grey
 * again the moment it closes.
 */
int       GWPrefsWin_CanEdit(void);
void      GWPrefsWin_EditCommand(int cmd);

#ifdef __cplusplus
}
#endif

#endif /* GW_PREFSWIN_H */
