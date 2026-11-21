/* The float math Berry wants (BE_USE_SINGLE_FLOAT) on MINIX's libc,
 * which has only the double functions (src/lib/math, and those need
 * modf/frexp, which exist only as i386 assembly): the VM uses just
 * fmodf (gzmath.c, real % real); the math module is off on the device
 * until a small float libm exists (PORTING.md "Berry").  Included before every Berry source (minix.mk
 * BERRY_CFLAGS), only for the device build. */
#ifndef GZMATH_H
#define GZMATH_H
#include <math.h>
#define INFINITY	__builtin_inff()
#define NAN		__builtin_nanf("")
#define HUGE_VALF	__builtin_huge_valf()
#define isnan(x)	__builtin_isnan(x)
#define isinf(x)	__builtin_isinf(x)
#define F1(f, x)	((float)f((double)(x)))
#define sinf(x)		F1(sin, x)
#define cosf(x)		F1(cos, x)
#define tanf(x)		F1(tan, x)
#define asinf(x)	F1(asin, x)
#define acosf(x)	F1(acos, x)
#define atanf(x)	F1(atan, x)
#define atan2f(y, x)	((float)atan2((double)(y), (double)(x)))
#define sinhf(x)	F1(sinh, x)
#define coshf(x)	F1(cosh, x)
#define tanhf(x)	F1(tanh, x)
#define sqrtf(x)	__builtin_sqrtf(x)	/* vsqrt.f32 */
#define expf(x)		F1(exp, x)
#define logf(x)		F1(log, x)
#define log10f(x)	F1(log10, x)
#define powf(x, y)	((float)pow((double)(x), (double)(y)))
#define fmodf(x, y)	gz_fmodf(x, y)	/* gzmath.c */
float gz_fmodf(float x, float y);
#define fabsf(x)	__builtin_fabsf(x)	/* vabs.f32 */
#define ceilf(x)	F1(ceil, x)
#define floorf(x)	F1(floor, x)
#define roundf(x)	((x) < 0 ? -floorf(0.5f - (x)) : floorf((x) + 0.5f))
#endif
