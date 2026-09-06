/*
 * gw_log.h - fixed-size log ring shared by the network core and the UI.
 *
 * PORTABLE: no Mac or Windows headers. Everything is statically sized: the
 * log must never allocate, because it is written from the same cooperative
 * slice that is trying to keep the proxy moving.
 */
#ifndef GW_LOG_H
#define GW_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Deep enough that a page load's worth of activity can be scrolled back
 * through afterwards; 200 lines costs 25 KB. */
#define GW_LOG_LINES 200
#define GW_LOG_WIDTH 128

void gw_log_reset(void);

/* Append one line. Long lines are truncated, never wrapped. */
void gw_log(const char *fmt, ...);

/* Number of lines currently held (<= GW_LOG_LINES). */
int gw_log_count(void);

/* Line idx, 0 being the oldest still held. NULL when out of range. */
const char *gw_log_line(int idx);

/* Bumped on every append so the UI knows when to redraw. */
long gw_log_generation(void);

/*
 * Also append every line to `path`, which is created if absent and appended to
 * across runs. Pass NULL or "" to stop. Returns 0 if the file could not be
 * opened, in which case logging to the window carries on regardless.
 */
int  gw_log_to_file(const char *path);
void gw_log_close_file(void);

#ifdef __cplusplus
}
#endif

#endif /* GW_LOG_H */
