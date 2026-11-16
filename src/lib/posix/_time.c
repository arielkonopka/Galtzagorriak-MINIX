#include <lib.h>
#define time	_time
#include <time.h>

PUBLIC time_t time(tp)
time_t *tp;
{
/* The 64-bit time comes in two halves, m2_l1 low and m2_l2 high (R2d.2). */
  message m;
  time_t t;

  if (_syscall(FS, TIME, &m) < 0) return( (time_t) -1);
  t = (time_t) ((unsigned long long) (unsigned long) m.m2_l2 << 32
					| (unsigned long) m.m2_l1);
  if (tp != (time_t *) 0) *tp = t;
  return(t);
}
