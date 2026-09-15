/*
 * gw_prefs.h - "key = value" preference-text lookup.
 *
 * PORTABLE: no Mac or Windows headers. See gw_util.h. The Toolbox side reads
 * the prefs file off disk and hands the raw text in here, so the parsing rules
 * stay testable on the host.
 *
 * Format: one setting per line, '#' or ';' starts a comment, the separator is
 * '=' or ':', surrounding whitespace is trimmed. Keys are case-insensitive.
 */
#ifndef GW_PREFS_H
#define GW_PREFS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Copy the value for key into out. Returns 1 when the key was present with a
 * non-empty value, 0 otherwise (out is set to "" in that case).
 */
int gw_prefs_get(const char *text, size_t len, const char *key,
                 char *out, size_t cap);

/*
 * The nth occurrence of a key, counting from 0. Prefs are one setting per
 * line, but a list -- the Wayback allow-list, say -- reads far better as the
 * same key repeated than as one enormous space-separated value.
 *
 * Returns 1 when that occurrence exists, whatever its value, and 0 only when
 * there is no nth occurrence. The distinction is the whole point of the
 * function: a caller walking indices has to be able to tell a blank entry
 * from the end of the list, or one blank line in the prefs silently discards
 * every entry below it. Check out[0] for an empty value.
 */
int gw_prefs_get_nth(const char *text, size_t len, const char *key, int n,
                     char *out, size_t cap);

/* Same, but parses the value as a decimal number. Returns def when absent. */
long gw_prefs_get_num(const char *text, size_t len, const char *key, long def);

/*
 * The nth entry across all occurrences of a key, splitting each value on ';'.
 *
 * Walks every occurrence of the key (like gw_prefs_get_nth), but within each
 * one, splits the value on ';', trims spaces around entries, and skips empty
 * ones. Returns 1 when the nth entry exists (whatever its value), 0 only
 * when there is no nth entry.
 *
 * This handles both storage forms — repeated keys and one ;-separated value
 * — so a file mixing the two forms is read correctly.
 */
int gw_prefs_get_nth_split(const char *text, size_t len, const char *key,
                           int n, char *out, size_t cap);

/*
 * Produce a copy of the prefs text with key set to value, writing it into out.
 * An existing setting is rewritten where it stands, keeping its separator and
 * the file's line endings; a new one is appended. Comments, ordering and
 * spacing elsewhere are left alone, because this file is hand-edited and
 * Gateway only ever has business changing one line of it.
 *
 * Returns the length written, or 0 if it would not fit in cap.
 */
size_t gw_prefs_set(const char *text, size_t len, const char *key,
                    const char *value, char *out, size_t cap);

/* Normalize an editable host list in place: trim entries, skip empty ones,
 * and convert semicolons or CR/LF separators to a single semicolon. */
void gw_prefs_normalize_list(char *value);

#ifdef __cplusplus
}
#endif

#endif /* GW_PREFS_H */
