/*
 * gw_prefsform.h - what the Preferences window shows, declared once.
 *
 * Mac OS 9 and Windows draw their own controls with their own toolkits, but
 * both read this table for what the controls are, what they are called, what
 * they may hold and where they belong. A settings window that exists twice is
 * a settings window that disagrees with itself the first time a preference is
 * added, and the two are built by different people on different days.
 *
 * Not every preference is here, by design. The form carries what a person
 * changes; what is left in the file is either bootstrapped out of band
 * (refresh_token, the OAuth client secret), written by Gateway itself
 * (wayback_date from the settings page), or a list rather than a field
 * (wayback_live, which has its own control).
 *
 * CLAUDE.md rule 8: portable, so the table and its validation are exercised by
 * the host tests rather than by clicking the window on two operating systems.
 */
#ifndef GW_PREFSFORM_H
#define GW_PREFSFORM_H

#include <stddef.h>

typedef enum {
    kGWFieldFlag = 0,   /* checkbox; value is "1" or "0"                   */
    kGWFieldNumber,     /* numeric entry, clamped to [min,max]             */
    kGWFieldText,       /* free text                                       */
    kGWFieldSecret,     /* free text, shown as bullets                     */
    kGWFieldChoice      /* one of `choices`, a '|'-separated list          */
} GWFieldKind;

/*
 * The groups, in the order they appear. A platform draws them as boxes, tabs
 * or headings as suits it; what it may not do is reorder them, because the
 * order is the argument the window makes -- what Gateway is, then what each
 * module does.
 */
typedef enum {
    kGWGroupModules = 0,
    kGWGroupWeb,
    kGWGroupArchive,
    kGWGroupMail,
    kGWGroupLog,
    kGWGroupCount
} GWFieldGroup;

const char *gw_prefsform_group_name(int group);

typedef struct {
    const char  *key;        /* the preferences key it reads and writes      */
    const char  *label;      /* what the control is called on screen         */
    GWFieldKind  kind;
    GWFieldGroup group;
    const char  *def;        /* the same default the reader applies          */
    long         min, max;   /* kGWFieldNumber only                          */
    const char  *choices;    /* kGWFieldChoice only: "auto|always|never"     */
    /*
     * 1 when changing it does nothing until Gateway is restarted -- a port
     * that is already bound, a module that is already running or not. The
     * window says so rather than leaving someone to wonder why the number
     * they typed had no effect.
     */
    int          needs_restart;
    const char  *hint;       /* one line under or beside the control, or NULL */
} GWPrefField;

/* The table, and how many entries it has. */
const GWPrefField *gw_prefsform_fields(int *count);

/* The field for a key, or NULL. */
const GWPrefField *gw_prefsform_find(const char *key);

/*
 * Check a value a person typed, before it is written anywhere.
 *
 * Returns 1 when it is acceptable and copies the value Gateway should store
 * into `out` -- which is not always what was typed: a number is clamped to the
 * field's range, a flag becomes "1" or "0", and a choice is matched
 * case-insensitively and stored in the table's spelling. Returns 0 when the
 * value cannot be used at all, with `why` set to a sentence to show the
 * person, so the window can refuse a save rather than write nonsense and
 * leave them to find out from the log.
 */
int gw_prefsform_validate(const GWPrefField *f, const char *value,
                          char *out, size_t cap,
                          const char **why);

/*
 * 1 when `value` differs from what the file already holds, so a save writes
 * only what changed -- which keeps the file's comments, spacing and key
 * spelling intact for every line nobody touched.
 */
int gw_prefsform_changed(const char *current, const char *value);

#endif /* GW_PREFSFORM_H */
