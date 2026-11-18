/* Milestone 2/3 user-level test, run by the test INIT via exec():
 * relocated executable, argv/environ, malloc/brk, signals (kernel signal
 * frame + sigreturn), fork/wait (shadowing), pipes and exec from a child.
 */
#include <lib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <minix/callnr.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <minix/fg.h>
#include <minix/ibutton.h>
#include <minix/hid.h>
#include <minix/subghz.h>
#include "libhid.h"
#include <minix/partition.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>

static int fails;
static volatile int got_sig;
static int data_word = 12345;		/* initialised data */
static char *msg = "relocated pointer";	/* needs a relocation */

static void check(int ok, const char *what)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) fails++;
}

/* memcpy/memmove/memset (arm/rts/memops.S) against byte loops: every
 * alignment of source and destination, lengths 0..80, overlap both ways. */
static unsigned char ta[200], tb[200], tr[200];

static void fill(unsigned char *p, int seed)
{
	int i;

	for (i = 0; i < 200; i++) p[i] = (unsigned char) (i * 7 + seed);
}

static int memops_ok(void)
{
	int so, d, n, i, k;

	for (so = 0; so < 4; so++) for (d = 0; d < 4; d++)
	for (n = 0; n <= 80; n++) {
		fill(ta, 1); fill(tb, 2); fill(tr, 2);
		for (i = 0; i < n; i++) tr[50 + d + i] = ta[10 + so + i];
		if (memcpy(tb + 50 + d, ta + 10 + so, n) != tb + 50 + d) return 0;
		if (memcmp(tb, tr, 200) != 0) return 0;

		fill(tb, 3); fill(tr, 3);
		for (i = 0; i < n; i++) tr[20 + d + i] = (unsigned char) (so + 9);
		if (memset(tb + 20 + d, so + 9 + 256, n) != tb + 20 + d) return 0;
		if (memcmp(tb, tr, 200) != 0) return 0;

		for (k = -9; k <= 9; k++) {		/* overlapping moves */
			fill(tb, 4); fill(tr, 4);
			for (i = 0; i < n; i++) ta[i] = tr[60 + so + i];
			for (i = 0; i < n; i++) tr[60 + so + k + i] = ta[i];
			if (memmove(tb + 60 + so + k, tb + 60 + so, n)
						!= tb + 60 + so + k) return 0;
			if (memcmp(tb, tr, 200) != 0) return 0;
		}
	}
	return 1;
}

/* Recursion with every byte of each frame written: the stack moves down
 * without skipping the 32 B heap guard. */
static int dig(int n)
{
	volatile char frame[48];

	memset((char *) frame, n, sizeof frame);
	return n > 100000 ? 0 : dig(n + 1) + frame[n % 48];
}

static void handler(int sig)
{
	got_sig = sig;
}

/* Flash pages erased since boot (R2b.11 counters), for CI's flashcheck:
 * the whole M2 run must not erase more than flipper/flash-budget.txt. */
static void flash_report(void)
{
	message m;
	struct fccount fc;

	m.m1_p1 = (char *) &fc;
	m.m1_i1 = -1;			/* read only */
	m.m1_i2 = MI_FCBUDGET;
	if (_syscall(MM, MEMINFO, &m) != 0) {
		printf("M2 FLASH unknown\n");
		return;
	}
	printf("M2 FLASH erased cache=%lu inst=%lu\n", fc.erased,
							fc.inst_erased);
}

/* Phase 2d (UTC, 64-bit time_t).  gmtime and mktime against known dates:
 * time, wday, yday and the year, both ways. */
static int date_is(time_t t, int y, int mo, int d, int h, int mi, int se,
							int wd, int yd)
{
	struct tm *tm = gmtime(&t), c;

	if (tm == NULL || tm->tm_year != y - 1900 || tm->tm_mon != mo - 1
	    || tm->tm_mday != d || tm->tm_hour != h || tm->tm_min != mi
	    || tm->tm_sec != se || tm->tm_wday != wd || tm->tm_yday != yd)
		return 0;
	c = *tm;			/* back with mktime, TZ=UTC0 */
	c.tm_isdst = 0;
	return mktime(&c) == t;
}

static void alarm_handler(int sig)
{
	got_sig = sig;
}

static char tz_utc[] = "TZ=UTC0";	/* putenv writes into them: not const */
static char tz_cet[] = "TZ=CET-1CEST,M3.5.0,M10.5.0/3";

static void time_tests(void)
{
	struct tm *tm, lt;
	struct stat st;
	struct utimbuf ut;
	time_t t0, t, a;
	int fd, r, e;

	putenv(tz_utc);
	check(sizeof(time_t) == 8 && (time_t) -1 < 0
		&& date_is((time_t) 2147483648LL, 2038, 1, 19, 3, 14, 8, 2, 18)
		&& date_is((time_t) -1, 1969, 12, 31, 23, 59, 59, 3, 364)
		&& date_is((time_t) 4107542400LL, 2100, 3, 1, 0, 0, 0, 1, 59)
		&& date_is((time_t) 253402300799LL, 9999, 12, 31, 23, 59, 59,
									5, 364)
		&& date_is((time_t) -2203891200LL, 1900, 3, 1, 0, 0, 0, 4, 59),
		"64-bit time_t: gmtime/mktime 1900-9999 (R2d.4)");

	/* AC2d.5: CET/CEST spring forward, 2026-03-29 01:00 UTC */
	putenv(tz_cet);
	t = 1774745999L;
	tm = localtime(&t);
	r = tm->tm_hour == 1 && tm->tm_min == 59 && tm->tm_sec == 59
		&& tm->tm_isdst == 0;
	t++;
	tm = localtime(&t);
	r = r && tm->tm_hour == 3 && tm->tm_min == 0 && tm->tm_sec == 0
		&& tm->tm_isdst == 1;
	lt = *tm;
	lt.tm_isdst = -1;
	check(r && mktime(&lt) == t, "TZ CET-1CEST: 01:59:59 -> 03:00:00 local"
							" (AC2d.5)");
	putenv(tz_utc);

	/* AC2d.2: files keep 1970-2106 */
	fd = creat("/tmp/t2d", 0644);
	if (fd >= 0) close(fd);
	ut.actime = ut.modtime = 4291747200LL;		/* 2106-01-01 */
	r = utime("/tmp/t2d", &ut);
	check(fd >= 0 && r == 0 && stat("/tmp/t2d", &st) == 0
		&& st.st_mtime == 4291747200LL && st.st_atime == 4291747200LL,
		"utime 2106-01-01 kept (AC2d.2)");
	ut.actime = ut.modtime = 4323283200LL;		/* 2107-01-01 */
	errno = 0;
	r = utime("/tmp/t2d", &ut);
	check(r == -1 && errno == EINVAL, "utime 2107 -> EINVAL (AC2d.2)");

	/* AC2d.1: the clock across 2038-01-19 03:14:07 UTC */
	t0 = time((time_t *) 0);
	t = 4294967296LL + 5;				/* past 2106 */
	errno = 0;
	r = stime(&t);
	e = errno;
	t = 2147483647LL - 1;			/* 03:14:06 */
	check(r == -1 && e == EINVAL && stime(&t) == 0, "stime: 2106+ -> "
					"EINVAL, 2038-01-19 03:14:06 ok");
	signal(SIGALRM, alarm_handler);
	got_sig = 0;
	alarm(3);
	while (got_sig == 0) pause();
	t = time((time_t *) 0);
	check(t >= 2147483648LL && t <= 2147483652LL,
		"time() and alarm(3) across 2038-01-19 03:14:08 (AC2d.1)");
	r = utime("/tmp/t2d", (struct utimbuf *) NULL);	/* now */
	tm = stat("/tmp/t2d", &st) == 0 ? gmtime(&st.st_mtime) : NULL;
	check(r == 0 && tm != NULL && st.st_mtime >= 2147483648LL
		&& tm->tm_year == 138 && st.st_mtime > (time_t) 0x7FFFFFFF,
		"a file stamped after 2038-01-19 03:14:07 (AC2d.1)");
	unlink("/tmp/t2d");
	t = t0 + (time((time_t *) 0) - 2147483646LL);	/* the clock back */
	stime(&t);
	signal(SIGALRM, SIG_DFL);
}

#if REALTIME_WRAP
/* AC2d.3, the uptime-wrap test build (make WRAPTEST=1): the uptime ticks
 * wrap at 2^32 REALTIME_WRAP seconds after the kernel started.  An alarm
 * set before the wrap must come after it, on time, and sleep() too. */
static void wrap_test(void)
{
	struct tms tms;
	unsigned long up = (unsigned long) times(&tms), left;
	time_t a, t;

	left = (0UL - up) / HZ;
	if (up < 0x80000000UL) {
		check(0, "uptime wrap: passed before the test (make "
				"WRAPTEST=<more seconds>)");
		return;
	}
	printf("  uptime wraps in %lu s\n", left);
	signal(SIGALRM, alarm_handler);
	a = time((time_t *) 0);
	got_sig = 0;
	alarm((unsigned) left + 3);
	while (got_sig == 0) pause();
	t = time((time_t *) 0) - a;
	up = (unsigned long) times(&tms);
	check(t >= (time_t) left + 2 && t <= (time_t) left + 4
		&& up < 0x80000000UL, "alarm across the uptime wrap (AC2d.3)");
	a = time((time_t *) 0);
	sleep(2);
	t = time((time_t *) 0) - a;
	check(t >= 1 && t <= 3, "sleep(2) after the uptime wrap (AC2d.3)");
	signal(SIGALRM, SIG_DFL);
}
#endif

/* poll() (spec R4b.6; devices with DEV_SELECT: devpoll_tests): a pipe and a 250 ms timeout, a pipe a child writes to,
 * EINTR from alarm(), hangup, bad descriptors, a device without
 * DEV_SELECT (always ready). */
static void poll_tests(void)
{
	struct pollfd p[3];
	struct tms tm;
	clock_t t0, t1;
	int fd[2], r, e, st;
	pid_t pid;
	char c;

	if (pipe(fd) != 0) {
		check(0, "poll: pipe");
		return;
	}
	p[0].fd = fd[0]; p[0].events = POLLIN; p[0].revents = 0x7FFF;
	t0 = times(&tm);
	r = poll(p, 1, 250);
	t1 = times(&tm);
	printf("  poll 250 ms: %ld ticks\n", (long) (t1 - t0));
	check(r == 0 && p[0].revents == 0 && t1 - t0 >= 15 && t1 - t0 < 60,
				"poll: empty pipe, 250 ms timeout");

	p[1].fd = fd[1]; p[1].events = POLLOUT; p[1].revents = 0;
	r = poll(p, 2, 0);
	check(r == 1 && p[0].revents == 0 && p[1].revents == POLLOUT,
				"poll: timeout 0, write end writable");

	pid = fork();
	if (pid == 0) {
		sleep(1);
		write(fd[1], "x", 1);
		_exit(0);
	}
	p[0].revents = 0;
	t0 = times(&tm);
	r = poll(p, 1, 5000);
	t1 = times(&tm);
	check(r == 1 && p[0].revents == POLLIN && t1 - t0 < 3 * 60
		&& read(fd[0], &c, 1) == 1 && c == 'x',
				"poll: pipe becomes readable from a child");
	check(wait(&st) == pid, "poll: child done");

	signal(SIGALRM, alarm_handler);
	got_sig = 0;
	alarm(1);
	r = poll(p, 1, -1);
	e = errno;
	alarm(0);
	check(r == -1 && e == EINTR && got_sig == SIGALRM,
				"poll: EINTR on alarm");

	/* After EINTR the registration is gone: a later poll still works. */
	write(fd[1], "y", 1);
	r = poll(p, 1, 1000);
	check(r == 1 && p[0].revents == POLLIN && read(fd[0], &c, 1) == 1,
				"poll: works again after EINTR");

	close(fd[1]);
	r = poll(p, 1, 1000);
	check(r == 1 && (p[0].revents & POLLHUP) && !(p[0].revents & POLLIN),
				"poll: writer gone -> POLLHUP");
	p[2].fd = open("/dev/null", O_RDWR); p[2].events = POLLIN | POLLOUT;
	close(fd[0]);

	p[0].fd = fd[0]; p[0].events = POLLIN;	/* closed now */
	p[1].fd = -1; p[1].events = POLLIN; p[1].revents = 0x55;
	r = poll(p, 3, -1);
	check(r == 2 && p[0].revents == POLLNVAL && p[1].revents == 0
		&& p[2].revents == (POLLIN | POLLOUT),
		"poll: bad fd -> POLLNVAL, fd -1 ignored, /dev/null ready");
	if (p[2].fd >= 0) close(p[2].fd);

	t0 = times(&tm);
	r = poll((struct pollfd *) NULL, 0, 100);
	t1 = times(&tm);
	check(r == 0 && t1 - t0 >= 6 && t1 - t0 < 40, "poll: no fds, 100 ms");

	/* Two pollers at once with different timeouts: FS's one alarm goes
	 * to the earlier, then on to the later. */
	pid = fork();
	if (pid == 0) {
		t0 = times(&tm);
		r = poll((struct pollfd *) NULL, 0, 600);
		t1 = times(&tm);
		_exit(r == 0 && t1 - t0 >= 36 && t1 - t0 < 80 ? 0 : 1);
	}
	t0 = times(&tm);
	r = poll((struct pollfd *) NULL, 0, 200);
	t1 = times(&tm);
	check(r == 0 && t1 - t0 >= 12 && t1 - t0 < 30 && wait(&st) == pid
		&& WIFEXITED(st) && WEXITSTATUS(st) == 0,
		"poll: two pollers, 200 and 600 ms");
	r = poll(p, 1000, 0);
	check(r == -1 && errno == EINVAL, "poll: too many fds -> EINVAL");
	r = poll((struct pollfd *) 0x20000100, 1, 0);
	check(r == -1 && errno == EFAULT, "poll: fds in kernel RAM -> EFAULT");
}

/* R7b.3.1: MM tests a free 4 KB block of RAM in place (MI_MEMTEST). */
static int memtest(unsigned long addr)
{
	message m;

	m.m1_i1 = (int) addr;
	m.m1_i2 = MI_MEMTEST;
	m.m1_p1 = NULL;
	return _syscall(MM, MEMINFO, &m);
}

