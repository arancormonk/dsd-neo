/* Endian names the Apache-2.0 TETRA sources expect.
 * Force-included while compiling those sources. The upstream files stay unchanged. */
#ifndef TETRA_TOOLS_GCC_COMPAT_H
#define TETRA_TOOLS_GCC_COMPAT_H

#if defined(_MSC_VER) && !defined(__clang__)
#include <stdlib.h>
#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN 1234
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN 4321
#endif
#ifndef __BYTE_ORDER
#define __BYTE_ORDER __LITTLE_ENDIAN
#endif
#ifndef __builtin_bswap16
#define __builtin_bswap16(x) _byteswap_ushort((unsigned short)(x))
#endif
#ifndef __builtin_bswap32
#define __builtin_bswap32(x) _byteswap_ulong((unsigned long)(x))
#endif
#ifndef __builtin_bswap64
#define __builtin_bswap64(x) _byteswap_uint64((__int64)(x))
#endif
#else
#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN __ORDER_LITTLE_ENDIAN__
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN __ORDER_BIG_ENDIAN__
#endif
#ifndef __BYTE_ORDER
#define __BYTE_ORDER __BYTE_ORDER__
#endif
#endif

#endif
