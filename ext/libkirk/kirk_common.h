#pragma once

typedef unsigned char u8;
typedef unsigned short int u16;
typedef unsigned int u32;


// KIRK headers live in byte buffers in the PSP's (little-endian) layout. Their 32-bit fields go
// through this on the way in and out, so the code also works on big-endian hosts.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define KIRK_LE32(x) ((u32)__builtin_bswap32((u32)(x)))
#else
#define KIRK_LE32(x) ((u32)(x))
#endif
