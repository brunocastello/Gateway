#include "gw_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * Retro68's PPC crt0 does not run C++ global constructors (CLAUDE.md rule 2),
 * so nothing here may depend on one. Plain C file-scope statics are zero
 * initialised by the loader and are fine.
 */
static char sLines[GW_LOG_LINES][GW_LOG_WIDTH];
static int  sHead;      /* index of the next slot to write */
static int  sCount;
static long sGeneration;

/*
 * Optional second destination for every line, so a whole session can be handed
 * to someone rather than read 200 lines at a time off a screen.
 *
 * It is a callback rather than a FILE * because this file is portable and host
 * tested, while on Mac OS 9 the log has to be written through the File Manager
 * like the preferences are. The platform supplies the sink; see gw_core.c.
 */
static GWLogSink sSink;
static int       sDebug;      /* the log_debug preference */

void gw_log_reset(void)
{
    sHead = 0;
    sCount = 0;
    sGeneration++;
}

/* The slot about to be written, and committing it once it holds a line. */
static char *next_slot(void)
{
    return sLines[sHead];
}

static void commit_slot(char *slot)
{
    slot[GW_LOG_WIDTH - 1] = '\0';

    sHead = (sHead + 1) % GW_LOG_LINES;
    if (sCount < GW_LOG_LINES) sCount++;
    sGeneration++;

    if (sSink != NULL) sSink(slot);
}

void gw_log(const char *fmt, ...)
{
    va_list ap;
    char *slot = next_slot();

    va_start(ap, fmt);
    vsnprintf(slot, GW_LOG_WIDTH, fmt, ap);
    va_end(ap);
    commit_slot(slot);
}

void gw_logc(const char *code, const char *fmt, ...)
{
    va_list ap;
    char   *slot = next_slot();
    size_t  tail, room, len;

    if (code == NULL) code = "";
    /*
     * " (" + code + ")" is reserved first, so that however long the
     * sentence, the code survives: it is the part a screenshot needs.
     */
    tail = strlen(code) + 3;
    if (tail >= GW_LOG_WIDTH) tail = 0;         /* absurd code: drop it */
    room = GW_LOG_WIDTH - tail;

    va_start(ap, fmt);
    vsnprintf(slot, room, fmt, ap);
    va_end(ap);

    if (tail > 0) {
        len = strlen(slot);
        snprintf(slot + len, GW_LOG_WIDTH - len, " (%s)", code);
    }
    commit_slot(slot);
}

void gw_logd(const char *fmt, ...)
{
    va_list ap;
    char   *slot;

    if (!sDebug) return;

    slot = next_slot();
    slot[0] = ' ';
    slot[1] = ' ';
    va_start(ap, fmt);
    vsnprintf(slot + 2, GW_LOG_WIDTH - 2, fmt, ap);
    va_end(ap);
    commit_slot(slot);
}

void gw_log_set_debug(int on)
{
    sDebug = on != 0;
}

int gw_log_debug(void)
{
    return sDebug;
}

void gw_log_set_sink(GWLogSink sink)
{
    sSink = sink;
}

int gw_log_count(void)
{
    return sCount;
}

const char *gw_log_line(int idx)
{
    int start;

    if (idx < 0 || idx >= sCount) return NULL;
    start = (sHead - sCount + GW_LOG_LINES) % GW_LOG_LINES;
    return sLines[(start + idx) % GW_LOG_LINES];
}

long gw_log_generation(void)
{
    return sGeneration;
}
