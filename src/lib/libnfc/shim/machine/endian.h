/* machine/endian.h (newlib) - __bswap32 for the mykey parser (#412). */
#pragma once
#include <stdint.h>
#ifndef __bswap32
#define __bswap32(x) __builtin_bswap32(x)
#endif
