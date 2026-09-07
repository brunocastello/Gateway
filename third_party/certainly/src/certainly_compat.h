/*
 * certainly_compat.h - the three Toolbox calls the TLS core makes.
 *
 * certainly.c allocates with NewPtrClear, frees with DisposePtr and reads the
 * clock with TickCount. All three are Mac OS calls, and all three have exact
 * equivalents elsewhere, so rather than rewrite the core they are supplied
 * here for platforms that lack them. docs/porting.md counted them before the
 * port began and this is the whole list.
 */
#ifndef CERTAINLY_COMPAT_H
#define CERTAINLY_COMPAT_H

#ifndef CERTAINLY_OPEN_TRANSPORT

#include <stdlib.h>
#include <stdint.h>
#include <windows.h>

#define NewPtrClear(n) ((void *)calloc(1, (size_t)(n)))
#define DisposePtr(p)  free((void *)(p))

/*
 * Sixtieths of a second, which is what TickCount counts and what every
 * timeout in the TLS core was written against. The 64-bit intermediate
 * matters: GetTickCount passes a milliard milliseconds in under a fortnight of
 * uptime, and multiplying that by 60 in 32 bits wraps long before it does.
 */
static uint32_t TickCount(void)
{
    return (uint32_t)((ULONGLONG)GetTickCount() * 60 / 1000);
}

#endif /* !CERTAINLY_OPEN_TRANSPORT */

#endif /* CERTAINLY_COMPAT_H */
