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

/*
 * The readable log (0.3.7). A line a person reads is a plain sentence with a
 * short stable code after it, so a screenshot still says exactly which branch
 * wrote it:
 *
 *     gw_logc("H12", "#%ld the browser gave up after seeing our certificate",
 *             id);                 ->  "#2 the browser gave up ... (H12)"
 *
 * Codes are one letter for the module and two digits, listed once in
 * docs/log-codes.md and never reused. When a sentence is too long for a line
 * it is the sentence that is cut, never the code.
 *
 * The engineer's detail -- hello bytes, suite numbers, byte counts, library
 * error numbers -- goes through gw_logd(), which writes nothing unless the
 * log_debug preference is on. Its lines are indented under the sentence they
 * explain. log_debug is read at launch like every other preference; it is a
 * runtime switch, not a build flag, because builds come from CI and a user
 * asked to reproduce something must not need a special one.
 *
 * gw_log() stays for lines that are already readable as they are, such as
 * the request line.
 */
void gw_logc(const char *code, const char *fmt, ...);
void gw_logd(const char *fmt, ...);

void gw_log_set_debug(int on);
int  gw_log_debug(void);

/* Number of lines currently held (<= GW_LOG_LINES). */
int gw_log_count(void);

/* Line idx, 0 being the oldest still held. NULL when out of range. */
const char *gw_log_line(int idx);

/* Bumped on every append so the UI knows when to redraw. */
long gw_log_generation(void);

/*
 * Called with every line as it is logged, for a platform that wants to keep a
 * copy on disk. NULL to stop. The window's ring buffer is unaffected either
 * way, so a sink that fails costs nothing else.
 */
typedef void (*GWLogSink)(const char *line);
void gw_log_set_sink(GWLogSink sink);

#ifdef __cplusplus
}
#endif

#endif /* GW_LOG_H */
