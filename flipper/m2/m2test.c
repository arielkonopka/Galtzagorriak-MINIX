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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <minix/fg.h>
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

static void finish(void)
{
	flash_report();
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
	for (;;) pause();
}

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

	time_tests();
	finish();
	return 0;
}