static void memtest_tests(void)
{
	static int mine = 1;
	unsigned long holes[2 * 16], blk, end;
	message m;
	int i, r = -9;

	m.m1_i1 = (int) sizeof holes;
	m.m1_i2 = 0;
	m.m1_p1 = (char *) holes;
	if (_syscall(MM, MEMINFO, &m) < 0) {
		check(0, "memtest: hole list");
		return;
	}
	for (i = 0; i < 16 && holes[2 * i + 1] != 0; i++) {
		blk = (holes[2 * i] + MT_BLOCK - 1) & ~(unsigned long) (MT_BLOCK - 1);
		end = holes[2 * i] + holes[2 * i + 1];
		if (blk + MT_BLOCK <= end) {
			r = memtest(blk);
			break;
		}
	}
	check(r == MT_PASS, "memtest: a free block passes March C-");
	check(memtest((unsigned long) &mine & ~(unsigned long) (MT_BLOCK - 1))
		== MT_INUSE, "memtest: our own data is in use");
	check(memtest(0x20000000UL) == MT_OWNED, "memtest: the kernel's RAM is not ours");
	check(memtest(0x20000100UL) < 0 && errno == EINVAL,
		"memtest: an unaligned address -> EINVAL");
}

/* Phase 3: the sticky bit (R3.7, AC3.5) and snprintf (R3.6). */
static void phase3_tests(void)
{
	struct stat sb;
	char b[8];
	int fd, st, r1, r2, ok1;
	pid_t pid;

	r1 = snprintf(b, sizeof b, "%d-%s", 42, "ab");
	ok1 = r1 == 5 && strcmp(b, "42-ab") == 0;
	r2 = snprintf(b, sizeof b, "%s", "0123456789");
	check(ok1 && r2 == -1 && strcmp(b, "0123456") == 0,
		"snprintf: stores at most n - 1, -1 when cut (R3.6)");

	fd = creat("/tmp/rootfile", 0666);
	if (fd >= 0) close(fd);
	(void) rmdir("/tmp/sticky");
	r1 = mkdir("/tmp/sticky", 0777) == 0 && chmod("/tmp/sticky", 01777) == 0
		&& stat("/tmp/sticky", &sb) == 0 && (sb.st_mode & 07777) == 01777;
	(void) rmdir("/tmp/sticky");
	pid = fork();
	if (pid == 0) {
		int rm_no, mv_no, own;

		setgid(99);
		setuid(9999);			/* nobody */
		rm_no = unlink("/tmp/rootfile") == -1 && errno == EPERM;
		mv_no = rename("/tmp/rootfile", "/tmp/nobodys") == -1
							&& errno == EPERM;
		own = (fd = creat("/tmp/nobodys", 0644)) >= 0 && close(fd) == 0
		    && rename("/tmp/nobodys", "/tmp/nobodys2") == 0
		    && unlink("/tmp/nobodys2") == 0;
		_exit(rm_no && mv_no && own ? 0 : 1);
	}
	check(stat("/tmp", &sb) == 0 && (sb.st_mode & 01000) != 0 && r1
		&& pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st)
		&& WEXITSTATUS(st) == 0 && unlink("/tmp/rootfile") == 0,
		"sticky /tmp: nobody cannot rm or mv root's file, "
		"may its own (AC3.5)");
}

static void finish(void)
{
	flash_report();
	{
		/* Console output post-processing: the PASS line below must
		 * end in CR NL (OPOST|ONLCR, the default), this one in a bare
		 * NL (OPOST off, as raw programs set it).  CI checks both. */
		struct termios t, raw;

		fflush(stdout);
		if (tcgetattr(1, &t) == 0) {
			raw = t;
			raw.c_oflag &= ~OPOST;
			tcsetattr(1, TCSADRAIN, &raw);
			write(1, "M2 RAW NL\n", 10);
			tcsetattr(1, TCSADRAIN, &t);
		}
	}
	printf(fails == 0 ? "M2 USER PASS\n" : "M2 USER FAIL\n");
	fflush(stdout);
#ifdef BOARD_QEMU
	{
		unsigned block[2] = { 0x20026, fails != 0 };
		register unsigned r0 __asm("r0") = 0x20;
		register unsigned r1 __asm("r1") = (unsigned) block;
		__asm volatile ("bkpt 0xAB" : : "r" (r0), "r" (r1) : "memory");
	}
#endif
	/* As INIT it may never exit; started from a shell (on the Flipper,
	 * #109) it must, or the shell waits for it forever. */
	if (getpid() != 1) exit(fails > 125 ? 125 : fails);
	for (;;) pause();
}

#ifdef BOARD_QEMU
/* libfuri GUI (spec R5.2): furihello, a ViewDispatcher with a Submenu
 * and a Popup, driven with fake buttons (QEMU only).  Its frames go to
 * /dev/lcd (dumped to the console).  OK on "Say hello" -> the popup;
 * Back -> the menu; Down, OK on "Quit" -> exit 0.  Each press is held
 * 60 ms (a Short), with 250 ms for the app to redraw. */
static void furi_tap(int b, int key)
{
	unsigned char m = key;

	(void) write(b, (char *) &m, 1);
	(void) poll((struct pollfd *) NULL, 0, 60);
	m = 0;
	(void) write(b, (char *) &m, 1);
	(void) poll((struct pollfd *) NULL, 0, 250);
}

#ifdef BOARD_QEMU
/* DEV_SELECT in the device tasks (R4b.6, protocol #131): the console's
 * TTY line, /dev/buttons with fake presses (AC4b.3), /dev/speaker while a
 * tune plays.  Before, these were "always ready". */
static void devpoll_tests(void)
{
	struct pollfd p[2];
	struct tms tm;
	clock_t t0, t1;
	unsigned char m[4];
	int b, s, r, st;
	pid_t pid;

	if (isatty(0) && isatty(1)) {
		p[0].fd = 0; p[0].events = POLLIN; p[0].revents = 0;
		p[1].fd = 1; p[1].events = POLLOUT; p[1].revents = 0;
		r = poll(p, 2, 100);
		check(r == 1 && p[0].revents == 0 && p[1].revents == POLLOUT,
				"poll: console writable, no input");
		t0 = times(&tm);
		r = poll(p, 1, 100);
		t1 = times(&tm);
		check(r == 0 && t1 - t0 >= 6 && t1 - t0 < 40,
				"poll: console without input waits 100 ms");
	}

	b = open("/dev/buttons", O_RDWR | O_NONBLOCK);
	if (b < 0) {
		check(0, "poll: open /dev/buttons");
		return;
	}
	furi_tap(b, 0x04);		/* the backlight on: not swallowed */
	while (read(b, (char *) m, 2) == 2) ;
	p[0].fd = b; p[0].events = POLLIN; p[0].revents = 0;
	r = poll(p, 1, 0);
	check(r == 0 && p[0].revents == 0, "poll: /dev/buttons, none queued");
	fflush(stdout);
	pid = fork();
	if (pid == 0) {
		(void) poll((struct pollfd *) NULL, 0, 300);
		m[0] = 0x10;
		(void) write(b, (char *) m, 1);
		(void) poll((struct pollfd *) NULL, 0, 60);
		m[0] = 0;
		(void) write(b, (char *) m, 1);
		_exit(0);
	}
	t0 = times(&tm);
	r = poll(p, 1, 3000);
	t1 = times(&tm);
	printf("  poll /dev/buttons: %ld ticks\n", (long) (t1 - t0));
	check(r == 1 && p[0].revents == POLLIN && t1 - t0 >= 12
		&& t1 - t0 < 60 && read(b, (char *) m, 2) == 2
		&& m[0] == 0x10 && m[1] == 1,
				"poll: /dev/buttons wakes on a press");
	check(pid > 0 && wait(&st) == pid, "poll: button child done");
	while (read(b, (char *) m, 2) == 2) ;
	close(b);

	/* /dev/speaker: a 300 ms note from a child; writable again after */
	s = open("/dev/speaker", O_WRONLY);
	if (s < 0) {
		check(0, "poll: open /dev/speaker");
		return;
	}
	pid = fork();
	if (pid == 0) {
		m[0] = 440 & 0xFF; m[1] = 440 >> 8;
		m[2] = 300 & 0xFF; m[3] = 300 >> 8;
		_exit(write(s, (char *) m, 4) == 4 ? 0 : 1);
	}
	(void) poll((struct pollfd *) NULL, 0, 100);
	p[0].fd = s; p[0].events = POLLOUT; p[0].revents = 0;
	r = poll(p, 1, 0);
	check(r == 0 && p[0].revents == 0, "poll: /dev/speaker busy");
	t0 = times(&tm);
	r = poll(p, 1, 3000);
	t1 = times(&tm);
	check(r == 1 && p[0].revents == POLLOUT && t1 - t0 >= 6
		&& t1 - t0 < 40, "poll: /dev/speaker writable after the tune");
	check(pid > 0 && wait(&st) == pid && WIFEXITED(st)
		&& WEXITSTATUS(st) == 0, "poll: speaker child done");
	close(s);
}
#endif

#ifdef BOARD_QEMU
/* R11.3.1 (T28a): the USB HID files with QEMU's backend, which prints each
 * report as a console line "hid< kbd 02 00 0b ..."; HIDIOCLINE gives that
 * line's FNV-1a hash, so the test checks the very line on the console.
 * HIDIOCFAKE plays the host (unplug, LEDs); HIDP_TEST is a fake persona
 * holding the bus. */
static unsigned long fnv(const char *s)
{
	unsigned long h = 2166136261UL;

	while (*s) h = ((h ^ (unsigned char) *s++) * 16777619UL) & 0xFFFFFFFFUL;
	return h;
}

static int hid_line(int fd, const char *want)
{
	long h = 0;

	return ioctl(fd, HIDIOCLINE, &h) == 0
			&& ((unsigned long) h & 0xFFFFFFFFUL) == fnv(want);
}

static void hid_tests(void)
{
	unsigned char r[16];
	struct pollfd p;
	struct stat sb;
	struct tms tm;
	clock_t t0, t1;
	int k, m, c, n, st, v, e;
	pid_t pid;

	k = open("/dev/hidkbd", O_RDWR);
	m = open("/dev/hidmouse", O_WRONLY);
	c = open("/dev/hidcons", O_WRONLY);
	check(k >= 0 && m >= 0 && c >= 0, "hid: open hidkbd, hidmouse, hidcons");
	if (k < 0 || m < 0 || c < 0) return;
	check(stat("/dev/hidkbd", &sb) == 0 && (sb.st_mode & 0777) == 0660
		&& sb.st_gid == 12 && (sb.st_rdev & 0xFF) == HID_KBD_MINOR,
		"hid: /dev/hidkbd is group usb (12), 0660, minor 17");
	v = 0;
	check(ioctl(k, HIDIOCSTATE, &v) == 0 && (v & 0xFF) == (HID_ATTACHED
		| HID_CONFIGURED) && HID_PERSONA(v) == HIDP_NONE,
		"hid: HIDIOCSTATE says configured, no persona");

	hid_kbd(r, HID_MOD_LSHIFT, 0x0B);
	check(write(k, (char *) r, 8) == 8
		&& hid_line(k, "hid< kbd 02 00 0b 00 00 00 00 00"),
		"hid: a keyboard report, its console line");
	hid_mouse(r, HID_BTN_LEFT, 5, -3, 0);
	check(write(m, (char *) r, 4) == 4
		&& hid_line(m, "hid< mouse 01 05 fd 00"),
		"hid: a mouse report, its console line");
	hid_cons(r, HID_CC_VOL_UP);
	check(write(c, (char *) r, 2) == 2 && hid_line(c, "hid< cons e9 00"),
		"hid: a consumer report, its console line");
	check(hid_type(k, hid_us, "Hi", &n) == 2 && n == 0
		&& hid_line(k, "hid< kbd 00 00 00 00 00 00 00 00"),
		"hid: libhid types \"Hi\", the last line is all keys up");
	r[0] = 0; r[1] = 0x80; r[2] = 0; r[3] = 0;
	e = write(m, (char *) r, 4) < 0 && errno == EINVAL;
	check(e && write(k, (char *) r, 7) < 0 && errno == EINVAL,
		"hid: dx -128 and a short report: EINVAL");

	/* the host unplugs: POLLIN (a change), writes EAGAIN, no POLLOUT;
	 * plugged in again by a child: poll(POLLOUT) wakes */
	(void) ioctl(m, HIDIOCSTATE, &v);		/* seen */
	p.fd = m; p.events = POLLIN | POLLOUT; p.revents = 0;
	check(poll(&p, 1, 0) == 1 && p.revents == POLLOUT,
		"hid: poll: writable, no change");
	v = 0;
	check(ioctl(k, HIDIOCFAKE, &v) == 0 && poll(&p, 1, 0) == 1
		&& p.revents == POLLIN, "hid: unplugged: POLLIN, not writable");
	hid_mouse(r, 0, 1, 0, 0);
	e = write(m, (char *) r, 4) < 0 && errno == EAGAIN;
	check(e && ioctl(m, HIDIOCSTATE, &v) == 0 && (v & 0xFF) == 0
		&& poll(&p, 1, 0) == 0, "hid: unplugged: EAGAIN, state 0, "
		"POLLIN cleared by HIDIOCSTATE");
	fflush(stdout);
	if ((pid = fork()) == 0) {
		(void) poll((struct pollfd *) NULL, 0, 300);
		v = HID_ATTACHED | HID_CONFIGURED;
		_exit(ioctl(k, HIDIOCFAKE, &v) == 0 ? 0 : 1);
	}
	p.events = POLLOUT;
	t0 = times(&tm);
	n = poll(&p, 1, 3000);
	t1 = times(&tm);
	check(n == 1 && p.revents == POLLOUT && t1 - t0 >= 12 && t1 - t0 < 60,
		"hid: poll(POLLOUT) wakes when the host configures us");
	check(pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st)
		&& WEXITSTATUS(st) == 0, "hid: the host child done");

	/* the LED byte: a read waits for a change */
	fflush(stdout);
	if ((pid = fork()) == 0) {
		(void) poll((struct pollfd *) NULL, 0, 300);
		v = HID_ATTACHED | HID_CONFIGURED | HID_LED_CAPS << 16;
		_exit(ioctl(k, HIDIOCFAKE, &v) == 0 ? 0 : 1);
	}
	r[0] = 0xFF;
	t0 = times(&tm);
	n = read(k, (char *) r, 1);
	t1 = times(&tm);
	check(n == 1 && r[0] == HID_LED_CAPS && t1 - t0 >= 12,
		"hid: read on hidkbd waits for the LEDs, gets Caps Lock");
	(void) waitpid(pid, &st, 0);
	n = open("/dev/hidkbd", O_RDONLY | O_NONBLOCK);
	e = n >= 0 && read(n, (char *) r, 1) < 0 && errno == EAGAIN;
	check(e, "hid: no change since: O_NONBLOCK read EAGAIN");
	if (n >= 0) close(n);
	(void) ioctl(k, HIDIOCSTATE, &v);
	v = HID_ATTACHED | HID_CONFIGURED;		/* LEDs off again */
	(void) ioctl(k, HIDIOCFAKE, &v);
	(void) read(k, (char *) r, 1);

	/* one persona at a time: a fake one holds the bus */
	v = HIDP_TEST;
	check(ioctl(k, HIDIOCSPERSONA, &v) == 0, "hid: HIDIOCSPERSONA test");
	n = open("/dev/hidmouse", O_WRONLY);
	e = n < 0 && errno == EBUSY;
	check(e, "hid: persona held: open() EBUSY");
	if (n >= 0) close(n);
	hid_cons(r, HID_CC_MUTE);
	e = write(c, (char *) r, 2) < 0 && errno == EBUSY;
	check(e && ioctl(k, HIDIOCSTATE, &v) == 0
		&& HID_PERSONA(v) == HIDP_TEST, "hid: persona held: write EBUSY, "
		"HIDIOCSTATE names it");
	fflush(stdout);
	if ((pid = fork()) == 0) {		/* not the holder */
		v = HIDP_NONE;
		_exit(ioctl(k, HIDIOCSPERSONA, &v) < 0 && errno == EBUSY ? 0 : 1);
	}
	check(pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st)
		&& WEXITSTATUS(st) == 0, "hid: another process cannot let go");
	close(k);				/* the holder's close lets go */
	k = open("/dev/hidkbd", O_RDWR);
	hid_kbd(r, 0, 0);
	check(k >= 0 && write(k, (char *) r, 8) == 8,
		"hid: the holder closed: open and write again");
	close(k);
	close(m);
	close(c);
}
#endif

