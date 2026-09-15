/*
 * settings_win32.h - the preferences window.
 *
 * The counterpart of src/ui/gw_settings.cpp, and the same shape: one field
 * table drives both the values and the geometry, and the window is only as
 * tall as the pane on show.
 */
#ifndef GW_SETTINGS_WIN32_H
#define GW_SETTINGS_WIN32_H

#include <windows.h>

/* Open the window, or bring it forward if it is already up. */
void GWSettings_Show(HINSTANCE inst);

/* The window, or NULL. The caller's message pump hands it IsDialogMessage so
 * Tab, Enter and Escape work as they would in a real dialog. */
HWND GWSettings_Window(void);

#endif /* GW_SETTINGS_WIN32_H */
