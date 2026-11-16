/* Host stub of src/kernel/kernel.h for the PC tests (tests/host): only what
 * the kernel files under test need.  `long` is 32 bits on the Flipper; on
 * the PC it would be 64 and hide every wrap-around, so it is made `int`. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define long	int
#define PUBLIC
#define PRIVATE	static
#define FORWARD	static
#define EXTERN	extern
#define _PROTOTYPE(f, a)	f a
#define CHIP		1
#define ARM_CM		1
#define SECTOR_SIZE	512
#define SECTOR_SHIFT	9
#define SECTOR_MASK	511
#define TRUE		1
#define FALSE		0

typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
typedef struct { int m_type; } message;

#ifdef T_QUIET		/* the kernel's messages: not on the test's output */
int t_quiet(const char *fmt, ...);
#undef printf
#define printf	t_quiet
#endif
