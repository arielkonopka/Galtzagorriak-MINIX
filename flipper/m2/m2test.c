/* Milestone 2/3 user-level test, run by the test INIT via exec():
 * relocated executable, argv/environ, malloc/brk, signals (kernel signal
 * frame + sigreturn), fork/wait (shadowing), pipes and exec from a child.
 */
#include <lib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <minix/callnr.h>
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
#endif
	memtest_tests();
	poll_tests();
#ifdef BOARD_QEMU
	devpoll_tests();
	ibutton_tests();
	gpio_tests();
	card_tests();
	discard_tests();
	preempt_tests();
#endif
	time_tests();
	finish();
	return 0;
}