#ifdef BOARD_QEMU
/* Issue #302 (spec R8.2): Sub-GHz transmit with QEMU's backend, which
 * prints each frame or packet as console lines ("sg> tx ..."); SGIOCLINE
 * gives the hash of that text, SGIOCFAKE makes the next write a received
 * frame or packet (the hid< / HIDIOCFAKE pattern).  The region is set
 * once; every transmit is checked against its bands and duty cycle. */
static int sg_tune(unsigned long hz)
{
	char t[12];
	int fd = open("/dev/subghz", O_WRONLY), n, ok;

	if (fd < 0) return 0;
	n = sprintf(t, "%lu", hz);
	ok = write(fd, t, n) == n;
	close(fd);
	return ok;
}

static int sg_line(int fd, const char *want)
{
	long h = 0;

	return ioctl(fd, SGIOCLINE, &h) == 0
			&& ((unsigned long) h & 0xFFFFFFFFUL) == fnv(want);
}

static void subghz_tests(void)
{
	unsigned long f[4];
	struct sg_air air;
	struct stat sb;
	int s, r, k, v, e, st, n;
	pid_t pid;
	char b[8];

	s = open("/dev/subghz", O_RDWR);
	r = open("/dev/subghzraw", O_RDWR);
	k = open("/dev/subghzpkt", O_RDWR);
	check(s >= 0 && r >= 0 && k >= 0 && stat("/dev/subghzpkt", &sb) == 0
		&& (sb.st_rdev & 0xFF) == 20, "subghz: open the 3 files (pkt: minor 20)");
	if (s < 0 || r < 0 || k < 0) return;
	f[0] = 350; f[1] = 700; f[2] = 350;
	e = write(r, (char *) f, 12) < 0 && errno == EIO && sg_tune(433920000UL);
	v = -1;
	e = e && ioctl(s, SGIOCGREGION, &v) == 0 && v == 0
		&& write(r, (char *) f, 12) < 0 && errno == EPERM;
	check(e, "subghz: untuned EIO; tuned, no region: EPERM");
	v = SG_REGION('X', 'X');
	e = ioctl(s, SGIOCREGION, &v) < 0 && errno == EINVAL;
	v = SG_REGION('E', 'U');
	e = e && ioctl(s, SGIOCREGION, &v) == 0;
	v = SG_REGION('U', 'S');
	e = e && ioctl(s, SGIOCREGION, &v) < 0 && errno == EPERM;
	v = 0;
	check(e && ioctl(s, SGIOCGREGION, &v) == 0 && v == SG_REGION('E', 'U'),
		"subghz: region XX EINVAL, EU set once (US: EPERM)");
	check(write(r, (char *) f, 12) == 12
		&& sg_line(r, "tx 433920000 3 350 -700 350"),
		"subghz: a frame goes out, its console line (sg> tx)");
	f[1] = 0;
	e = write(r, (char *) f, 12) < 0 && errno == EINVAL;
	f[1] = 2000000;
	e = e && write(r, (char *) f, 12) < 0 && errno == EINVAL;
	f[1] = 700;
	e = e && write(r, (char *) f, 10) < 0 && errno == EINVAL;
	/* 315 MHz is not EU's; 868.8 MHz: 0.1 % of an hour, 3.6 s */
	e = e && sg_tune(315000000UL) && write(r, (char *) f, 12) < 0
		&& errno == EPERM && write(k, "yo", 2) < 0 && errno == EPERM;
	f[0] = f[1] = f[2] = f[3] = 1000000;
	e = e && sg_tune(868800000UL) && ioctl(r, SGIOCAIR, &air) == 0
		&& air.limit_ms == 3600;
	check(e && write(r, (char *) f, 16) < 0 && errno == EAGAIN,
		"subghz: EINVAL (0 us, > 1 s, part words); EPERM at 315 MHz; "
		"EAGAIN for 4 s at 868.8 MHz (0.1 %)");

	/* a received frame for a waiting reader: another program (a reader
	 * is pinned while it waits, R4b.5, so a forked copy sharing our
	 * memory could not run beside it) */
	(void) sg_tune(433920000UL);
	fflush(stdout);
	if ((pid = fork()) == 0) {
		execl("/bin/m2test", "m2test", "sg-read", (char *) NULL);
		_exit(99);
	}
	f[0] = 500; f[1] = 1000; f[2] = 500; f[3] = 2000;
	for (n = 0, e = -1; n < 25 && e < 0; n++) {	/* until it reads */
		(void) poll((struct pollfd *) NULL, 0, 200);
		v = SG_FAKE_RAW;
		if (ioctl(r, SGIOCFAKE, &v) < 0) break;
		e = write(r, (char *) f, 16);
		if (e < 0 && errno != EAGAIN) break;
	}
	check(e == 16 && pid > 0 && waitpid(pid, &st, 0) == pid
		&& WIFEXITED(st) && WEXITSTATUS(st) == 0,
		"subghz: SGIOCFAKE: the reader gets the frame");

	/* packets */
	v = SG_FAKE_PKT;
	e = ioctl(r, SGIOCFAKE, &v) < 0 && errno == EINVAL
		&& write(k, "yo", 2) == 2 && sg_line(k, "pkt 433920000 79 6f")
		&& read(k, b, sizeof(b)) < 0 && errno == EAGAIN;
	check(e && ioctl(k, SGIOCFAKE, &v) == 0 && write(k, "yo", 2) == 2
		&& read(k, b, sizeof(b)) == 2 && b[0] == 'y' && b[1] == 'o'
		&& ioctl(r, SGIOCAIR, &air) == 0 && air.limit_ms == 360000
		&& air.used_ms > 0 && air.used_ms < 100,
		"subghz: a packet out (sg> pkt), none heard (EAGAIN), one "
		"heard (SGIOCFAKE); air time, 10 % limit");
	(void) sg_tune(0UL);
	close(s);
	close(r);
	close(k);
}
#endif

#ifdef BOARD_QEMU
/* Spec N.6 (the card pulled out), kernel part: while the card is out
 * every transfer fails and the console says so; another card is refused;
 * the same card back carries on.  QEMU's hook DIOCFAKECARD; raw reads at
 * offsets the FS cache does not hold. */
static void card_tests(void)
{
	static char b[512];
	int fd, st, r1, r2, r3;

	sync();
	if ((fd = open("/dev/hd0", O_RDONLY)) < 0) {
		check(0, "card: open /dev/hd0");
		return;
	}
	fflush(stdout);
	st = 1;
	if (ioctl(fd, DIOCFAKECARD, &st) != 0) {
		check(0, "card: DIOCFAKECARD");
		close(fd);
		return;
	}
	lseek(fd, 1L << 20, SEEK_SET);
	r1 = read(fd, b, sizeof b);
	st = 2;				/* another card */
	(void) ioctl(fd, DIOCFAKECARD, &st);
	lseek(fd, 2L << 20, SEEK_SET);
	r2 = read(fd, b, sizeof b);
	st = 0;				/* the original back */
	(void) ioctl(fd, DIOCFAKECARD, &st);
	lseek(fd, 3L << 20, SEEK_SET);
	r3 = read(fd, b, sizeof b);
	close(fd);
	check(r1 < 0, "card out: a read fails");
	check(r2 < 0, "card: another card is refused");
	check(r3 == (int) sizeof b, "card: the same card back carries on");
}

/* The root's s_flags on the disk (spec N.1), read through /dev/hd1 (the
 * same cache as the FS: what is on the card).  -1: not that device. */
static int root_sflags(void)
{
	static unsigned char b[1024];
	struct stat sr, sd;
	int fd, n;

	if (stat("/", &sr) != 0 || stat("/dev/hd1", &sd) != 0
					|| sd.st_rdev != sr.st_dev)
		return(-1);
	if ((fd = open("/dev/hd1", O_RDONLY)) < 0) return(-1);
	lseek(fd, 1024L, SEEK_SET);
	n = read(fd, b, sizeof b);
	close(fd);
	return(n == (int) sizeof b ? (b[18] | b[19] << 8) : -1);
}

static int fill_file(char *path, int c)
{
	static char b[1024];
	int fd, n;

	memset(b, c, sizeof b);
	if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
		return(-1);
	n = write(fd, b, sizeof b);
	close(fd);
	return(n == (int) sizeof b ? 0 : -1);
}

/* T17a: spec N.1, N.2 (CLEAN off before the first write, on again after a
 * quiet sync period; a read writes nothing) and the FS side of N.6 (a
 * dirty block written while the card is out stays in the cache and goes
 * to the card when it is back, not "Unrecoverable write error"). */
static void sflag_tests(void)
{
	static char b[1024];
	int fd, f0, f1, f2, st, i, n, same;

	if (fill_file("/tmp/n6", 'A') != 0) {
		check(0, "fs flags: write /tmp/n6");
		return;
	}
	f0 = root_sflags();
	sync();
	sync();
	f1 = root_sflags();
	if ((fd = open("/etc/motd", O_RDONLY)) >= 0) {
		while (read(fd, b, sizeof b) > 0) {}
		close(fd);
	}
	sync();
	sync();
	f2 = root_sflags();
	check(f0 >= 0 && !(f0 & 1), "fs flags: CLEAN off before the first write");
	check(f1 >= 0 && (f1 & 1), "fs flags: CLEAN again after a quiet sync");
	check(f2 >= 0 && (f2 & 1), "fs flags: a read writes nothing (no atime)");

	/* N.6: the card out while a dirty block is written. */
	n = fill_file("/tmp/n6", 'B');
	if (n != 0 || (fd = open("/dev/hd0", O_RDONLY)) < 0) {
		check(0, "card out: write /tmp/n6 again");
		return;
	}
	fflush(stdout);
	st = 1;
	(void) ioctl(fd, DIOCFAKECARD, &st);
	sync();				/* fails: the blocks stay dirty */
	st = 0;
	(void) ioctl(fd, DIOCFAKECARD, &st);
	close(fd);
	sync();				/* the card is back: written now */
	same = 0;
	if ((fd = open("/tmp/n6", O_RDONLY)) >= 0) {
		n = read(fd, b, sizeof b);
		for (i = 0; i < n && b[i] == 'B'; i++) {}
		same = (n == (int) sizeof b && i == n);
		close(fd);
	}
	check(same, "card out: a dirty block is kept until the card is back");
	unlink("/tmp/n6");
	sync();
}

/* R12.40: DIOCDISCARD on the whole disk's last sector, which no partition
 * or swap test uses.  QEMU's card zero-fills a discarded range, so the
 * pattern written first reads back as zeros; empty and too-long ranges
 * are refused.  /dev/hd0 goes through FS's cache: closing it (not
 * mounted) writes the pattern out and drops its blocks. */
static void discard_tests(void)
{
	static char b[512];
	struct partition pt;
	struct part_range rg;
	off_t at;
	int fd, i, zero, r0, r1;

	sync();
	if ((fd = open("/dev/hd0", O_RDWR)) < 0
	    || ioctl(fd, DIOCGETP, &pt) != 0) {
		check(0, "discard: open /dev/hd0");
		if (fd >= 0) close(fd);
		return;
	}
	rg.first = pt.size / 512 - 1;
	at = (off_t) rg.first * 512;
	memset(b, 0xA5, sizeof b);
	lseek(fd, at, SEEK_SET);
	(void) write(fd, b, sizeof b);
	close(fd);
	if ((fd = open("/dev/hd0", O_RDWR)) < 0) {
		check(0, "discard: open /dev/hd0 again");
		return;
	}
	rg.count = 1;
	r1 = ioctl(fd, DIOCDISCARD, &rg);
	lseek(fd, at, SEEK_SET);
	zero = read(fd, b, sizeof b) == (int) sizeof b;
	for (i = 0; zero && i < (int) sizeof b; i++) if (b[i] != 0) zero = 0;
	rg.count = 2;			/* one past the end */
	r0 = ioctl(fd, DIOCDISCARD, &rg);
	rg.count = 0;
	i = ioctl(fd, DIOCDISCARD, &rg);
	close(fd);
	check(r1 == 0, "discard: DIOCDISCARD of the last sector");
	check(zero, "discard: the range reads back as zeros (QEMU)");
	check(r0 < 0 && i < 0, "discard: empty and too-long ranges refused");
}


/* R7b.2: an alert preempts the screen and the buttons (FGIOCPREEMPT), a
 * reader waiting meanwhile is parked and ends empty at the release, a
 * second alert waits its turn (EBUSY), an alert that exits releases. */
