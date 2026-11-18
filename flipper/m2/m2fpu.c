/* Built for the FPU (-mfloat-abi=softfp, spec R5.7): a little float
 * arithmetic for the m2test FPU checks.  Returns round(1000 * result). */
float fpu_scale = 1.5f;			/* data, so it is not folded */

int fpu_calc(int n)
{
	float x = 0.0f;
	int i;

	for (i = 1; i <= n; i++) x += fpu_scale / (float) i;
	return (int) (x * 1000.0f + 0.5f);
}
