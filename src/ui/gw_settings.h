#ifndef GW_SETTINGS_H
#define GW_SETTINGS_H

/* No Toolbox types cross the Universal/Multiversal translation-unit boundary.
 * The callback services the owner's update/Apple events and returns quit state. */
void GWSettings_Run(int (*serviceEvent)(void *, void *), void *context);

/* Nonzero when the Appearance Manager the window is built from is installed.
 * On 68K its calls are traps, so running without it would crash, not fail. */
int GWSettings_Available(void);

#endif