static int rd_n(int fd)
{
	char c;
	struct pollfd p;

	p.fd = fd; p.events = POLLIN; p.revents = 0;
	if (poll(&p, 1, 2000) != 1 || read(fd, &c, 1) != 1) return -9;
	return c - '0';
}

static void preempt_body(void);

/* INIT (m2test here) may never be the foreground: the alert is a child,
 * which reports its failures in its exit status. */
static void preempt_tests(void)
{
	pid_t p;
	int st;

	fflush(stdout);
	if ((p = fork()) == 0) {
		fails = 0;
		preempt_body();
		fflush(stdout);
		_exit(fails);
	}
	check(p > 0 && waitpid(p, &st, 0) == p && WIFEXITED(st)
		&& WEXITSTATUS(st) == 0, "preempt: all of the above");
}

static void preempt_body(void)
{
	unsigned char m[2];
	int b, l, nb, pr[2], st, r, n;
	pid_t rd, c;

	b = open("/dev/buttons", O_RDWR);
	nb = open("/dev/buttons", O_RDONLY | O_NONBLOCK);
	l = open("/dev/lcd", O_RDWR);
	if (b < 0 || nb < 0 || l < 0 || pipe(pr) != 0) {
		check(0, "preempt: open");
		return;
	}
	furi_tap(b, 0x04);		/* the backlight on */
	while (read(nb, (char *) m, 2) == 2) ;
	fflush(stdout);
	if ((rd = fork()) == 0) {	/* a reader like the launcher */
		for (n = 0; n < 3; n++) {
			r = read(b, (char *) m, 2);
			m[0] = '0' + (r < 0 ? 7 : r);
			(void) write(pr[1], (char *) m, 1);
			if (r == 2) break;
		}
		_exit(0);
	}
	(void) poll((struct pollfd *) NULL, 0, 300);	/* it waits now */
	r = ioctl(l, FGIOCPREEMPT, (void *) 0);
	check(r == 0 && rd_n(pr[0]) == 0,
		"preempt: taken, the waiting reader gets an empty read");
	if ((c = fork()) == 0)
		_exit(ioctl(l, FGIOCPREEMPT, (void *) 0) < 0 && errno == EBUSY
			? 0 : 1);
	check(c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st)
		&& WEXITSTATUS(st) == 0, "preempt: a second alert gets EBUSY");
	furi_tap(b, 0x10);
	r = read(nb, (char *) m, 2);
	check(r == 2 && m[0] == 0x10 && m[1] == 1,
		"preempt: the alert gets the keys");
	while (read(nb, (char *) m, 2) == 2) ;
	r = ioctl(l, FGIOCRELEASE, (void *) 0);
	check(r == 0 && rd_n(pr[0]) == 0,
		"preempt: released, the parked reader gets an empty read");
	(void) poll((struct pollfd *) NULL, 0, 200);
	furi_tap(b, 0x20);
	check(rd_n(pr[0]) == 2 && waitpid(rd, &st, 0) == rd,
		"preempt: after the release the reader gets the next key");
	while (read(nb, (char *) m, 2) == 2) ;

	if ((c = fork()) == 0)		/* an alert that just exits */
		_exit(ioctl(l, FGIOCPREEMPT, (void *) 0) == 0 ? 0 : 1);
	check(c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st)
		&& WEXITSTATUS(st) == 0, "preempt: by a child that exits");
	(void) poll((struct pollfd *) NULL, 0, 100);
	r = ioctl(l, FGIOCPREEMPT, (void *) 0);
	check(r == 0 && ioctl(l, FGIOCRELEASE, (void *) 0) == 0,
		"preempt: its exit released the screen");

	/* The foreground app exits: the kernel clears it, so the next process
	 * in its slot does not seem to hold the screen (#192). */
	if ((c = fork()) == 0) {
		n = getpid();
		_exit(ioctl(l, FGIOCSET, &n) == 0 ? 0 : 1);
	}
	(void) waitpid(c, &st, 0);
	(void) poll((struct pollfd *) NULL, 0, 100);
	if ((c = fork()) == 0) {	/* B, likely in A's slot, starts C */
		if ((rd = fork()) == 0) {
			n = getpid();
			r = ioctl(l, FGIOCSET, &n);
			n = 0;
			(void) ioctl(l, FGIOCSET, &n);
			_exit(r == 0 ? 0 : 1);
		}
		_exit(waitpid(rd, &st, 0) == rd && WIFEXITED(st)
			? WEXITSTATUS(st) : 2);
	}
	check(c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st)
		&& WEXITSTATUS(st) == 0,
		"fg: an exited foreground app holds nothing");
	close(pr[0]); close(pr[1]);
	close(b); close(nb); close(l);
}
#endif

#ifdef BOARD_QEMU
/* R6.4: edges on header pins through poll() (QEMU: output ops set the
 * level the pin reads, so a child makes the edge). */
static void gpio_tests(void)
{
	unsigned char op[2];
	char v[16];
	struct pollfd p;
	struct tms tm;
	clock_t t0, t1;
	int g, g2, r, st;
	pid_t pid;

	if ((g = open("/dev/gpio", O_RDWR)) < 0) {
		check(0, "gpio: open");
		return;
	}
	op[0] = 3; op[1] = 0;			/* pin 3 low */
	(void) write(g, (char *) op, 2);
	op[1] = 6;				/* watch it */
	check(write(g, (char *) op, 2) == 2, "gpio: watch a pin (op 6)");
	p.fd = g; p.events = POLLIN; p.revents = 0;
	check(poll(&p, 1, 0) == 0, "gpio: no edge, not readable");
	fflush(stdout);
	if ((pid = fork()) == 0) {
		int c = open("/dev/gpio", O_WRONLY);

		(void) poll((struct pollfd *) NULL, 0, 200);
		op[0] = 3; op[1] = 1;
		_exit(c >= 0 && write(c, (char *) op, 2) == 2 ? 0 : 1);
	}
	t0 = times(&tm);
	r = poll(&p, 1, 3000);
	t1 = times(&tm);
	check(r == 1 && p.revents == POLLIN && t1 - t0 >= 9 && t1 - t0 < 60,
				"gpio: poll wakes on the edge");
	r = read(g, v, 16);
	check(r == 16 && v[3] == '1' && v[8 + 3] == '1',
				"gpio: read shows the level and the change");
	check(read(g, v, 16) == 16 && v[8 + 3] == '0' && poll(&p, 1, 0) == 0,
				"gpio: the read cleared the change");
	check(pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st)
		&& WEXITSTATUS(st) == 0, "gpio: the child (another opener) "
						"did not end the watch");
	close(g);			/* the watcher's close ends the watch */
	g2 = open("/dev/gpio", O_RDWR);
	p.fd = g2; p.revents = 0;
	check(g2 >= 0 && poll(&p, 1, 0) == 1,
		"gpio: closed by the watcher: no watch, readable at once");
	op[0] = 3; op[1] = 5;			/* the pin off again */
	if (g2 >= 0) {
		(void) write(g2, (char *) op, 2);
		close(g2);
	}
}
#endif

#ifdef BOARD_QEMU
/* R6.7 part 3: IBIOCEMU's rules (QEMU has no reader; the slots are the
 * host test's, tests/host/ibemutest.c): the emulating process owns the
 * contact, others get EBUSY, a master read or IBIOCRUN meanwhile EBUSY,
 * and its close ends it. */
