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

/* Same, but parses the value as a decimal number. Returns def when absent. */
long gw_prefs_get_num(const char *text, size_t len, const char *key, long def);

#ifdef __cplusplus
}
#endif

#endif /* GW_PREFS_H */
