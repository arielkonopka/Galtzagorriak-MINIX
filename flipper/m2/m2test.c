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
#include <minix/fg.h>
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

/* R5.7: the FPU, lazy context switch (arm/mpx.S).  m2fpu.c and
 * fpregs.S are built for the FPU, the rest of m2test is not; each check
 * runs in a child, so this process stays without an FP context. */
int fpu_calc(int n);
void fp_fill(int seed);
int fp_check(int seed);
int fp_nonzero(void);

static volatile int fp_sig_bad = -1;

static void fp_handler(int sig)
{
	fp_fill(2);			/* the handler's own FP context */
	getpid();
	fp_sig_bad = fp_check(2);
}

static int fp_child(int (*fn)(void))	/* fn's result as exit status */
{
	pid_t pid;
	int st;

	fflush(stdout);
	if ((pid = fork()) == 0) _exit(fn());
	if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st))
		return -1;
	return WEXITSTATUS(st);
}

static int fp_t_calc(void)
{
	return fpu_calc(10) == 4393 ? 0 : 1;
}

static int fp_t_pingpong(void)	/* two FP processes take turns */
{
	int a[2], b[2], i, bad = 0, st;
	pid_t pid;
	char c = 0;

	if (pipe(a) < 0 || pipe(b) < 0) return 99;
	if ((pid = fork()) == 0) {
		fp_fill(0x55);
		for (i = 0; i < 100; i++) {
			if (read(a[0], &c, 1) != 1) _exit(98);
			bad += fp_check(0x55) != 0;
			write(b[1], &c, 1);
		}
		_exit(bad);
	}
	fp_fill(0x33);
	for (i = 0; i < 100; i++) {
		write(a[1], &c, 1);
		if (read(b[0], &c, 1) != 1) { bad = 97; break; }
		bad += fp_check(0x33) != 0;
	}
	if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st)) return 96;
	return bad + WEXITSTATUS(st);
}

static int fp_t_preempt(void)	/* two FP processes, time-sliced */
{
	int i, bad = 0, st;
	pid_t pid;
	int seed;

	pid = fork();
	seed = pid == 0 ? 0x77 : 0x66;
	fp_fill(seed);
	for (i = 0; i < 300000; i++) bad += fp_check(seed) != 0;
	if (pid == 0) _exit(bad != 0);
	if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st)) return 96;
	return (bad != 0) + WEXITSTATUS(st);
}

static int fp_t_leak(void)	/* FP registers stay with their owner */
{
	int a[2], st, r;
	pid_t pid;
	char c = 0;

	if (pipe(a) < 0) return 99;
	if ((pid = fork()) == 0) {
		fp_fill(0x44);		/* secret, then let the parent run */
		write(a[1], &c, 1);
		for (;;) fp_check(0x44);
	}
	read(a[0], &c, 1);
	r = fp_nonzero();		/* this process's first FP use */
	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
	return r;
}

static int fp_t_signal(void)	/* handler with its own FP context */
{
	signal(SIGUSR1, fp_handler);
	fp_fill(1);
	kill(getpid(), SIGUSR1);
	return (fp_sig_bad != 0) + (fp_check(1) != 0) * 2;
}

static int fp_t_fork(void)	/* the child gets a copy */
{
	int st, bad;
	pid_t pid;

	fp_fill(5);
	pid = fork();
	bad = fp_check(5) != 0;
	if (pid == 0) _exit(bad);
	if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st))
		return 96;
	return bad + WEXITSTATUS(st) * 2;
}

static int fp_t_exec(void)	/* exec clears the FP registers */
{
	fp_fill(3);
	execl("/bin/m2test", "m2test", "fp-exec", (char *) NULL);
	return 99;
}

static void fpu_tests(void)
{
	int r;

	check(fp_child(fp_t_calc) == 0, "fpu: float arithmetic (softfp)");
	r = fp_child(fp_t_pingpong);
	check(r == 0, "fpu: two FP processes take turns, registers kept");
	r = fp_child(fp_t_preempt);
	check(r == 0, "fpu: two FP processes time-sliced, registers kept");
	r = fp_child(fp_t_leak);
	check(r == 0, "fpu: another process's FP registers read as 0");
	r = fp_child(fp_t_signal);
	check(r == 0, "fpu: signal handler with its own FP context");
	r = fp_child(fp_t_fork);
	check(r == 0, "fpu: fork copies the FP context");
	r = fp_child(fp_t_exec);
	check(r == 0, "fpu: exec clears the FP registers");
}

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
	if (argc == 2 && strcmp(argv[1], "fp-exec") == 0)
		return fp_nonzero();
	if (argc == 2 && strcmp(argv[1], "child-exec") == 0) {
		printf("  [exec'd child pid %d running]\n", getpid());
		return 7;
	}

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

	fpu_tests();
	finish();
	return 0;
}
