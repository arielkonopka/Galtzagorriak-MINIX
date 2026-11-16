#include <lib.h>
#define stime	_stime
#include <minix/minlib.h>
#include <time.h>

PUBLIC int stime(top)
time_t *top;
{
/* The 64-bit time goes in two halves, m2_l1 low and m2_l2 high (R2d.2);
 * FS answers EINVAL for a time the clock cannot keep. */
  message m;

  m.m2_l1 = (long) (unsigned long) *top;
  m.m2_l2 = (long) (unsigned long) ((unsigned long long) *top >> 32);
  return(_syscall(FS, STIME, &m));
}
