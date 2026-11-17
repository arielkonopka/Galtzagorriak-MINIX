#include "syslib.h"

PUBLIC int sys_swap(proc, out)
int proc;			/* the process */
int out;			/* 1: its image is on the swap device, 0: back */
{
/* Tell the kernel that a process's image left RAM or came back (R12.4,
 * R12.6; ARM_CM only).  While swapped the process never runs and nothing
 * may be copied into its range.
 */
  message m;

  m.m1_i1 = proc;
  m.m1_i2 = out;
  return(_taskcall(SYSTASK, SYS_SWAP, &m));
}
