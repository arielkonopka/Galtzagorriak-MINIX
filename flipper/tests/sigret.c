/* sigret - S1/R1.1 and A1/R1.2: a forged stack pointer must not let a
 * process write kernel memory or halt the system.
 *  1. sigreturn() with sc_sp pointing into kernel RAM   -> EFAULT
 *  2. the same through the SC_NOREGLOCALS path          -> EFAULT
 *  3. sigreturn() with an unaligned sc_sp               -> EFAULT
 *  4. a system call made with SP outside RAM, and in kernel RAM
 *     -> only this process dies (SIGSEGV), the system keeps running
 */
#include "t.h"
#include <sys/sigcontext.h>

#define KERNEL_RAM	0x20000100L	/* kernel .data/.bss (ld/kernel.ld) */

static long target = KERNEL_RAM + 32;
static int flags;

static void forge(void)
{
	struct sigcontext sc;
	int r;

	memset(&sc, 0, sizeof(sc));
	sc.sc_flags = SC_SIGCONTEXT | flags;
	sc.sc_sp = target;
	sc.sc_pc = (long) forge;
	r = sigreturn(&sc);
	_exit(r == -1 && errno == EFAULT ? 0 : 1);
}

static long bad_sp = KERNEL_RAM + 64;

static void bad_sp_call(void)
{
	/* SP into kernel RAM, then an SVC: the hardware stacks there with
	 * the process's rights, or the kernel reads the frame with them. */
	__asm volatile ("mov sp, %0\n\tsvc #0"
			: : "r" (bad_sp) : "memory");
	_exit(1);			/* still here: wrong */
}

int main(void)
{
	int st;

	target = KERNEL_RAM + 32;
	flags = 0;
	st = t_child(forge);
	t_result("sigreturn, sc_sp in kernel RAM", st == 0, t_status(st));

	flags = SC_NOREGLOCALS;
	st = t_child(forge);
	t_result("sigreturn (NOREGLOCALS), sc_sp in kernel RAM", st == 0,
							t_status(st));

	flags = 0;
	target = (long) &st + 2;		/* our stack, unaligned */
	st = t_child(forge);
	t_result("sigreturn, unaligned sc_sp", st == 0, t_status(st));

	bad_sp = 0x60000040L;			/* nothing there */
	st = t_child(bad_sp_call);
	t_result("system call with SP outside RAM",
		st != -1 && WIFSIGNALED(st), t_status(st));

	bad_sp = KERNEL_RAM + 64;
	st = t_child(bad_sp_call);
	t_result("system call with SP in kernel RAM",
		st != -1 && WIFSIGNALED(st), t_status(st));

	t_result("system still running", 1, NULL);
	return t_fail;
}
