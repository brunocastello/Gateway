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

/*
 * What a manual check ("Check for Updates..." on the menu) can end with.
 * Kept separate from the log line GWUpdater_Request() writes: the menu
 * command wants an answer to put in a dialog, not just a line the launch
 * check is content to leave quiet.
 */
typedef enum {
    kGWManualNewer = 0,
    kGWManualCurrent,
    kGWManualFailed
} GWManualResultKind;

typedef struct {
    GWManualResultKind kind;
    char version[32];    /* Newer: the release tag. Current: the running
                           * version. Empty for Failed. */
    char url[256];       /* Newer: the platform asset link. Empty otherwise. */
    char reason[96];     /* Failed: why, in the debug line's own words.
                           * Empty otherwise. */
} GWManualResult;

/*
 * Run the check the menu command asked for. Starts one unless a check is
 * already in flight -- the launch check, or an earlier manual one -- in
 * which case nothing new is started and that check's result is reported
 * through GWUpdater_ManualResult() when it lands; two requests never run at
 * once. Unlike GWUpdater_Request(), this ignores check_updates and an
 * already-finished launch check, because the user asked outright.
 */
void GWUpdater_RequestManual(void);

/*
 * 1 once, with *out filled, the first time GWUpdater_Poll() notices that the
 * check a manual request asked for has finished. 0 otherwise, including on
 * every call after the one true answer -- the caller shows its dialog and
 * does not ask again.
 */
int GWUpdater_ManualResult(GWManualResult *out);

#endif /* GW_UPDATER_H */
