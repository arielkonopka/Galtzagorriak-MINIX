#include "syslib.h"

PUBLIC int sys_kstacks(proc, buf, len)
int proc;			/* process that gets the table */
char *buf;			/* struct kstack array in that process */
int len;			/* its size in bytes */
{
/* Ask the kernel for the stack use of its tasks (ARM_CM only).  Returns
 * the number of entries, or an error.
 */
  message m;

  m.m1_i1 = proc;
  m.m1_i2 = len;
  m.m1_p1 = buf;
  return(_taskcall(SYSTASK, SYS_KSTACKS, &m));
}
