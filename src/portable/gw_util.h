/*
 * gw_util.h - small string helpers shared by the portable modules.
 *
 * PORTABLE: this file and its implementation must never include
 * <OpenTransport.h>, <Windows.h> or any other Mac/Win system header, so the
 * host test suite (tests/host) compiles them with a plain Linux/macOS cc.
 */
#ifndef GW_UTIL_H
#define GW_UTIL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ASCII-only tolower; the Toolbox locale must not enter into protocol code. */
int gw_lower(int c);

/* Case-insensitive compare of NUL-terminated strings. */
int gw_stricmp(const char *a, const char *b);

/* Case-insensitive compare of at most n bytes. */
int gw_strnicmp(const char *a, const char *b, size_t n);

/* 1 when the first strlen(prefix) bytes of s (bounded by len) match prefix,
 * case-insensitively. */
int gw_starts_ci(const char *s, size_t len, const char *prefix);

/* Copy len bytes of src into a cap-sized dst and NUL-terminate.
 * Returns the number of bytes copied (never more than cap - 1). */
size_t gw_copy_n(char *dst, size_t cap, const char *src, size_t len);

/* Locate the end of an RFC 822 header block. On success stores the offset one
 * past the terminating CRLFCRLF (or LFLF) in *head_len and returns 1.
 * Returns 0 when the block is still incomplete. */
int gw_find_head_end(const char *buf, size_t len, size_t *head_len);

/* Find a header value inside a header block. head points at the start line;
 * the search skips it. Returns a pointer to the first byte of the trimmed
 * value and stores its length in *val_len, or NULL when absent. */
const char *gw_header_find(const char *head, size_t head_len,
                           const char *name, size_t *val_len);

/* Parse a non-negative decimal number from at most len bytes.
 * Returns -1 when no digits are present. */
long gw_parse_dec(const char *s, size_t len);

/* Offset of the next line in a header block, given the current offset. */
size_t gw_next_line(const char *buf, size_t len, size_t off);

/*
 * Shell-style glob match, case-insensitive: '*' matches any run of characters
 * including none, '?' matches exactly one. Used against hostnames, so both
 * sides are short and the recursion is shallow.
 *
 * Getting this wrong sends a live site to the archive or an archived one to
 * the live web, and both look like the site itself is broken -- hence the
 * unusually thorough tests.
 */
int gw_glob_match(const char *pattern, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* GW_UTIL_H */
