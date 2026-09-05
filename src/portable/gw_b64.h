/*
 * gw_b64.h - base64, used by SASL PLAIN/LOGIN and XOAUTH2.
 *
 * PORTABLE: no Mac or Windows headers. See gw_util.h.
 */
#ifndef GW_B64_H
#define GW_B64_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encode len bytes into a NUL-terminated string. Returns the encoded length,
 * or 0 when it would not fit in cap. */
size_t gw_b64_encode(const void *src, size_t len, char *out, size_t cap);

/* Decode a base64 string, ignoring embedded whitespace. Returns the decoded
 * length, or (size_t)-1 on invalid input / overflow. */
size_t gw_b64_decode(const char *src, size_t len, void *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* GW_B64_H */
