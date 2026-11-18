/* swaptime - how long a swapped program takes to wake (spec R12.28, the
 * device check "swap-in time of a 64 KB image" in #109).
 *
 *   swaptime [-n N]	N rounds (default 10) with swap-out, N without
 *   swaptime64 -p	(helper, chmem 64 KB) wait in pause(); answer each
 *			SIGUSR1 with one byte on standard output
 *
 * Each round: MM swaps the helper out (MI_SWAPTEST), the clock starts,
 * SIGUSR1 goes to it, and the round ends when its byte arrives.  The same
 * without the swap-out gives the signal's own round trip; the difference
 * is the swap-in (and the helper's check of its 60 KB, the same in both).
 * Times are clock ticks (1/60 s) summed over the rounds,
 * so the mean has about 1/(60 N) s resolution.  Run as root, swap on.
 * Writes nothing to internal flash; the swap partition is on the SD card.
 */
#include <lib.h>
#include <minix/callnr.h>
#include <sys/times.h>
#include "t.h"

static volatile int got;

static int meminfo(int req, int i1, int i3, void *p)
{
	message m;

	m.m1_i1 = i1;
	m.m1_i2 = req;
	m.m1_i3 = i3;
	m.m1_p1 = p;
	return _syscall(MM, MEMINFO, &m);
}

static void on_usr1(int sig)
{
	got = 1;
	signal(SIGUSR1, on_usr1);
}

/* Most of the 64 KB image, on the heap (malloc: only the helper has it):
 * MM swaps data, bss, heap and stack, not the gap between heap and stack
 * (it comes back zeroed). */
#define BIG	(60 * 1024)

static int helper(void)
{
	char *big = malloc(BIG);
	unsigned i;
	int ok;

	if (big == NULL) return 1;
	for (i = 0; i < BIG; i++) big[i] = (char) (i * 37 + 11);
	signal(SIGUSR1, on_usr1);
	(void) write(1, "r", 1);	/* ready */
	for (;;) {
		while (!got) pause();
		got = 0;
		/* came back whole? (read every byte: also keeps big live) */
		for (ok = 1, i = 0; i < BIG; i++)
			if (big[i] != (char) (i * 37 + 11)) ok = 0;
		if (write(1, ok ? "x" : "e", 1) != 1) return 1;
	}
}

/* One round: optionally swap pid out, then time SIGUSR1 -> its byte. */
static long round(int pid, int rfd, int swap)
{
	struct tms tm;
	struct swinfo si;
	clock_t t0;
	char c;
	int i;

	if (swap) {
		for (i = 0; i < 50; i++) {
			if (meminfo(MI_SWAPTEST, 1, pid, NULL) == 0) break;
			sleep(1);	/* not in pause() yet */
		}
		if (i == 50) return -1;
		if (meminfo(MI_SWAPINFO, 0, 0, &si) < 0 || si.nproc == 0)
			return -1;
	}
	t0 = times(&tm);
	if (kill(pid, SIGUSR1) != 0 || read(rfd, &c, 1) != 1) return -1;
	t0 = times(&tm) - t0;
	return c == 'x' ? (long) t0 : -2;
}

int main(int argc, char **argv)
{
	struct swinfo si;
	long sw = 0, plain = 0, t;
	int n = 10, i, p[2], pid, st;
	unsigned short in0;
	char c;

	if (argc == 2 && strcmp(argv[1], "-p") == 0) return helper();
	if (argc == 3 && strcmp(argv[1], "-n") == 0) n = atoi(argv[2]);
	if (n < 1 || n > 1000) {
		fprintf(stderr, "Usage: swaptime [-n rounds]\n");
		return 2;
	}
	if (meminfo(MI_SWAPINFO, 0, 0, &si) < 0 || si.mode == 0) {
		printf("swaptime: swap is not on (swapon /dev/hd3), or not root\n");
		return 2;
	}
	in0 = si.nin;
	if (pipe(p) != 0) return 2;
	if ((pid = fork()) == 0) {
		dup2(p[1], 1);
		close(p[0]);
		close(p[1]);
		execl("/usr/tests/swaptime64", "swaptime64", "-p", (char *) 0);
		_exit(99);
	}
	close(p[1]);
	if (pid < 0 || read(p[0], &c, 1) != 1 || c != 'r') {
		printf("swaptime: the helper did not start\n");
		return 2;
	}
	sleep(1);
	for (i = 0; i < n; i++) {
		if ((t = round(pid, p[0], 1)) < 0) break;
		sw += t;
		if ((t = round(pid, p[0], 0)) < 0) break;
		plain += t;
	}
	(void) meminfo(MI_SWAPINFO, 0, 0, &si);
	kill(pid, SIGKILL);
	(void) wait(&st);
	t_result("swaptime rounds", i == n, i == n ? NULL
		: t == -2 ? "the image came back damaged" : "a round failed");
	t_result("swaptime swap-ins counted", (unsigned short) (si.nin - in0) >= n,
		NULL);
	/* ms = ticks * 1000 / 60, mean over n */
	printf("swaptime: %d rounds: swapped %ld.%ld ms, not swapped %ld.%ld ms "
		"(mean); swap-in %ld ms\n", i,
		sw * 1000 / 60 / (i ? i : 1), sw * 10000 / 60 / (i ? i : 1) % 10,
		plain * 1000 / 60 / (i ? i : 1),
		plain * 10000 / 60 / (i ? i : 1) % 10,
		(sw - plain) * 1000 / 60 / (i ? i : 1));
	return t_fail;
}
