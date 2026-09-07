/*
 * gw_platform.h - the handful of platform primitives the protocol layer uses.
 *
 * src/proxy/ is 2,100 lines of state machine that has nothing to do with any
 * operating system, but it was written against Mac spellings for sized types
 * and heap allocation. Rather than rename 56 call sites and re-verify them,
 * this header supplies those spellings everywhere, so the modules compile
 * unchanged on both platforms. docs/porting.md section 1 sizes the problem.
 *
 * This is deliberately NOT in src/portable/. That directory is guarded by
 * host-tests.yml, which fails the build if anything there so much as includes
 * a platform header, and the Mac branch below does exactly that.
 *
 * GW_MAC_OS is set by CMakeLists.txt for the Classic build. Keying off a
 * definition we control beats guessing at which predefined macro a given
 * toolchain happens to set.
 */
#ifndef GW_PLATFORM_H
#define GW_PLATFORM_H

#include <stddef.h>

#ifdef GW_MAC_OS

#include <MacTypes.h>
#include <MacMemory.h>

#else

#include <stdlib.h>

typedef char           *Ptr;
typedef unsigned char   UInt8;
typedef unsigned short  UInt16;
typedef unsigned long   UInt32;
typedef short           SInt16;
typedef long            SInt32;
typedef long            Size;
typedef long            OSStatus;
typedef unsigned char   Boolean;

#ifndef noErr
#define noErr 0
#endif
#ifndef true
#define true  1
#endif
#ifndef false
#define false 0
#endif

/*
 * The Memory Manager's three calls, which is all Gateway uses of it. NewPtr
 * does not clear and NewPtrClear does, matching the Toolbox; every caller
 * relies on that distinction.
 */
#define NewPtr(n)       ((Ptr)malloc((size_t)(n)))
#define NewPtrClear(n)  ((Ptr)calloc(1, (size_t)(n)))
#define DisposePtr(p)   free((void *)(p))

#endif /* GW_MAC_OS */

#endif /* GW_PLATFORM_H */
