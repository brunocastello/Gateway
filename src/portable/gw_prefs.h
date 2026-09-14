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

/*
 * Replace every occurrence of a repeated key with the `count` values in
 * `values`, writing the whole file to out. The first occurrence keeps its
 * place in the file so a list stays where its comment is; the rest are
 * removed and the remaining values written after it. A count of 0 removes the
 * key. Returns bytes written, or 0 if it would not fit.
 *
 * gw_prefs_set() cannot do this: it replaces the first match and leaves the
 * others, which for a list means writing one entry and silently keeping the
 * old rest.
 */
size_t gw_prefs_set_list(const char *text, size_t len, const char *key,
                         const char *const *values, int count,
                         char *out, size_t cap);

/* Same, but parses the value as a decimal number. Returns def when absent. */
long gw_prefs_get_num(const char *text, size_t len, const char *key, long def);

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

#ifdef __cplusplus
}
#endif

#endif /* GW_PREFS_H */
