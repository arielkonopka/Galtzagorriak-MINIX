/* fmodf for the Berry VM (real % real) on the device; see gzmath.h. */
#include "gzmath.h"

float gz_fmodf(float x, float y)
{
	float q;

	if (y == 0.0f || isnan(x) || isnan(y) || isinf(x)) return NAN;
	if (isinf(y)) return x;
	q = x / y;
	/* |q| >= 2^24: every float is an integer, the remainder is lost */
	if (q >= 16777216.0f || q <= -16777216.0f) return 0.0f;
	return x - (float)(long)q * y;	/* truncation toward zero, as C */
}
