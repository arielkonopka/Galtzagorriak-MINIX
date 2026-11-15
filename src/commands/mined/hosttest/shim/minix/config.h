/* Host shim (T8 tests): what mined takes from <minix/config.h>. */
#ifndef _SHIM_MINIX_CONFIG_H
#define _SHIM_MINIX_CONFIG_H
#define INTEL		1
#define M68000		2
#define ARM_CM		4
#define CHIP		ARM_CM
#define _PROTOTYPE(f, a)	f a
#endif
