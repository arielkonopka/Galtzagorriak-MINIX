/* fptest - the R5.7 FPU checks (lazy FP context switch, arm/mpx.S), run
 * by m2test in QEMU and by hand on the device: /bin/fptest.
 * m2fpu.c and fpregs.S are built for the FPU, this file is not: between
 * fp_fill and fp_check nothing here touches the FP registers.  Each check
 * runs in a child, so this process stays without an FP context.
 * Exit status: the number of failures.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fails;

static void check(int ok, const char *what)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	fflush(stdout);
	if (!ok) fails++;
}

int fpu_calc(int n);
void fp_load(const unsigned *v);
void fp_store(unsigned *v);

/* S0-S31 = seed << 8 | n, FPSCR = RMode (seed & 3) and the IOC flag. */
static void fp_pattern(unsigned *v, int seed)
{
	int i;

	for (i = 0; i < 32; i++) v[i] = (unsigned) seed << 8 | i;
	v[32] = (unsigned) (seed & 3) << 22 | 1;
}

static void fp_fill(int seed)
{
	unsigned v[33];

	fp_pattern(v, seed);
	fp_load(v);
}

static int fp_check(int seed)	/* registers that differ from fp_fill */
{
	unsigned v[33], w[33];
	int i, bad = 0;

	fp_store(w);
	fp_pattern(v, seed);
	for (i = 0; i < 33; i++) bad += v[i] != w[i];
	return bad;
}

static int fp_nonzero(void)	/* nonzero registers, FPSCR included */
{
	unsigned w[33];
	int i, n = 0;

	fp_store(w);
	for (i = 0; i < 33; i++) n += w[i] != 0;
	return n;
}

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
	execl("/bin/fptest", "fptest", "fp-exec", (char *) NULL);
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

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "fp-exec") == 0)
		return fp_nonzero();
	fpu_tests();
	return fails;
}
