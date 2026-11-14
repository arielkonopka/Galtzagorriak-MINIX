#include "syslib.h"

PUBLIC int sys_flash(op, addr, buf, len, base, size)
int op;				/* FL_INFO, FL_ERASE, FL_WRITE */
unsigned long addr;		/* flash address */
void *buf;			/* data to program */
unsigned len;			/* bytes */
unsigned long *base, *size;	/* FL_INFO: the cache region */
{
/* Flash text cache access for MM (ARM_CM only). */
  message m;
  int r;

  m.m1_i1 = op;
  m.m1_i2 = (int) len;
  m.m1_p1 = (char *) buf;
  m.m1_p2 = (char *) addr;
  r = _taskcall(SYSTASK, SYS_FLASH, &m);
  if (op == FL_INFO && r == 0) {
	*base = (unsigned long) m.m2_l1;
	*size = (unsigned long) m.m2_l2;
  }
  return(r);
}
