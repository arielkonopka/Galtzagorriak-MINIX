#include "m1.h"

static int slen(const char *s)
{
	int n = 0;

	while (s[n] != 0) n++;
	return n;
}

/* Ask TTY to write count bytes at addr in process proc's memory, like FS
 * does for write(2).  Servers only: users may not send to tasks. */
int tty_write_for(int proc, char *addr, int count)
{
	message m;

	m.m_type = DEV_WRITE;
	m.TTY_LINE = 0;
	m.PROC_NR = proc;
	m.ADDRESS = addr;
	m.COUNT = count;
	m.TTY_FLAGS = 0;
	if (_sendrec(TTY, &m) != OK) return -1;
	return m.REP_STATUS;
}

int tty_write(int me, const char *s)
{
	return tty_write_for(me, (char *) s, slen(s));
}

/* For user processes: print through the FS stand-in. */
int fs_print(const char *s)
{
	message m;

	m.m_type = M1_PRINT;
	m.ADDRESS = (char *) s;
	m.COUNT = slen(s);
	if (_sendrec(FS_PROC_NR, &m) != OK) return -1;
	return m.m_type;
}

void fs_print_num(unsigned long n)
{
	char buf[12];
	int i = sizeof buf - 1;

	buf[i] = 0;
	do {
		buf[--i] = '0' + n % 10;
		n /= 10;
	} while (n != 0);
	fs_print(buf + i);
}

/* ARM semihosting SYS_EXIT_EXTENDED; QEMU needs userspace=on for this to
 * work from an unprivileged process.  On real hardware: loop. */
void semihost_exit(int status)
{
#ifdef BOARD_FLIPPER
	/* No debugger attached: a semihosting bkpt would just fault. */
	for (;;) {
		message m;
		_receive(ANY, &m);
	}
#else
	unsigned block[2] = { 0x20026, (unsigned) status };
	register unsigned r0 __asm("r0") = 0x20;
	register unsigned r1 __asm("r1") = (unsigned) block;

	__asm volatile ("bkpt 0xAB" : : "r" (r0), "r" (r1) : "memory");
	for (;;) ;
#endif
}

void exit(int status)
{
	for (;;) {
		message m;
		_receive(ANY, &m);	/* servers never return */
	}
}
