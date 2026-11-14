/* mpucover.h - cover a memory range with ARMv7-M MPU regions
 *
 * Shared by the kernel (arm/klib.c, mpu_switch: loads the regions of an
 * unprivileged process) and MM (exec.c: refuses a layout that cannot be
 * covered, R1.6), so both agree on what is coverable.
 */
#ifndef _MINIX_MPUCOVER_H
#define _MINIX_MPUCOVER_H

#define MPUC_REGIONS	7		/* regions 1..7; region 0 is flash */
#define MPUC_AP_RW_RW	(3UL << 24)	/* read/write, privileged and user */
#define MPUC_ENABLE	1UL

/* Cover [lo, hi) exactly (multiples of 256) with at most max regions,
 * using subregions: each step takes the largest aligned power-of-two
 * region whose subregion boundaries match lo and the covered end.  Returns
 * the number of regions, or -1 if max is not enough.  rbar and rasr may be
 * NULL to only count.
 */
static int mpu_cover(unsigned long lo, unsigned long hi, unsigned long *rbar,
				unsigned long *rasr, int max)
{
  int n = 0, k, s0, s1;
  unsigned long size, base, sub, end;

  while (lo < hi) {
	if (n == max) return -1;
	for (k = 31; k >= 8; k--) {
		size = 1UL << k;
		base = lo & ~(size - 1);
		sub = size >> 3;
		if (((lo - base) & (sub - 1)) != 0) continue;
		end = hi - base > size ? base + size : hi;
		if (((end - base) & (sub - 1)) != 0 || end <= lo) continue;
		s0 = (lo - base) / sub;
		s1 = (end - base) / sub;
		if (rbar != 0) {
			rbar[n] = base;
			rasr[n] = MPUC_AP_RW_RW | ((unsigned long) (k - 1) << 1)
			    | ((~(((1UL << s1) - 1) & ~((1UL << s0) - 1))
			       & 0xFF) << 8)
			    | MPUC_ENABLE;
		}
		n++;
		lo = end;
		break;
	}
	if (k < 8) return -1;
  }
  return n;
}

#endif /* _MINIX_MPUCOVER_H */
