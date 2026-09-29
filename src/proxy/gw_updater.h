/*
 * gw_updater.h - check GitHub for a newer release, once per launch.
 *
 * Platform independent (gw_transport.h names no operating system). One
 * request, made a little after the listeners are up: an HTTP/1.0 GET of
 * github.com/brunocastello/Gateway/releases/latest over Gateway's own TLS
 * stack, driven from the cooperative loop the same way gw_token.c drives a
 * token refresh -- a small state machine on a GWStream, never a blocking
 * call. The redirect it gets back is read, never followed; the version
 * arithmetic that turns its Location header into a tag and an asset link
 * lives in src/portable/gw_update.c, so it is host-tested.
 */
#ifndef GW_UPDATER_H
#define GW_UPDATER_H

void GWUpdater_Init(void);
void GWUpdater_Shutdown(void);

/*
 * Start the one check for this run, unless check_updates is 0 (then no
 * network request is made at all) or a check has already started or
 * finished. Safe to call more than once -- GW_Start() does, on every
 * Stop/Start cycle from the Preferences window -- because after the first
 * call it is a no-op.
 */
void GWUpdater_Request(void);

/*
 * Tear down a check still in flight, e.g. because the Preferences window's
 * Stop button was pressed. Leaves the state at "done" rather than "not
 * started", so the check is not retried on a later Start -- it is still
 * once per launch, and an aborted one counts as the one. Quiet: nothing
 * goes in the plain log, since Stop is not a failure.
 */
void GWUpdater_Abort(void);

void GWUpdater_Poll(void);

#endif /* GW_UPDATER_H */
