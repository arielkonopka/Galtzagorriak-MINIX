/* Milestone 2/3 user-level test, run by the test INIT via exec():
 * relocated executable, argv/environ, malloc/brk, signals (kernel signal
 * frame + sigreturn), fork/wait (shadowing), pipes and exec from a child.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static void handler(int sig)
{
	got_sig = sig;
}

static void finish(void)
{
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

	/* Memory protection: a user process may not touch kernel memory. */
	pid = fork();
	if (pid == 0) {
		*(volatile int *) 0x20000100 = 0;	/* kernel data */
		_exit(0);
	}
	check(pid > 0 && wait(&st) == pid && WIFSIGNALED(st)
		&& WTERMSIG(st) == SIGSEGV, "write to kernel memory -> SIGSEGV");

	pid = fork();
	if (pid == 0) {
		execl("/bin/m2test", "m2test", "child-exec", (char *) NULL);
		_exit(99);
	}
	check(pid > 0 && wait(&st) == pid && WIFEXITED(st)
				&& WEXITSTATUS(st) == 7, "fork + exec + wait");

	finish();
	return 0;
}
