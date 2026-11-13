/* INIT stand-in: an unprivileged user process.  Checks that IPC to servers
 * works, that sending to a task is refused, that the clock ticks while it
 * spins (preemption), then reports PASS and exits QEMU. */
#include "m1.h"

static int fails;

static void check(int ok, const char *what)
{
	fs_print(ok ? "  ok   " : "  FAIL ");
	fs_print(what);
	fs_print("\n");
	if (!ok) fails++;
}

static long uptime(void)
{
	message m;

	m.m_type = M1_UPTIME;
	if (_sendrec(MM_PROC_NR, &m) != OK || m.m_type != OK) return -1;
	return m.m2_l1;
}

int main(void)
{
	message m;
	long t0, t, last;
	int r;

	fs_print("INIT stand-in: running unprivileged\n");

	m.m_type = DEV_WRITE;
	r = _sendrec(TTY, &m);
	check(r == E_BAD_DEST, "user -> TTY refused (E_BAD_DEST)");

	m.m_type = M1_PING;
	r = _sendrec(MM_PROC_NR, &m);
	check(r == OK && m.m_type == OK, "user -> MM sendrec");

	m.m_type = GET_UPTIME;
	r = _sendrec(CLOCK, &m);
	check(r == E_BAD_DEST, "user -> CLOCK refused (E_BAD_DEST)");

	r = _sendrec(12345, &m);
	check(r == E_BAD_SRC || r == E_BAD_DEST, "bogus destination refused");

	t0 = uptime();
	check(t0 >= 0, "uptime via MM");

	/* Spin without blocking: only clock interrupts and preemption can
	 * let MM and CLOCK run to answer. */
	last = t0;
	while ((t = uptime()) >= 0 && t - t0 < 2 * HZ) {
		if (t - last >= HZ) {
			fs_print("  uptime ");
			fs_print_num((unsigned long) t);
			fs_print(" ticks\n");
			last = t;
		}
	}
	check(t - t0 >= 2 * HZ, "clock advances 2 s while INIT spins");

	/* Last check: an unprivileged process must not be able to touch
	 * the system control space (here: stop SysTick).  The write has to
	 * fault; the kernel turns that into a signal and tells MM, which
	 * then reports the overall result. */
	m.m_type = M1_RESULT;
	m.m1_i1 = fails;
	_sendrec(MM_PROC_NR, &m);
	fs_print("  ...  user write to SysTick must fault\n");
	*(volatile unsigned *) 0xE000E010 = 0;
	fs_print("  FAIL user wrote to SysTick\nM1 FAIL\n");
	semihost_exit(1);
}