static void ibemu_tests(void)
{
	static const unsigned char rom[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	struct ib_emu q;
	struct ib_run r;
	unsigned char buf[8];
	pid_t p;
	int fd, st, e1, e2;

	if ((fd = open("/dev/ibutton", O_RDWR)) < 0) {
		check(0, "ibemu: open");
		return;
	}
	memset(&q, 0, sizeof q);
	memcpy(q.rom, rom, 8);
	q.on = 1;
	q.served = 99;
	check(ioctl(fd, IBIOCEMU, &q) == 0 && q.served == 0,
		"ibemu: IBIOCEMU on, nobody served yet");
	e1 = read(fd, buf, 8) < 0 && errno == EBUSY;
	memset(&r, 0, sizeof r);
	r.op[0] = IB_RESET; r.nop = 1;
	e2 = ioctl(fd, IBIOCRUN, &r) < 0 && errno == EBUSY;
	check(e1 && e2, "ibemu: read and IBIOCRUN busy while emulating");
	if ((p = fork()) == 0) {		/* another process */
		int f = open("/dev/ibutton", O_RDWR);

		q.on = 1;
		_exit(f >= 0 && ioctl(f, IBIOCEMU, &q) < 0 && errno == EBUSY
			? 0 : 1);
	}
	check(p > 0 && waitpid(p, &st, 0) == p && WIFEXITED(st)
		&& WEXITSTATUS(st) == 0, "ibemu: another process gets EBUSY");
	close(fd);				/* ends the emulation */
	fd = open("/dev/ibutton", O_RDWR);
	check(fd >= 0 && read(fd, buf, 8) < 0 && errno == ENODEV,
		"ibemu: after close the contact reads again (no key: ENODEV)");
	if (fd >= 0) close(fd);
}

/* R6.7 part 2: IBIOCRUN, 1-Wire master steps (QEMU: nobody on the line,
 * it reads high). */
static void ibutton_tests(void)
{
	struct ib_run q;
	int fd = open("/dev/ibutton", O_RDWR), r;

	if (fd < 0) {
		check(0, "ibutton: open");
		return;
	}
	memset(&q, 0, sizeof q);
	q.op[0] = IB_RESET; q.op[1] = IB_WBYTE; q.op[2] = 0x33;
	q.op[3] = IB_RBYTE; q.op[4] = IB_W1;
	q.nop = 5;
	r = ioctl(fd, IBIOCRUN, &q);
	check(r == 0 && q.nout == 2 && q.out[0] == 0 && q.out[1] == 0xFF,
		"ibutton: IBIOCRUN reset (none), write, read (line high)");
	q.op[0] = 0x55; q.nop = 1;
	check(ioctl(fd, IBIOCRUN, &q) < 0 && errno == EINVAL,
		"ibutton: an unknown step -> EINVAL");
	q.op[0] = IB_WBYTE; q.nop = 1;		/* its value missing */
	check(ioctl(fd, IBIOCRUN, &q) < 0 && errno == EINVAL,
		"ibutton: a byte without its value -> EINVAL");
	q.nop = IB_MAXOP + 1;
	check(ioctl(fd, IBIOCRUN, &q) < 0 && errno == EINVAL,
		"ibutton: too many steps -> EINVAL");
	close(fd);
	ibemu_tests();
}
#endif

/* T21a / R5.6a: the launcher with icons, hints and its settings page,
 * driven by fake keys (AC5.4-AC5.7).  Its own apps and conf files in /tmp
 * (-a, -c).  Frames are read from /dev/lcd; FRAME lines in the log are
 * the frames for tools/frame2png.py. */
static unsigned char lfb[1024];

static int lpix(int x, int y)
{
	return lfb[(y >> 3) * 128 + x] >> (y & 7) & 1;
}

static void lgrab(int l, const char *name)
{
	int i;

	(void) poll((struct pollfd *) NULL, 0, 150);
	lseek(l, 0L, SEEK_SET);
	if (read(l, (char *) lfb, sizeof lfb) != sizeof lfb) memset(lfb, 0, 1024);
	printf("FRAME %s ", name);
	for (i = 0; i < 1024; i++) printf("%02x", lfb[i]);
	printf("\n");
}

/* the 6 rows of the 8x6 cell at x, y as bytes (MSB left) */
static void lcell(int x, int y, unsigned char *rows)
{
	int gx, gy;

	for (gy = 0; gy < 6; gy++) {
		rows[gy] = 0;
		for (gx = 0; gx < 8; gx++)
			rows[gy] |= (unsigned char) (lpix(x + gx, y + gy) << (7 - gx));
	}
}

/* T23b / spec K.8: a window app (gmdemo, libgm) in the launcher, driven
 * by fake keys: the desktop draws its windows, changes values, opens the
 * keyboard, keeps the parent, asks for a page; the app gets ok, submit,
 * back, page and close (its -l log).  ACK8.4: a deaf app (-n) gets the
 * busy mark after 150 ms, Back closes it at once, SIGTERM 2 s later.
 * ACK8.5: key -> frame and OK -> next window in QEMU (printed). */
static int has_line(const char *file, const char *want)
{
	FILE *f;
	char line[80];
	int ok = 0;

	if ((f = fopen(file, "r")) == NULL) return 0;
	while (fgets(line, sizeof line, f) != NULL)
		if (strncmp(line, want, strlen(want)) == 0) ok = 1;
	fclose(f);
	return ok;
}

/* A key down, ms until the screen changes (-1: not within 1 s), up.
 * Not inlined: its 2 KB of stack only while it runs (main's frame would
 * otherwise carry them and leave malloc 10000 no room). */
static long __attribute__((noinline)) key_frame(int b, int l, int key)
{
	unsigned char m = key, *before = lfb, now[1024];
	struct tms tm;
	clock_t t0, t1;
	long ms = -1;

	lseek(l, 0L, SEEK_SET);
	if (read(l, (char *) before, 1024) != 1024) return -1;
	t0 = times(&tm);
	(void) write(b, (char *) &m, 1);
	do {
		(void) poll((struct pollfd *) NULL, 0, 2);
		lseek(l, 0L, SEEK_SET);
		if (read(l, (char *) now, 1024) != 1024) break;
		t1 = times(&tm);
		if (memcmp(before, now, 1024) != 0) {
			ms = (long) (t1 - t0) * 1000 / CLK_TCK;
			break;
		}
	} while ((long) (t1 - t0) < CLK_TCK);
	(void) poll((struct pollfd *) NULL, 0, 20);
	m = 0;
	(void) write(b, (char *) &m, 1);
	(void) poll((struct pollfd *) NULL, 0, 60);
	return ms;
}

/* A quick key for the window test (the whole M2 run has 60 s): down,
 * up, and time for the desktop to draw. */
static void wtap(int b, int key)
{
	unsigned char m = key;

	(void) write(b, (char *) &m, 1);
	(void) poll((struct pollfd *) NULL, 0, 20);
	m = 0;
	(void) write(b, (char *) &m, 1);
	(void) poll((struct pollfd *) NULL, 0, 60);
}

static int hourglass(void)
{
	return !lpix(121, 1) && !lpix(125, 1) && !lpix(122, 2) && lpix(121, 2)
		&& !lpix(123, 3) && lpix(121, 3) && !lpix(121, 5);
}

static void __attribute__((noinline)) window_test(int b, int l)
{
	FILE *f;
	int i, ok;
	long ms, worst = 0;
	unsigned long sum;

	/* The launcher of launcher_test (one more launcher run would make
	 * the run-time cache copy it to flash: an erase over the M2 budget).
	 * Its list is on "Inor": OK there reloads the files below. */
	unlink("/tmp/gml");
	unlink("/tmp/gml2");
	f = fopen("/tmp/la", "w");
	fprintf(f, "Demo:root:@app:=/bin/gmdemo -l /tmp/gml -p 2\n"
		"Deaf:root::=/bin/gmdemo -n -l /tmp/gml2\n");
	fclose(f);
	f = fopen("/tmp/lc", "w");
	fprintf(f, "Icons: on\nHints: on\nBattery: off\nGuiLog: on\n");
	fclose(f);
	wtap(b, 0x10);					/* reload */
	(void) poll((struct pollfd *) NULL, 0, 300);
	wtap(b, 0x01);					/* Up: Demo */
	fflush(stdout);
	ms = key_frame(b, l, 0x10);			/* OK: Demo */
	printf("  K.8 start -> first window: %ld ms\n", ms);
	lgrab(l, "win-main");
	check(has_line("/tmp/gml", "start"), "window: the launcher starts the window app");
	ok = lpix(0, 0) && lpix(0, 8) && !lpix(0, 14) && lpix(0, 56) && lpix(127, 56);
	check(ok, "window: title bar, the first row selected, hint line (K.2 style)");
	for (sum = 0, i = 0; i < 1024; i++) sum = sum * 31 + lfb[i];
	wtap(b, 0x08);					/* Right: Volume 40 -> 45 */
	lgrab(l, "win-vol");
	{
		unsigned long s2 = 0;

		for (i = 0; i < 1024; i++) s2 = s2 * 31 + lfb[i];
		check(s2 != sum, "window: Right changes a value in the desktop");
	}
	worst = key_frame(b, l, 0x02);			/* Down: Name (ACK8.5) */
	ms = key_frame(b, l, 0x10);			/* OK: the keyboard */
	if (ms < 0 || ms > worst) worst = ms < 0 ? 99999 : ms;
	lgrab(l, "win-kb");
	ms = key_frame(b, l, 0x10);			/* types 'a' */
	if (ms < 0 || ms > worst) worst = ms < 0 ? 99999 : ms;
	wtap(b, 0x01);					/* Up: the command row */
	for (i = 0; i < 3; i++) wtap(b, 0x08);		/* Done */
	wtap(b, 0x10);
	ms = key_frame(b, l, 0x02);			/* Down: About */
	if (ms < 0 || ms > worst) worst = ms < 0 ? 99999 : ms;
	printf("  ACK8.5 key -> frame on a window app's list and keyboard: max %ld ms (4 keys)\n",
		worst);
	check(worst >= 0 && worst < 500, "ACK8.5: every key redrawn by the desktop (QEMU)");
	ms = key_frame(b, l, 0x10);			/* OK -> the text window */
	lgrab(l, "win-about");
	printf("  ACK8.5 OK -> next window: %ld ms\n", ms);
	check(ms >= 0, "ACK8.5: OK opens the next window");
	check(has_line("/tmp/gml", "value vol=45") && has_line("/tmp/gml", "value name=Wake upa")
		&& has_line("/tmp/gml", "ok about"),
		"window: submit with the changed values (keyboard text), then ok");
	ms = key_frame(b, l, 0x20);			/* Back: the kept parent */
	printf("  K.8 Back -> parent window: %ld ms\n", ms);
	check(has_line("/tmp/gml", "back about") && ms >= 0,
		"window: Back shows the parent at once, the app hears back");
	wtap(b, 0x02);					/* Down: Long list */
	wtap(b, 0x10);
	(void) poll((struct pollfd *) NULL, 0, 100);
	wtap(b, 0x02);
	wtap(b, 0x02);					/* past its 2-row page */
	(void) poll((struct pollfd *) NULL, 0, 100);
	lgrab(l, "win-page");
	check(has_line("/tmp/gml", "ok long") && has_line("/tmp/gml", "page 2"),
		"window: a long list asks for its next page");
	wtap(b, 0x20);					/* Back: main */
	wtap(b, 0x20);					/* Back: closes */
	(void) poll((struct pollfd *) NULL, 0, 100);
	check(has_line("/tmp/gml", "close"), "window: Back on the first window closes the app");

	/* ACK8.4: an app that never reads its events */
	wtap(b, 0x02);					/* Deaf */
	wtap(b, 0x10);
	(void) poll((struct pollfd *) NULL, 0, 200);
	wtap(b, 0x02);
	wtap(b, 0x02);					/* About */
	wtap(b, 0x10);					/* ok: no answer */
	(void) poll((struct pollfd *) NULL, 0, 200);
	lgrab(l, "deaf-busy");
	check(hourglass(), "ACK8.4: the busy mark after 150 ms");
	ms = key_frame(b, l, 0x01);			/* the desktop still moves */
	check(ms >= 0 && ms < 500, "ACK8.4: keys still work while the app is busy");
	ms = key_frame(b, l, 0x20);			/* Back: closed at once */
	printf("  ACK8.4 Back on a deaf app -> launcher: %ld ms\n", ms);
	lgrab(l, "deaf-closed");
	check(ms >= 0 && ms < 500 && !hourglass(), "ACK8.4: Back closes it at once");
	check(!has_line("/tmp/gml2", "term"), "ACK8.4: no SIGTERM before 2 s");
	(void) poll((struct pollfd *) NULL, 0, 2300);
	check(has_line("/tmp/gml2", "term"), "ACK8.4: SIGTERM 2 s after Back");
	(void) poll((struct pollfd *) NULL, 0, 200);	/* the launcher reaps it */
}

/* T23a / spec K.6: the desktop in the launcher.  ACK.6: a Left+Up chord
 * held 1.5 s keeps APPS, a short Left turns the page on its release.
 * ACK.1: a shell module of three lines (guimod) puts a tile on HOME and
 * changes it; ACK.2: killed, its tile goes and the desktop keeps working;
 * ACK.3: four modules writing as fast as they can, key -> frame on the
 * desktop (QEMU: < 500 ms, as ACK8.5; L1 itself on the device).  The
 * launcher of launcher_test, on its APPS page. */
static unsigned long lsum(int y0, int y1)
{
	unsigned long sum = 0;
	int x, y;

	for (y = y0; y <= y1; y++)
		for (x = 0; x < 128; x++) sum = sum * 31 + (unsigned long) lpix(x, y);
	return sum;
}

static unsigned long tsum(void)		/* the page's name on the title */
{
	unsigned long sum = 0;
	int x, y;

	for (y = 1; y <= 5; y++)
		for (x = 40; x < 88; x++) sum = sum * 31 + (unsigned long) lpix(x, y);
	return sum;
}

static int row_text(int i)		/* row i of a page has text */
{
	int x, y;

	for (y = 7 + 6 * i; y < 12 + 6 * i; y++)
		for (x = 6; x < 60; x++)
			if (lpix(x, y) != lpix(0, y)) return 1;
	return 0;
}

static int sectors_written(void);

static void __attribute__((noinline)) desktop_test(int b, int l, pid_t lpid)
{
	FILE *f;
	unsigned char m;
	unsigned long apps, s1;
	int p[2], i, st, ok;
	long ms, worst = 0;
	pid_t mod, w[4];
	struct stat sb;
	int s0;

	ok = stat("/var/run/gui", &sb) == 0 && S_ISFIFO(sb.st_mode)
		&& (sb.st_mode & 07777) == 0620 && sb.st_uid == 0 && sb.st_gid == 10;
	check(ok, "desktop: /var/run/gui is a FIFO, root's, 0620, group gui");
	lgrab(l, "desk-apps");
	apps = tsum();
	/* ACK.6 (Up still moves the selection: it is not a page key) */
	m = 0x05;
	(void) write(b, (char *) &m, 1);
	(void) poll((struct pollfd *) NULL, 0, 1500);
	m = 0;
	(void) write(b, (char *) &m, 1);
	(void) poll((struct pollfd *) NULL, 0, 200);
	lgrab(l, "ack6-chord");
	check(tsum() == apps, "ACK.6: Left+Up held 1.5 s: the page stays APPS");
	m = 0x04;
	(void) write(b, (char *) &m, 1);
	(void) poll((struct pollfd *) NULL, 0, 100);
	lgrab(l, "ack6-press");
	ok = tsum() == apps;
	m = 0;
	(void) write(b, (char *) &m, 1);
	(void) poll((struct pollfd *) NULL, 0, 100);
	lgrab(l, "ack6-release");
	check(ok && tsum() != apps,
		"ACK.6: a short Left turns to SETTINGS on its release, not its press");
	wtap(b, 0x20);					/* Back: APPS */

	/* ACK.1, ACK.2 */
	f = fopen("/tmp/ack1.sh", "w");
	fprintf(f, "guimod tile ack Kaixo mundua\nread x\n"
		"guimod tile ack Agur; read x\n");
	fclose(f);
	if (pipe(p) < 0) { check(0, "desktop: pipe"); return; }
	fflush(stdout);
	if ((mod = fork()) == 0) {
		dup2(p[0], 0);
		close(p[0]);
		close(p[1]);
		execl("/bin/sh", "sh", "/tmp/ack1.sh", (char *) NULL);
		_exit(99);
	}
	close(p[0]);
	wtap(b, 0x08);					/* Right: HOME */
	(void) poll((struct pollfd *) NULL, 0, 600);
	lgrab(l, "ack1-tile");
	s1 = lsum(7, 12);
	ok = lpix(0, 7) && row_text(0) && !row_text(1);
	(void) write(p[1], "\n", 1);			/* the module goes on */
	(void) poll((struct pollfd *) NULL, 0, 600);
	lgrab(l, "ack1-changed");
	check(ok && lpix(0, 7) && lsum(7, 12) != s1 && !row_text(1),
		"ACK.1: a 3-line shell module puts a tile on HOME and changes it");
	kill(mod, SIGKILL);
	(void) waitpid(mod, &st, 0);
	close(p[1]);
	ms = key_frame(b, l, 0x02);			/* any key: it is seen */
	lgrab(l, "ack2-gone");
	check(ms >= 0 && !lpix(0, 7) && kill(lpid, 0) == 0,
		"ACK.2: the killed module's tile goes, the desktop keeps working");

	/* ACK.3: four modules as fast as they can, keys on APPS */
	wtap(b, 0x04);					/* Left: APPS */
	(void) sectors_written();	/* ACK.5: ACK.1's file, the clean mark */
	s0 = sectors_written();
	fflush(stdout);
	for (i = 0; i < 4; i++)
		if ((w[i] = fork()) == 0) {
			char rec[64];
			int fd = open("/var/run/gui", O_WRONLY | O_NONBLOCK), n;
			long k;

			for (k = 0; fd >= 0; k++) {
				n = sprintf(rec, "tile\tw%d\t0\t%d\tflood %ld\n", i,
					    (int) getpid(), k);
				(void) write(fd, rec, (size_t) n);
			}
			_exit(0);
		}
	(void) poll((struct pollfd *) NULL, 0, 200);
	for (i = 0; i < 4; i++) {
		ms = key_frame(b, l, i & 1 ? 0x01 : 0x02);
		if (ms < 0 || ms > worst) worst = ms < 0 ? 99999 : ms;
	}
	printf("  ACK.3 key -> frame on APPS, 4 modules flooding: max %ld ms (4 keys)\n",
		worst);
	check(worst >= 0 && worst < 500, "ACK.3: keys redrawn while 4 modules flood (QEMU)");
	wtap(b, 0x08);					/* Right: HOME */
	ms = key_frame(b, l, 0x02);
	printf("  ACK.3 key -> frame on HOME, 4 modules flooding: %ld ms\n", ms);
	lgrab(l, "ack3-home");
	check(ms >= 0 && ms < 500 && row_text(0) && row_text(3) && !row_text(4),
		"ACK.3: HOME has the 4 modules' tiles, keys still move");
	for (i = 0; i < 4; i++)		/* not kill(-1): a fork that failed */
		if (w[i] > 0) kill(w[i], SIGKILL);
	for (i = 0; i < 4; i++) if (w[i] > 0) (void) waitpid(w[i], &st, 0);
	(void) poll((struct pollfd *) NULL, 0, 100);
	s0 = sectors_written() - s0;
	printf("  ACK.5 4 modules flooding /var/run/gui, then sync: %d sectors"
		" written\n", s0);
	check(s0 == 0, "ACK.5: FIFO records never reach the card (K.6)");
	wtap(b, 0x02);
	lgrab(l, "ack3-gone");
	check(!row_text(0), "ACK.2: four killed modules: their tiles go");

	/* a notification: the unread count on the status line, the text on
	 * the message line */
	wtap(b, 0x04);					/* APPS */
	(void) poll((struct pollfd *) NULL, 0, 100);
	lgrab(l, "desk-before-note");
	s1 = lsum(0, 6);
	apps = lsum(57, 63);
	{	/* as guimod writes it (guimod runs only twice here: a third
		 * run would make the run-time cache copy it to flash) */
		static const char rec[] = "notify\tmail\t0\t1\tKaixo\n";
		int fd = open("/var/run/gui", O_WRONLY | O_NONBLOCK);

		(void) write(fd, rec, sizeof rec - 1);
		close(fd);
	}
	(void) poll((struct pollfd *) NULL, 0, 300);
	lgrab(l, "desk-note");
	check(lsum(0, 6) != s1 && lsum(57, 63) != apps,
		"desktop: a notification: the count on the status line, the text noted");
}

/* T24b, T24d / spec K.7, K.8: settings time from the launcher's SETTINGS
 * page, the window app setgroup (the launcher draws it), run once here
 * and once for Display (more runs would make the run-time cache copy
 * setgroup to flash: an erase over the M2 budget).  ACK7.2: opening and
 * leaving the group writes 0 sectors (QEMU's WINCHESTER counter,
 * DIOCFAKECARD 3): a minute moved goes to the RTC only, the zone list
 * opened and left is no change.  ACK7.4: free RAM while the group is open
 * against the launcher alone, without its text (in RAM here, in flash on
 * the device: base flash or the run-time cache).  The clock is put back
 * afterwards. */
static int sectors_written(void)
{
	int fd, n;

	sync();
	if ((fd = open("/dev/hd0", O_RDONLY)) < 0) return -1;
	n = 3;
	if (ioctl(fd, DIOCFAKECARD, &n) != 0) n = -1;
	close(fd);
	return n;
}

static long free_ram(void)
{
	message m;

	m.m1_p1 = NULL;
	m.m1_i1 = 0;
	m.m1_i2 = 0;
	return (long) _syscall(MM, MEMINFO, &m);
}

/* #271: pkg install must not exit 0 unless its postinst ran (and ended
 * with 0); else it exits 1 and says why.  Under load the exec of the
 * postinst's sh failed with EAGAIN (no memory hole) in pkg's vfork child,
 * silently: pkg said "installed".  The package is made here (ustar, as
 * tools/mkpkg.py makes them); its postinst appends a line to PP_RAN, or
 * exits 3 while PP_FAIL exists.  Then installs while a filler holds all
 * the memory there is for a while: each must either have run its postinst
 * or exit 1 with a "pkg: " reason; pkg tries an exec that found no hole
 * again (1 s apart), so they should all get through. */
#define PP_TAR	"/tmp/pkgpost.tar"
#define PP_RAN	"/tmp/pkgpost.ran"
#define PP_FAIL	"/tmp/pkgpost.fail"
#define PP_ERR	"/tmp/pkgpost.err"
#define PP_LOAD	2		/* installs under load (M2 has 90 s) */

/* The strings of the pkg test, in one const struct (flash): with -fpic
 * every string literal has its own GOT word, in each process's data; the
 * pkg test's 44 put every copy of m2test one 256 B click over, and one of
 * ACK.3's 4 flooders could not fork. */
#define PK_STRINGS(S) \
	S(o7, "%07o") \
	S(o11, "%011o") \
	S(o6, "%06o") \
	S(ustar, "ustar") \
	S(v00, "00") \
	S(bin, "bin") \
	S(oper, "operator") \
	S(err, PP_ERR) \
	S(tar, PP_TAR) \
	S(ran, PP_RAN) \
	S(fail, PP_FAIL) \
	S(pkgbin, "/bin/pkg") \
	S(said, "  pkg said: %s%s") \
	S(nl, "\n") \
	S(scratch, "/usr/var/pkg/tmp") \
	S(sleepbin, "/bin/sleep") \
	S(s60, "60") \
	S(f, "f") \
	S(control, "Package: pkgpost\nVersion: 1.0\n" "Abi: any\nDescription: M2 test of postinst (#271)\n") \
	S(postinst, "echo ran >> " PP_RAN "\n" "test -f " PP_FAIL " && exit 3\nexit 0\n") \
	S(var, "/usr/var") \
	S(varpkg, "/usr/var/pkg") \
	S(info, "/usr/var/pkg/info") \
	S(cmake, "pkg #271: make the test package") \
	S(mctl, "CONTROL/control") \
	S(mpost, "CONTROL/postinst") \
	S(pf, "/tmp/pkgpost.f") \
	S(install, "install") \
	S(installed, "installed") \
	S(cinst, "pkg #271: install runs the postinst, exit 0") \
	S(sectors, "  pkg install: %d card sectors written\n") \
	S(failed, "pkg: pkgpost: postinst failed: exit 3") \
	S(notset, "is not set up") \
	S(cfail, "pkg #271: a postinst that fails: exit 1, says why") \
	S(pkgc, "pkg: ") \
	S(loadbad, "  pkg install under load: exit %d, postinst " "lines %d -> %d, %d left in the scratch " "directory\n") \
	S(started, "  pkg install under load: %d of %d started\n") \
	S(cload, "pkg #271: under load, exit 0 only " "with the postinst run, else a reason") \
	S(remove, "remove") \
	S(pkgpost, "pkgpost") \
	S(cremove, "pkg #271: remove") \

#define PK_DECL(n, s)	char n[sizeof s];
#define PK_INIT(n, s)	s,
static const struct { PK_STRINGS(PK_DECL) } pk = { PK_STRINGS(PK_INIT) };

/* A member: data, or (data NULL) n bytes of 'x'. */
static void tar_member(int fd, const char *name, const char *data, int n,
								int mode)
{
	char h[512];
	unsigned sum = 0;
	int i;

	if (data != NULL) n = (int) strlen(data);
	memset(h, 0, sizeof h);
	strcpy(h, name);
	sprintf(h + 100, pk.o7, mode);
	sprintf(h + 108, pk.o7, 2);		/* bin */
	sprintf(h + 116, pk.o7, 0);		/* operator */
	sprintf(h + 124, pk.o11, n);
	sprintf(h + 136, pk.o11, 0);
	h[156] = '0';
	memcpy(h + 257, pk.ustar, 6);
	memcpy(h + 263, pk.v00, 2);
	strcpy(h + 265, pk.bin);
	strcpy(h + 297, pk.oper);
	memset(h + 148, ' ', 8);
	for (i = 0; i < 512; i++) sum += (unsigned char) h[i];
	sprintf(h + 148, pk.o6, sum);		/* then NUL, ' ' */
	write(fd, h, sizeof h);
	memset(h, 0, sizeof h);
	for (i = 0; i < n; i += 512) {
		if (data != NULL)
			memcpy(h, data + i, n - i < 512 ? n - i : 512);
		else memset(h, 'x', n - i < 512 ? n - i : 512);
		write(fd, h, sizeof h);
		memset(h, 0, sizeof h);
	}
}

/* pkg op arg in a child; its stderr (and stdout if out < 0) to PP_ERR. */
static pid_t pkg_start(const char *op, const char *arg, int out)
{
	pid_t pid;
	int fd;

	fflush(stdout);
	if ((pid = fork()) != 0) return pid;	/* < 0: no memory */
	if ((fd = creat(pk.err, 0644)) >= 0) {
		dup2(out >= 0 ? out : fd, 1);
		dup2(fd, 2);
		close(fd);
	}
	if (out >= 0) close(out);
	execl(pk.pkgbin, pk.pkgbin + 5, op, arg, (char *) NULL);
	_exit(errno == EAGAIN ? 126 : 127);
}

static int pkg_wait(pid_t pid)
{
	int st;

	if (pid < 0) return -2;			/* not started */
	if (waitpid(pid, &st, 0) != pid) return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int pkg_run(const char *op, const char *arg)
{
	return pkg_wait(pkg_start(op, arg, -1));
}

static int file_lines(const char *path)
{
	char buf[64];
	int fd, n, i, lines = 0;

	if ((fd = open(path, O_RDONLY)) < 0) return 0;
	while ((n = read(fd, buf, sizeof buf)) > 0)
		for (i = 0; i < n; i++) lines += buf[i] == '\n';
	close(fd);
	return lines;
}

static int err_has(const char *want)
{
	char buf[256];
	int fd, n;

	if ((fd = open(pk.err, O_RDONLY)) < 0) return 0;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	buf[n > 0 ? n : 0] = 0;
	return strstr(buf, want) != NULL;
}

static void err_show(void)
{
	char buf[256];
	int fd, n;

	if ((fd = open(pk.err, O_RDONLY)) < 0) return;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	buf[n > 0 ? n : 0] = 0;
	printf(pk.said, buf,
			n > 0 && buf[n - 1] == '\n' ? pk.nl + 1 : pk.nl);
}

/* Entries left in pkg's scratch directory (cleanup: rm -r). */
static int scratch_left(void)
{
	DIR *d;
	struct dirent *e;
	int n = 0;

	if ((d = opendir(pk.scratch)) == NULL) return 0;
	while ((e = readdir(d)) != NULL) n += e->d_name[0] != '.';
	closedir(d);
	return n;
}

/* Fills the memory there is, writes a byte to ready, holds the memory ms
 * milliseconds, gives it back.  Copies of m2test take the big holes, then
 * /bin/sleep (~7 KB) the rest and, every 10 ms, what other programs give
 * back.  Runs in its own child (pid returned). */
static pid_t filler(int ms, int ready)
{
	pid_t pid, kids[48];
	volatile int err;
	struct tms tm;
	clock_t t0;
	int n = 0, said = 0;

	fflush(stdout);
	if ((pid = fork()) != 0) return pid;
	while (n < 48 && (kids[n] = fork()) >= 0) {
		if (kids[n] == 0) { for (;;) pause(); }
		n++;
	}
	t0 = times(&tm);
	do {				/* and what others give back */
		while (n < 48) {
			err = 0;
			if ((pid = vfork()) < 0) break;
			if (pid == 0) {
				execl(pk.sleepbin, pk.sleepbin + 5, pk.s60,
							(char *) NULL);
				err = errno != 0 ? errno : ENOEXEC;
				_exit(127);
			}
			if (err != 0) { waitpid(pid, NULL, 0); break; }
			kids[n++] = pid;
		}
		if (!said) {
			said = 1;
			write(ready, pk.f, 1);
		}
		poll(NULL, 0, 10);
	} while ((long) (times(&tm) - t0) < (long) ms * HZ / 1000);
	while (n > 0) { kill(kids[--n], SIGKILL); wait(NULL); }
	_exit(0);
}

/* The run-time cache's erase budget (R2b.11): set it (>= 0; 0: no
 * caching), return the one before.  The pkg test runs pkg, tar, sh and
 * sleep more than twice: cached, they would erase flash pages the M2
 * budget does not have. */
static long fc_budget(int pages)
{
	message m;
	struct fccount fc;

	m.m1_p1 = (char *) &fc;
	m.m1_i1 = pages;
	m.m1_i2 = MI_FCBUDGET;
	if (_syscall(MM, MEMINFO, &m) < 0) return -1;
	return (long) fc.budget;
}

static void __attribute__((noinline)) pkg_tests(void)
{
	int fd, st, i, ok, before, started = 0, bad = 0, s0, s1;
	pid_t f, f2;

	fc_budget(0);
	mkdir(pk.var, 0755);
	mkdir(pk.varpkg, 0755);
	mkdir(pk.info, 0755);
	unlink(pk.ran);
	unlink(pk.fail);
	if ((fd = creat(pk.tar, 0644)) < 0) {
		check(0, pk.cmake);
		return;
	}
	tar_member(fd, pk.mctl, pk.control, 0, 0644);
	tar_member(fd, pk.mpost, pk.postinst, 0, 0755);
	tar_member(fd, pk.pf + 1, NULL, 600, 0644);
	{	char z[1024];
		memset(z, 0, sizeof z);
		write(fd, z, sizeof z);
	}
	close(fd);

	s0 = sectors_written();
	st = pkg_run(pk.install, pk.tar);
	s1 = sectors_written();
	ok = st == 0 && file_lines(pk.ran) == 1 && err_has(pk.installed);
	check(ok, pk.cinst);
	if (!ok) err_show();
	if (s0 >= 0 && s1 >= 0)
		printf(pk.sectors, s1 - s0);

	close(creat(pk.fail, 0644));
	st = pkg_run(pk.install, pk.tar);
	ok = st == 1 && file_lines(pk.ran) == 2
		&& err_has(pk.failed)
		&& err_has(pk.notset);
	check(ok, pk.cfail);
	if (!ok) err_show();
	unlink(pk.fail);

	/* Under load, the moment of #271: pkg has unpacked the package and
	 * is about to start rm (its cleanup) and sh (the postinst), with no
	 * memory hole for them.  pkg's stdout is a full pipe: it stops in
	 * the fflush() before those, after tar.  Then the filler takes all
	 * the memory there is and the pipe is read: rm and sh find no hole
	 * (exec: EAGAIN) until the filler gives the memory back 1.5 s later.
	 * Before #271 pkg said "installed", exit 0, no postinst run. */
	for (i = 0; i < PP_LOAD; i++) {
		int p[2], q[2];
		char c[64];

		before = file_lines(pk.ran);
		if (pipe(p) < 0) break;
		if (pipe(q) < 0) { close(p[0]); close(p[1]); break; }
		fcntl(p[1], F_SETFL, O_NONBLOCK);
		while (write(p[1], c, sizeof c) == (int) sizeof c) ;
		while (write(p[1], c, 1) == 1) ;	/* full */
		fcntl(p[1], F_SETFL, 0);
		f = pkg_start(pk.install, pk.tar, p[1]);
		close(p[1]);
		poll(NULL, 0, 300);		/* pkg: tar, then stops */
		f2 = filler(1500, q[1]);
		close(q[1]);
		read(q[0], c, 1);		/* memory full */
		close(q[0]);
		while (read(p[0], c, sizeof c) > 0) ;	/* pkg goes on */
		close(p[0]);
		st = pkg_wait(f);
		if (f2 > 0) waitpid(f2, NULL, 0);
		if (st == -2 || st == 126) continue;	/* pkg did not start */
		started++;
		if ((st == 0 ? file_lines(pk.ran) != before + 1
			     : !err_has(pk.pkgc)) || scratch_left() != 0) {
			bad++;
			printf(pk.loadbad, st, before, file_lines(pk.ran),
				scratch_left());
			err_show();
		}
	}
	printf(pk.started, started,
								PP_LOAD);
	check(started > 0 && bad == 0, pk.cload);

	st = pkg_run(pk.remove, pk.pkgpost);
	check(st == 0 && access(pk.pf, F_OK) < 0,
						pk.cremove);
	if (st != 0) err_show();
	unlink(pk.tar);
	unlink(pk.ran);
	unlink(pk.err);
	/* The budget stays 0: MM copies a program to the cache in the
	 * background, after its 3rd run - it would still do so for pkg,
	 * tar, sh and sleep if the budget came back now. */
}

/* What the window's two pipes cost on the card (s0: the counter now):
 * nothing since K.6 "pipe data never reaches the card" (T23a's FS part):
 * a pipe's inode and zones are in core only, no bitmap is written.  It
 * was 6 sectors before (an inode bit and zone bits, T9). */
static int pipe_sectors(int s0)
{
	int p[2], q[2];
	char c[8];

	if (pipe(p) < 0) return -1;
	if (pipe(q) < 0) { close(p[0]); close(p[1]); return -1; }
	(void) write(p[1], "ok\tx\n", 5);
	(void) write(q[1], "show\n", 5);
	(void) read(p[0], c, sizeof c);
	(void) read(q[0], c, sizeof c);
	close(p[0]); close(p[1]); close(q[0]); close(q[1]);
	return sectors_written() - s0;
}

/* a_text of a program (its a.out header), -1 */
static long text_size(const char *path)
{
	unsigned char h[12];
	int fd, n = 0;

	if ((fd = open(path, O_RDONLY)) < 0) return -1;
	n = read(fd, (char *) h, sizeof h);
	close(fd);
	if (n != (int) sizeof h || h[0] != 0x01 || h[1] != 0x03) return -1;
	return (long) h[8] | (long) h[9] << 8 | (long) h[10] << 16
		| (long) h[11] << 24;
}

static void __attribute__((noinline)) settings_test(int b, int l)
{
	struct stat st;
	struct tms tm;
	clock_t c0;
	time_t t0, t1, t2;
	long f0, ft, text;
	int z0, s0, s1, i, tz0, conf0, ok, up, ps;

	tz0 = stat("/etc/TZ", &st) == 0;
	conf0 = stat("/etc/flipper/time.conf", &st) == 0;
	(void) poll((struct pollfd *) NULL, 0, 300);
	f0 = free_ram();
	z0 = sectors_written();			/* a sync of its own costs */
	s0 = sectors_written();
	ps = pipe_sectors(s0);
	s0 = sectors_written();
	{	/* MM: "memuse:" at setgroup's exit (its stack never used) */
		message m;

		m.m1_i1 = 1;
		m.m1_i2 = MI_TRACE;
		(void) _syscall(MM, MEMINFO, &m);
	}
	wtap(b, 0x04);				/* SETTINGS */
	for (i = 0; i < 3; i++) wtap(b, 0x02);	/* Display Sound LED Time */
	wtap(b, 0x10);				/* OK: setgroup time */
	(void) poll((struct pollfd *) NULL, 0, 800);
	ft = free_ram();
	lgrab(l, "time");
	ok = lpix(0, 0) && lpix(0, 8) && lpix(0, 56);
	check(ok, "settings time: the desktop draws it: title, the first row selected, hint line");
	wtap(b, 0x02);				/* Time */
	wtap(b, 0x10);				/* OK: edit, the hours */
	wtap(b, 0x08);				/* Right: the minutes */
	t0 = time((time_t *) 0);
	up = t0 / 60 % 60 != 59;		/* no wrap to the hour's :00 */
	c0 = times(&tm);
	wtap(b, up ? 0x01 : 0x02);		/* Up: +1 minute (59: Down) */
	lgrab(l, "time-edit");
	wtap(b, 0x10);				/* OK: done, sent at once (LIVE) */
	(void) poll((struct pollfd *) NULL, 0, 300);
	t1 = time((time_t *) 0);
	wtap(b, 0x02);				/* Zone */
	wtap(b, 0x10);				/* OK: the zone list */
	(void) poll((struct pollfd *) NULL, 0, 300);
	lgrab(l, "time-zones");
	ok = lpix(0, 0) && lpix(0, 8) && lpix(0, 56);
	check(ok, "settings time: Zone opens the list of zones");
	wtap(b, 0x20);				/* Back: Time again, no zone */
	wtap(b, 0x20);				/* Back: nothing to write */
	(void) poll((struct pollfd *) NULL, 0, 500);
	s1 = sectors_written();
	t2 = t0 + (time_t) ((times(&tm) - c0) / CLK_TCK);
	(void) stime(&t2);			/* the clock back */
	{
		message m;

		m.m1_i1 = 0;
		m.m1_i2 = MI_TRACE;
		(void) _syscall(MM, MEMINFO, &m);
	}
	wtap(b, 0x20);				/* SETTINGS -> list */
	printf("  settings time: %s on the minutes: %ld -> %ld\n",
		up ? "Up" : "Down", (long) t0, (long) t1);
	/* (the minute shown came at the window's start: it may have turned
	 * since, so the new time is the minute after, or this one) */
	check(t1 % 60 <= 3 && t1 - t0 <= 66 && t0 - t1 <= 125,
		"settings time: a new minute sets the clock at once (seconds 0)");
	printf("  ACK7.2 settings time, no saved change: %d sectors written,"
		" the window's pipes alone %d (the probe's own sync: %d)\n",
		s1 - s0, ps, s0 - z0);
	check(z0 >= 0 && ps == 0 && s1 - s0 == 0,
		"ACK7.2: a group opened and left without a change writes 0 sectors"
		" (its K.8 pipes too, K.6)");
	check((stat("/etc/TZ", &st) == 0) == tz0
		&& (stat("/etc/flipper/time.conf", &st) == 0) == conf0,
		"ACK7.2: no /etc/TZ, no time.conf written");
	text = text_size("/bin/setgroup");
	if (text > 0) text = (text + 255) & ~255L;	/* MM's 256 B clicks */
	printf("  ACK7.4 free while settings time is open: %ld B more in use"
		" than the launcher alone, %ld B without its text (%ld B in clicks)\n",
		f0 - ft, f0 - ft - text, text);
	check(text > 0 && f0 - ft - text > 0 && f0 - ft - text <= 3072,
		"ACK7.4: an open group takes <= 3 KB more than the launcher (text in flash)");
	{
		message m;
		char id[24];

		m.m1_p1 = id;
		m.m1_i1 = (int) sizeof id;
		m.m1_i2 = MI_BUILD;
		id[0] = 0;
		ok = _syscall(MM, MEMINFO, &m) == 0;
		id[sizeof id - 1] = 0;
		printf("  MI_BUILD: %s\n", id);
		check(ok && id[0] >= '0' && id[0] <= '9' && strchr(id, '.') != NULL,
			"MI_BUILD: the image's version and build id (settings about)");
	}
}

static void launcher_test(void)
{
	static const unsigned char bell[6] = { 0x18, 0x3C, 0x3C, 0x7E, 0x00, 0x18 };
	unsigned char c[6];
	struct stat s1, s2;
	FILE *f;
	char line[40];
	int b, l, i, st, ok, rule, blank;
	pid_t pid;

	f = fopen("/tmp/la", "w");
	fprintf(f, "Kanpaia:root:#183C3C7E0018:/bin/true\n"
		"Ordua:root:@clock:/bin/true\nKaixo:root:Zz:/bin/true\n"
		"Hutsa:root::/bin/true\nEz:root:@nosuch:/bin/true\n"
		"Txarra:root:#12XY:/bin/true\nGezi:root:>_:/bin/true\n"
		"Bat:root:!:/bin/true\nZaharra:root:/bin/true\n"
		"Inor:nosuchuser:@bell:/bin/true\n"
		"K:root:@app:/bin/true\nL:root:@app:/bin/true\n");
	fclose(f);
	f = fopen("/tmp/lc", "w");
	fprintf(f, "Icons: on\nHints: on\n");
	fclose(f);
	b = open("/dev/buttons", O_WRONLY);
	l = open("/dev/lcd", O_RDWR);
	if (b < 0 || l < 0) { check(0, "launcher: open lcd/buttons"); return; }
	furi_tap(b, 0x20);	/* the backlight on (Back: no page action) */
	fflush(stdout);
	if ((pid = fork()) == 0) {
		execl("/bin/launcher", "launcher", "-a", "/tmp/la", "-c", "/tmp/lc",
			(char *) NULL);
		_exit(99);
	}
	(void) poll((struct pollfd *) NULL, 0, 600);
	lgrab(l, "list");
	lcell(6, 7, c);
	for (i = 0; i < 6; i++) c[i] = (unsigned char) ~c[i];	/* selected */
	check(memcmp(c, bell, 6) == 0, "launcher: # bitmap icon at x 6-13, inverted on the selected row");
	lcell(6, 13, c);
	check(c[0] | c[1] | c[2] | c[3] | c[4] | c[5], "launcher: @clock drawn");
	blank = 1;
	for (i = 3; i <= 5; i++) {		/* Hutsa, Ez, Txarra: no icon */
		lcell(6, 7 + 6 * i, c);
		if (c[0] | c[1] | c[2] | c[3] | c[4] | c[5]) blank = 0;
	}
	check(blank, "launcher: empty, unknown and malformed icons: a blank cell");
	for (rule = 1, i = 0; i < 128; i++) if (!lpix(i, 56)) rule = 0;
	check(rule && lpix(127, 6), "launcher: hint line rule at y 56, scroll mark (12 entries)");
	for (i = 0; i < 7; i++) furi_tap(b, 0x02);	/* Down to the 8th */
	lgrab(l, "list-8");
	for (i = 0; i < 2; i++) furi_tap(b, 0x02);	/* "Inor": unknown user */
	furi_tap(b, 0x10);
	lgrab(l, "note");
	ok = 0;
	for (i = 1; i < 60; i++) if (lpix(i, 59)) ok = 1;
	(void) poll((struct pollfd *) NULL, 0, 2300);
	lgrab(l, "note-gone");
	check(ok && lpix(0, 56), "launcher: unknown user noted on the message line, list still drawn");
	stat("/tmp/lc", &s1);
	furi_tap(b, 0x04);			/* Left: SETTINGS (K.7) */
	lgrab(l, "settings");
	furi_tap(b, 0x20);			/* Back, no change */
	(void) poll((struct pollfd *) NULL, 0, 1100);
	stat("/tmp/lc", &s2);
	check(s1.st_mtime == s2.st_mtime && s1.st_size == s2.st_size,
		"launcher: settings and Back without a change write nothing");
	/* Left+Up together (the backlight chord): no page */
	{
		unsigned char m = 0x05;

		(void) write(b, (char *) &m, 1);
		(void) poll((struct pollfd *) NULL, 0, 100);
		m = 0;
		(void) write(b, (char *) &m, 1);
		(void) poll((struct pollfd *) NULL, 0, 250);
	}
	lgrab(l, "chord");
	lcell(6, 7, c);
	check(lpix(0, 0) && c[0] != 0xFF, "launcher: a Left+Up chord opens no page");
	furi_tap(b, 0x04);			/* SETTINGS */
	for (i = 0; i < 7; i++) furi_tap(b, 0x02);	/* to Launcher */
	furi_tap(b, 0x10);			/* OK: its options */
	furi_tap(b, 0x10);			/* OK: icons off */
	lgrab(l, "settings-off");
	furi_tap(b, 0x20);			/* Back: SETTINGS */
	furi_tap(b, 0x20);			/* Back: the list, written */
	lgrab(l, "list-noicons");
	ok = 0;
	if ((f = fopen("/tmp/lc", "r")) != NULL) {
		while (fgets(line, sizeof line, f) != NULL)
			if (strcmp(line, "Icons: off\n") == 0) ok = 1;
		fclose(f);
	}
	check(ok, "launcher: OK toggles, Back writes /etc/launcher.conf (Icons: off)");
	/* ACK7.1 (K.7): SETTINGS -> Display -> Right on Brightness -> Back:
	 * the backlight changes at once, display.conf is written once */
	{
		unsigned long st0 = 0, st1 = 0;
		int fd = open("/dev/backlight", O_RDWR);
		unsigned char lv[1];

		lv[0] = 128;			/* 50 %: room to go up */
		if (fd >= 0) {
			(void) write(fd, (char *) lv, 1);
			lseek(fd, 0L, SEEK_SET);
			(void) read(fd, (char *) &st0, 4);
		}
		unlink("/etc/flipper/display.conf");
		furi_tap(b, 0x04);		/* SETTINGS */
		furi_tap(b, 0x10);		/* OK on Display */
		(void) poll((struct pollfd *) NULL, 0, 800);
		lgrab(l, "display");
		furi_tap(b, 0x08);		/* Right: brightness +10 % */
		if (fd >= 0) {
			lseek(fd, 0L, SEEK_SET);
			(void) read(fd, (char *) &st1, 4);
		}
		furi_tap(b, 0x20);		/* Back: saved, the launcher */
		(void) poll((struct pollfd *) NULL, 0, 500);
		ok = 0;
		if ((f = fopen("/etc/flipper/display.conf", "r")) != NULL) {
			while (fgets(line, sizeof line, f) != NULL)
				if (strcmp(line, "Brightness: 60\n") == 0) ok = 1;
			fclose(f);
		}
		check((st1 >> 16 & 0xFF) > (st0 >> 16 & 0xFF),
			"settings: Display Right raises the brightness at once (ACK7.1)");
		check(ok, "settings: Back writes /etc/flipper/display.conf (ACK7.1)");
		furi_tap(b, 0x20);		/* SETTINGS -> list */
		if (fd >= 0) close(fd);
		unlink("/etc/flipper/display.conf");
	}
	settings_test(b, l);			/* T24b, spec K.7 */
	window_test(b, l);			/* T23b, spec K.8 */
	desktop_test(b, l, pid);		/* T23a, spec K.6 */
	kill(pid, SIGKILL);
	(void) waitpid(pid, &st, 0);
	/* m2test is process 1: whatever the launcher left is ours now */
	while (waitpid(-1, &st, WNOHANG) > 0) ;
	close(b);
	close(l);
	unlink("/tmp/la");
	unlink("/tmp/lc");
}

static void furihello_test(void)
{
	static const unsigned char keys[4] = { 0x10, 0x20, 0x02, 0x10 };
	unsigned char lcd[1024];
	int b, l, i, st, popup = 0;
	pid_t pid, r = 0;

	b = open("/dev/buttons", O_WRONLY);
	l = open("/dev/lcd", O_RDWR);
	/* a press while the backlight is off only turns it on */
	if (b >= 0) furi_tap(b, 0x04);
	memset(lcd, 0, sizeof lcd);
	if (l >= 0) (void) write(l, (char *) lcd, sizeof lcd);
	fflush(stdout);
	pid = fork();
	if (pid == 0) {
		execl("/bin/furihello", "furihello", (char *) NULL);
		_exit(99);
	}
	/* wait for its first frame (the menu's header on the top line) */
	for (i = 0; l >= 0 && i < 50; i++) {
		int x, top = 0;

		(void) poll((struct pollfd *) NULL, 0, 100);
		lseek(l, 0L, SEEK_SET);
		if (read(l, (char *) lcd, sizeof lcd) != sizeof lcd) break;
		for (x = 0; x < 128; x++) top |= lcd[x];
		if (top) break;
	}
	for (i = 0; b >= 0 && i < 4; i++) {
		furi_tap(b, keys[i]);
		lseek(l, 0L, SEEK_SET);
		if (i == 0 && l >= 0 && read(l, (char *) lcd, sizeof lcd)
							== sizeof lcd) {
			/* the popup: an empty top line, text in rows 20-27 */
			int x, top = 0, text = 0;

			for (x = 0; x < 128; x++) {
				top |= lcd[x];
				text |= lcd[2 * 128 + x] | lcd[3 * 128 + x];
			}
			popup = top == 0 && text != 0;
		}
	}
	for (i = 0; i < 40 && (r = waitpid(pid, &st, WNOHANG)) == 0; i++)
		(void) poll((struct pollfd *) NULL, 0, 100);
	if (r == 0) {
		kill(pid, SIGKILL);
		r = waitpid(pid, &st, 0);
	}
	if (b >= 0) close(b);
	if (l >= 0) close(l);
	check(popup, "libfuri GUI: OK on the Submenu shows the Popup on /dev/lcd");
	check(pid > 0 && r == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0,
		"libfuri GUI: furihello Back, Down, OK on Quit -> exit 0");
}
#endif

int main(int argc, char **argv)
{
	char *p, buf[64];
	int i, st, fd[2], n;
	pid_t pid;

	if (argc == 2 && strcmp(argv[1], "warm") == 0) {	/* see init.c */
		for (;;) pause();
	}
#ifdef BOARD_QEMU
	if (argc == 2 && strcmp(argv[1], "sg-read") == 0) {	/* #302 */
		unsigned long got[8];
		int c = open("/dev/subghzraw", O_RDONLY);

		n = read(c, (char *) got, sizeof(got));
		return n == 16 && got[0] == 500 && got[1] == 1000
			&& got[2] == 500 && got[3] == 2000 ? 0 : 1;
	}
#endif
	if (argc == 2 && strcmp(argv[1], "child-exec") == 0) {
		printf("  [exec'd child pid %d running]\n", getpid());
		return 7;
	}

#if REALTIME_WRAP
	wrap_test();			/* first: the wrap comes soon */
#endif
	printf("m2test: pid %d, argc %d:", getpid(), argc);
	for (i = 0; i < argc; i++) printf(" %s", argv[i]);
	printf("\n");
	check(argc == 3 && strcmp(argv[1], "hello") == 0
			&& strcmp(argv[2], "world") == 0, "argv");
	check(getenv("HOME") != NULL && strcmp(getenv("HOME"), "/") == 0,
							"environ");
	check(data_word == 12345 && strcmp(msg, "relocated pointer") == 0,
							"data + relocations");

	check(memops_ok(), "memcpy/memmove/memset, all alignments");

	p = malloc(10000);	/* default heap + stack is 16 KB (chmem) */
	if (p != NULL) memset(p, 0x5A, 10000);
	check(p != NULL && p[9999] == 0x5A, "malloc 10000");
	free(p);
	p = malloc(200000);	/* must fail cleanly, not corrupt the heap */
	check(p == NULL, "malloc 200000 fails");

	signal(SIGUSR1, handler);
	kill(getpid(), SIGUSR1);
	check(got_sig == SIGUSR1, "signal handler + sigreturn");

	pid = fork();
	if (pid == 0) {
		data_word = 1;		/* must not show up in the parent */
		_exit(42);
	}
	check(pid > 0 && wait(&st) == pid && WIFEXITED(st)
		&& WEXITSTATUS(st) == 42 && data_word == 12345, "fork + wait");

	check(pipe(fd) == 0, "pipe");
	pid = fork();
	if (pid == 0) {
		close(fd[0]);
		write(fd[1], "through the pipe", 17);
		_exit(0);
	}
	close(fd[1]);
	n = read(fd[0], buf, sizeof buf);
	close(fd[0]);
	wait(&st);
	check(n == 17 && strcmp(buf, "through the pipe") == 0,
						"fork + pipe");

	/* Spec K.6: pipe data lives in the cache only, at most half of it
	 * (fs/write.c, pipe_zone); 3 full pipes (21 blocks, the cache has
	 * 18) take real zones beyond that.  Every byte must come back. */
	{
		/* (heap, not .bss: a bigger image fails the forks below) */
		unsigned char *pb = (unsigned char *) malloc(512);
		int pp[3][2], k, j, okk = pb != NULL, np = 0;

		for (k = 0; pb != NULL && k < 3; k++)
			if (pipe(pp[k]) == 0) np++;
		for (k = 0; k < np; k++)
			for (j = 0; j < 14; j++) {
				memset(pb, (k * 14 + j) & 0xFF, 512);
				if (write(pp[k][1], (char *) pb, 512) != 512)
					okk = 0;
			}
		n = open("/bin/sh", O_RDONLY);	/* files go on meanwhile */
		if (n < 0 || read(n, (char *) pb, 512) != 512)
			okk = 0;
		if (n >= 0) close(n);
		for (k = 0; k < np; k++) {
			for (j = 0; j < 14; j++) {
				if (read(pp[k][0], (char *) pb, 512) != 512)
					okk = 0;
				else if (pb[0] != ((k * 14 + j) & 0xFF)
				    || pb[511] != pb[0]) okk = 0;
			}
			close(pp[k][0]);
			close(pp[k][1]);
		}
		free(pb);
		check(np == 3 && okk, "3 full pipes: the data comes back (K.6)");
	}

	/* Shadowing under load: parent and child (same addresses) take
	 * turns 200 times; each flip exchanges their images (flipclicks).
	 * Each must keep seeing its own data. */
	{
		int up[2], down[2], k, okk = 1;
		char c = 0;

		pipe(up);
		pipe(down);
		pid = fork();
		if (pid == 0) {
			data_word = 777;
			for (k = 0; k < 200; k++) {
				if (read(down[0], &c, 1) != 1 || data_word != 777)
					_exit(1);
				c++;
				write(up[1], &c, 1);
			}
			_exit(0);
		}
		for (k = 0; k < 200 && okk; k++) {
			c = (char) k;
			write(down[1], &c, 1);
			if (read(up[0], &c, 1) != 1 || c != (char) (k + 1)
						|| data_word != 12345)
				okk = 0;
		}
		close(up[0]); close(up[1]); close(down[0]); close(down[1]);
		check(okk && wait(&st) == pid && WIFEXITED(st)
			&& WEXITSTATUS(st) == 0, "fork: 200 shadow flips");
	}

	/* R4b.8: the foreground app.  Only it may hand the screen on while
	 * it lives; when it is gone anybody may. */
	{
		int fd = open("/dev/lcd", O_RDONLY), none = 0, bad = 30000;
		int r1, r2, r3, r4, e2;

		pid = fork();
		if (pid == 0) {
			pause();
			_exit(0);
		}
		r1 = ioctl(fd, FGIOCSET, &pid);
		r2 = ioctl(fd, FGIOCSET, &none);
		e2 = errno;
		kill(pid, SIGKILL);
		(void) wait(&st);
		r3 = ioctl(fd, FGIOCSET, &none);
		r4 = ioctl(fd, FGIOCSET, &bad);
		check(fd >= 0 && r1 == 0 && r2 == -1 && e2 == EPERM && r3 == 0
			&& r4 == -1 && errno == ESRCH,
			"foreground: hand over, refuse others, free at exit");
		close(fd);
	}

	/* R6.10 gestures, on QEMU only (fake buttons by writing /dev/buttons;
	 * the Flipper refuses that): Left+Up held 2 s -> forced off; again
	 * -> auto; released after 1 s -> unchanged; with Back -> unchanged. */
	{
		int b = open("/dev/buttons", O_WRONLY), l = open("/dev/backlight",
								O_RDONLY);
		unsigned char m;
		unsigned long st;
		int mode[4], i;
		static const unsigned char keys[4] = { 0x05, 0x05, 0x05, 0x25 };
		static const int hold[4] = { 2, 2, 1, 2 };

		m = 0;
		if (b >= 0 && write(b, (char *) &m, 1) == 1 && l >= 0) {
			for (i = 0; i < 4; i++) {
				m = keys[i];
				(void) write(b, (char *) &m, 1);
				sleep(hold[i]);
				m = 0;
				(void) write(b, (char *) &m, 1);
				sleep(1);
				lseek(l, 0L, SEEK_SET);
				st = 0;
				(void) read(l, (char *) &st, 4);
				mode[i] = (int) (st >> 14 & 3);
			}
			check(mode[0] == 1 && mode[1] == 0 && mode[2] == 0
				&& mode[3] == 0, "gestures: Left+Up toggles forced "
				"off; short or with Back does not");
		}
		if (b >= 0) close(b);
		if (l >= 0) close(l);
	}

	/* Memory protection: a user process may not touch kernel memory. */
	pid = fork();
	if (pid == 0) {
		*(volatile int *) 0x20000100 = 0;	/* kernel data */
		_exit(0);
	}
	check(pid > 0 && wait(&st) == pid && WIFSIGNALED(st)
		&& WTERMSIG(st) == SIGSEGV, "write to kernel memory -> SIGSEGV");

	/* R4a.2 (AC4a.1): NULL, code on the stack (no execute from RAM when
	 * the text is in flash), the stack running down into the heap. */
	pid = fork();
	if (pid == 0) {
		*(volatile int *) 0 = 1;	/* QEMU: the image is at 0, read-only */
		_exit(0);
	}
	check(pid > 0 && wait(&st) == pid && WIFSIGNALED(st)
		&& WTERMSIG(st) == SIGSEGV, "write at NULL -> SIGSEGV");
	if ((unsigned long) main >= 0x20000000UL
				&& (unsigned long) main < 0x20040000UL) {
		printf("  skip jump into the stack: this text is in RAM\n");
	} else {
		pid = fork();
		if (pid == 0) {
			volatile unsigned short code[2];

			code[0] = 0x4770;		/* bx lr */
			code[1] = 0x4770;
			((void (*)(void)) ((unsigned long) code | 1))();
			_exit(0);			/* it ran: no XN */
		}
		check(pid > 0 && wait(&st) == pid && WIFSIGNALED(st)
			&& WTERMSIG(st) == SIGSEGV,
			"jump into the stack -> SIGSEGV");
	}
	pid = fork();
	if (pid == 0) {
		(void) malloc(64);			/* some heap */
		_exit(dig(0) ? 0 : 1);
	}
	check(pid > 0 && wait(&st) == pid && WIFSIGNALED(st)
		&& WTERMSIG(st) == SIGSEGV, "stack into the heap -> SIGSEGV");

	/* Buffers of system calls are checked against the caller's memory
	 * (S13): not into the kernel, not past the end of the stack; but a
	 * constant (text, possibly in flash) is fine as a source. */
	{
		char local[4];
		int z = open("/dev/zero", O_RDONLY), r1, r2, r3, r4;

		errno = 0;
		r1 = read(z, (char *) 0x20000100, 16);
		check(r1 == -1 && errno == EFAULT, "read into kernel RAM -> EFAULT");
		r2 = write(1, (char *) 0x20000100, 16);
		check(r2 == -1 && errno == EFAULT, "write from kernel RAM -> EFAULT");
		r3 = read(z, local, 65536);
		check(r3 == -1 && errno == EFAULT, "buffer past the stack -> EFAULT");
		r4 = write(1, "  ok   write from a constant\n", 29);
		if (r4 != 29) check(0, "write from a constant");
		close(z);
	}

	pid = fork();
	if (pid == 0) {
		execl("/bin/m2test", "m2test", "child-exec", (char *) NULL);
		_exit(99);
	}
	check(pid > 0 && wait(&st) == pid && WIFEXITED(st)
				&& WEXITSTATUS(st) == 7, "fork + exec + wait");

	{	/* vfork (R2c.1): the child runs in our memory until exec or
		 * _exit; we wait, then go on with its pid */
		volatile int marker = 1234;
		char *argv0 = "m2test";
		pid_t me = getpid();

		pid = vfork();
		if (pid == 0) {
			marker = 5678;		/* our memory: we see it */
			execl("/bin/m2test", argv0, "child-exec", (char *) NULL);
			_exit(99);
		}
		check(pid > 0 && pid != me && marker == 5678
			&& wait(&st) == pid && WIFEXITED(st)
			&& WEXITSTATUS(st) == 7, "vfork + exec + wait");
		pid = vfork();
		if (pid == 0) _exit(5);
		check(pid > 0 && wait(&st) == pid && WIFEXITED(st)
			&& WEXITSTATUS(st) == 5 && getpid() == me,
						"vfork + _exit + wait");
		for (marker = 0; marker < 20; marker++) {	/* no leak */
			pid = vfork();
			if (pid == 0) _exit(0);
			if (pid < 0 || wait(&st) != pid) break;
		}
		check(marker == 20, "vfork 20 times");
	}

	phase3_tests();
#ifdef BOARD_QEMU
	/* libfuri core (spec R5.1): cooperative threads, queues, flags,
	 * mutexes, semaphores, timers, records, furi_string, furi_check. */
	fflush(stdout);
	pid = fork();
	if (pid == 0) {
		execl("/bin/furitest", "furitest", (char *) NULL);
		_exit(99);
	}
	check(pid > 0 && wait(&st) == pid && WIFEXITED(st)
		&& WEXITSTATUS(st) == 0, "libfuri core (furitest)");
	furihello_test();
	launcher_test();
	/* liblfrfid (spec R9.0.1) on the target: all 25 protocols allocate,
	 * render and fill their T5577 blocks; the line "decoders: N B of
	 * heap" is the R9.0.1 measurement (budget 6 KB). */
	fflush(stdout);
	pid = fork();
	if (pid == 0) {
		execl("/bin/rfid", "rfid", "list", (char *) NULL);
		_exit(99);
	}
	check(pid > 0 && wait(&st) == pid && WIFEXITED(st)
		&& WEXITSTATUS(st) == 0, "rfid list (liblfrfid)");
	/* libmodem (spec section P) on the target: the RFID line code's
	 * numbers and every Sub-GHz preset's CC1101 values and legal check
	 * (64-bit integer arithmetic, no FPU). */
	fflush(stdout);
	pid = fork();
	if (pid == 0) {
		execl("/bin/gmodem", "gmodem", "info", (char *) NULL);
		_exit(99);
	}
	check(pid > 0 && wait(&st) == pid && WIFEXITED(st)
		&& WEXITSTATUS(st) == 0, "gmodem info (libmodem)");
#endif
	memtest_tests();
	poll_tests();
#ifdef BOARD_QEMU
	devpoll_tests();
	ibutton_tests();
	hid_tests();
	subghz_tests();
	gpio_tests();
	card_tests();
	sflag_tests();
	discard_tests();
	preempt_tests();
	pkg_tests();
#endif
	time_tests();
	finish();
	return 0;
}
