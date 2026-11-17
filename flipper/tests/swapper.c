/* swapper - swap to the SD card, plain mode (spec Phase 12, R12.28).
 * Run as root with swap on (swapon /dev/hd3 plain).
 *
 *   swapper		all tests below, 100 kill cycles
 *   swapper -n N	the same with N kill cycles
 *   swapper -f		fill: start programs until more than RAM is in use
 *   swapper -p		(helper) wait in pause(), check memory on SIGUSR1
 *   swapper -w		(helper) wait for a child, check memory after
 *   swapper -c N	placement (AC12.10): N cycles, see test_place()
 *   swapper -P		(helper) a parked app: pause, check memory on
 *			every SIGUSR1, wait again; exits on SIGTERM
 *   swapper -F		(helper) a foreground app: runs for 0-1 s
 *   swapper -x		(helper) sleeps 3 s, exit status 7
 *
 *  1. image back intact, gap zeroed (AC12.5): a child fills its data, heap
 *     and stack, leaves junk in its gap, is swapped out (MI_SWAPTEST) and
 *     woken by a signal it catches; it checks its memory and the gap.
 *  2. a wait wakes a swapped parent (R12.6): the child of a swapped
 *     process exits; the parent comes back and checks its memory.
 *  3. kill without swap-in (AC12.6): N times swap out, SIGKILL, wait;
 *     free memory, swapped processes and the swap-in count are unchanged.
 *  4. a damaged slot (AC12.13): one byte flipped on the card; on swap-in
 *     the process is killed with SIGKILL, never run.
 *  5. never swapped (AC12.13): a Secret: yes image (swapsecret) and a
 *     set-uid copy of this program, /tmp/swsuid; its plain copy
 *     /tmp/swplain is swapped (control).  Only programs whose text runs
 *     from flash are swapped (R12.3): tools/swaptest.py installs these
 *     (pkg flash) before.
 *  6. on demand (AC12.1): programs started until they need more than the
 *     free RAM; the ones started first are swapped out to make room, all
 *     start, and all come back with their memory intact.
 *  7. placement (AC12.10, -c N): two small parked apps (swapsmall -P,
 *     placed high) are swapped out; N times a foreground app (-F, placed
 *     low by MI_FGEXEC as the launcher does) runs while one of them, at
 *     random, is resumed (SIGUSR1) and parked again.  A resumed app must
 *     never wait for its memory longer than that foreground session.
 * Exit status: the number of failures.
 */
#include <lib.h>
#include <minix/callnr.h>
#include <poll.h>
#include "t.h"

#define PAT(i)	((unsigned char) ((i) * 37 + 11))
#define HEAP	4096
#define JUNK	3072

static unsigned char dat[2048] = { 1 };	/* .data */
static unsigned char *heap;
static volatile int woke;
static char *me;

static int meminfo(int req, int i1, int i3, void *p)
{
	message m;

	m.m1_i1 = i1;
	m.m1_i2 = req;
	m.m1_i3 = i3;
	m.m1_p1 = p;
	return _syscall(MM, MEMINFO, &m);
}

static long free_bytes(void)
{
	message m;

	m.m1_i1 = 0;
	m.m1_i2 = 0;
	m.m1_p1 = NULL;
	return _syscall(MM, MEMINFO, &m);
}

static int swapinfo(struct swinfo *si)
{
	return meminfo(MI_SWAPINFO, 0, 0, si);
}

static int is_swapped(int pid, int *slot)
{
	struct swinfo si;
	int i;

	if (swapinfo(&si) < 0) return 0;
	for (i = 0; i < si.nproc; i++)
		if (si.p[i].pid == pid) {
			if (slot) *slot = si.p[i].slot;
			return 1;
		}
	return 0;
}

/* Swap pid out as soon as it waits in MM: 0, or the last error (errno
 * itself is not kept: the sleep between tries ends with EINTR). */
static int swap_out(int pid, int tries)
{
	int e = EINVAL;

	while (tries-- > 0) {
		if (meminfo(MI_SWAPTEST, 1, pid, NULL) == 0) return 0;
		if ((e = errno) != EBUSY) break;
		if (tries > 0) sleep(1);
	}
	return e;
}

/* Sleep ms milliseconds (in FS: poll, not an MM wait). */
static void msleep(int ms)
{
	poll((struct pollfd *) NULL, 0, ms);
}

static void catch(int s)
{
	woke = 1;
}

static void junk(void)		/* leaves JUNK bytes of 0xAA below sp */
{
	volatile unsigned char b[JUNK];
	int i;

	for (i = 0; i < JUNK; i++) b[i] = 0xAA;
}

static int gap_bytes(unsigned char *lo, unsigned char *hi, int want_zero)
{
	/* want_zero: 1 if [lo, hi) is all 0; else 1 if it is all 0xAA */
	for (; lo < hi; lo++)
		if (*lo != (want_zero ? 0 : 0xAA)) return 0;
	return 1;
}

/* Fill memory, leave junk in the gap.  Returns a stack array to check. */
static void fill(unsigned char *stk, int n)
{
	int i;

	for (i = 0; i < (int) sizeof dat; i++) dat[i] = PAT(i);
	heap = (unsigned char *) sbrk(HEAP);
	for (i = 0; i < HEAP; i++) heap[i] = PAT(i + 7);
	for (i = 0; i < n; i++) stk[i] = PAT(i + 3);
	junk();
}

/* 0: all as left by fill(); bit 1 data, 2 heap, 4 stack, 8 gap not 0.
 * The gap checked is the deep part of junk()'s array, below stk: the
 * signal frame and handler after the swap-in are above it. */
static int check(unsigned char *stk, int n, int gap)
{
	int i, r = 0;
	unsigned char *sp = stk;

	for (i = 0; i < (int) sizeof dat; i++) if (dat[i] != PAT(i)) r |= 1;
	for (i = 0; i < HEAP; i++) if (heap[i] != PAT(i + 7)) r |= 2;
	for (i = 0; i < n; i++) if (stk[i] != PAT(i + 3)) r |= 4;
	if (gap && !gap_bytes(sp - JUNK, sp - 2048, 1)) r |= 8;
	return r;
}

/* Helper -p: fill, pause, check after a caught SIGUSR1. */
static int do_pause(void)
{
	unsigned char stk[512];
	unsigned char *sp = (unsigned char *) &stk[0];

	signal(SIGUSR1, catch);
	fill(stk, sizeof stk);
	if (!gap_bytes(sp - JUNK, sp - 2048, 0)) return 16;	/* no junk */
	while (!woke) pause();
	return check(stk, sizeof stk, 1);
}

/* Helper -w: fill, wait for a child that sleeps 3 s, check.  The child
 * execs (vfork): a forked child that does not would share the parent's
 * addresses, and such a parent is never swapped (swap.c).  It is the
 * small copy (swapsmall -x, text in flash): next to two big images there
 * is no room for a program with its text in RAM. */
static int do_wait(void)
{
	unsigned char stk[512];
	int pid, st;

	fill(stk, sizeof stk);
	if ((pid = vfork()) == 0) {
		execl("/usr/tests/swapsmall", "swapsmall", "-x", (char *) NULL);
		_exit(127);
	}
	if (pid < 0 || waitpid(pid, &st, 0) != pid || !WIFEXITED(st)
					|| WEXITSTATUS(st) != 7) return 32;
	return check(stk, sizeof stk, 1);
}

/* Helper -P: a parked app.  Every SIGUSR1: check memory (exit 64 if
 * changed), then wait again. */
static int do_park(void)
{
	unsigned char stk[512];

	signal(SIGUSR1, catch);
	fill(stk, sizeof stk);
	for (;;) {
		while (!woke) pause();
		woke = 0;
		if (check(stk, sizeof stk, 0) != 0) return 64;
	}
}

/* Start "prog -p" (or -w); the pid. */
static int start(char *prog, char *opt)
{
	int pid;

	fflush(stdout);
	if ((pid = vfork()) == 0) {
		execl(prog, prog, opt, (char *) NULL);
		_exit(127);
	}
	return pid;
}

static void test_wake(void)
{
	int pid, st;

	pid = start(me, "-p");
	t_result("swap out a paused process", pid > 0
		&& swap_out(pid, 10) == 0 && is_swapped(pid, NULL), NULL);
	kill(pid, SIGUSR1);
	st = -1;
	waitpid(pid, &st, 0);
	t_result("back on a caught signal, memory intact, gap zeroed (AC12.5)",
		st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
		t_status(st));

	pid = start(me, "-w");
	t_result("swap out a waiting process", pid > 0
		&& swap_out(pid, 10) == 0 && is_swapped(pid, NULL), NULL);
	st = -1;
	waitpid(pid, &st, 0);
	t_result("back when its child exits, memory intact (R12.6)",
		st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0,
		t_status(st));
}

static void test_kill(int n)
{
	struct swinfo a, b;
	long f0, f1;
	int i, pid, st, ok = 1, bad = 0;

	sleep(1);
	swapinfo(&a);
	f0 = free_bytes();
	for (i = 0; i < n && ok; i++) {
		pid = start(me, "-p");
		if (pid < 0 || swap_out(pid, 10) != 0) { ok = 0; break; }
		kill(pid, SIGKILL);
		st = -1;
		if (waitpid(pid, &st, 0) != pid || !WIFSIGNALED(st)
				|| WTERMSIG(st) != SIGKILL) bad++;
	}
	f1 = free_bytes();
	swapinfo(&b);
	printf("  %d cycles: free %ld -> %ld bytes, swap-outs %u -> %u, "
		"swap-ins %u -> %u\n", i, f0, f1, a.nout, b.nout, a.nin, b.nin);
	t_result("kill a swapped process, N times (AC12.6)", ok && bad == 0
		&& (unsigned short) (b.nout - a.nout) == (unsigned) n, NULL);
	t_result("  no swap-in, free memory and slot use unchanged",
		b.nin == a.nin && f1 == f0 && b.nproc == a.nproc, NULL);
}

static void test_damage(void)
{
	int pid, st, fd, slot = -1;
	long pos;
	unsigned char c;

	pid = start(me, "-p");
	if (pid < 0 || swap_out(pid, 10) != 0 || !is_swapped(pid, &slot)) {
		t_result("damaged slot: swap out", 0, NULL);
		return;
	}
	pos = (long) slot * 192L * 1024 + 512 + 100;	/* in the image */
	fd = open("/dev/hd3", O_RDWR);
	if (fd < 0 || lseek(fd, pos, SEEK_SET) != pos || read(fd, &c, 1) != 1
	    || lseek(fd, pos, SEEK_SET) != pos
	    || (c ^= 0xFF, write(fd, &c, 1) != 1)) {
		t_result("damaged slot: flip a byte on /dev/hd3", 0, NULL);
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		return;
	}
	close(fd);
	sync();
	kill(pid, SIGUSR1);
	st = -1;
	waitpid(pid, &st, 0);
	t_result("damaged slot: killed on swap-in, never run (AC12.13)",
		st != -1 && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
		t_status(st));
}

static int never(char *prog)
{
	int pid, st, r;

	pid = start(prog, "-p");
	if (pid < 0) return 0;
	sleep(1);
	r = swap_out(pid, 3);
	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
	return r == EBUSY;
}

static void test_never(void)
{
	struct stat sb;

	t_result("Secret: yes image never swapped (AC12.13)",
		never("/usr/tests/swapsecret"), NULL);
	if (stat("/tmp/swsuid", &sb) < 0 || !(sb.st_mode & S_ISUID)
				|| access("/tmp/swplain", X_OK) < 0) {
		t_result("set-uid image: /tmp/swsuid, /tmp/swplain", 0,
			"missing (tools/swaptest.py makes them)");
		return;
	}
	t_result("set-uid image never swapped (AC12.13)", never("/tmp/swsuid"),
								NULL);
	t_result("  a plain copy is swapped (control)", !never("/tmp/swplain"),
								NULL);
}

/* Start programs until more than the free RAM is in use (AC12.1).  With
 * -f (want_parent) go on until the parent, a shell waiting for us, has
 * been swapped out on demand as well (or 12 programs run). */
static int test_fill(int want_parent)
{
	int pid[12], st, n, i, ok, back = 0, swapped = 0;
	long f = free_bytes(), img = 0;
	struct swinfo si;

	for (n = 0; n < 12; ) {
		if ((pid[n] = start(me, "-p")) < 0) break;
		sleep(2);		/* waited 1 s: may be swapped */
		if (waitpid(pid[n], &st, WNOHANG) != 0) {	/* exec failed */
			printf("  start %d: %s\n", n, t_status(st));
			break;
		}
		if (n++ == 0) img = f - free_bytes();
		if (img > 0 && (long) n * img > f + f / 2
			&& (!want_parent || is_swapped(getppid(), NULL))) break;
	}
	if (want_parent)
		t_result("the waiting parent shell swapped on demand (AC12.1)",
			is_swapped(getppid(), NULL), NULL);
	swapinfo(&si);
	swapped = si.nproc;
	printf("  %d programs of %ld bytes started, %ld bytes were free; "
		"%d swapped\n", n, img, f, swapped);
	ok = n >= 3 && swapped >= 1;
	t_result("start more than fits: the idle ones swapped (AC12.1)", ok,
								NULL);
	for (i = 0; i < n; i++) kill(pid[i], SIGUSR1);
	for (i = 0; i < n; i++) {
		st = -1;
		waitpid(pid[i], &st, 0);
		if (st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0) back++;
		else printf("  pid %d: %s\n", pid[i], t_status(st));
	}
	t_result("  all back, memory intact", back == n, NULL);
	return ok && back == n;
}

/* Placement over N cycles (AC12.10); see the top. */
static void test_place(int n)
{
	int park[2], fg, st, i, k, lost = 0, late = 0, bad = 0, slow = 0;
	unsigned seed = (unsigned) getpid();
	message m;

	for (k = 0; k < 2; k++) {
		park[k] = start("/usr/tests/swapsmall", "-P");
		sleep(1);
		if (park[k] < 0 || swap_out(park[k], 10) != 0) {
			t_result("placement: park two apps", 0, NULL);
			while (k-- > 0) kill(park[k], SIGKILL);
			return;
		}
	}
	for (i = 0; i < n; i++) {
		seed = seed * 1103515245U + 12345U;
		k = (seed >> 16) & 1;
		m.m1_i2 = MI_FGEXEC;		/* as the launcher (R12.22) */
		_syscall(MM, MEMINFO, &m);
		if ((fg = start("/usr/tests/swapsmall", "-F")) < 0) { bad++; break; }
		kill(park[k], SIGUSR1);		/* resume it in the session */
		msleep(200);
		if (is_swapped(park[k], NULL)) slow++;	/* still waiting */
		st = -1;
		waitpid(fg, &st, 0);		/* the session ends */
		if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) bad++;
		msleep(100);
		if (is_swapped(park[k], NULL)) late++;
		if (waitpid(park[k], &st, WNOHANG) != 0) { lost++; break; }
		if (swap_out(park[k], 10) != 0) bad++;	/* parked again */
	}
	for (k = 0; k < 2; k++) {
		kill(park[k], SIGKILL);
		waitpid(park[k], &st, 0);
	}
	printf("  %d cycles: resumed at once %d, waited for the session %d, "
		"longer %d, lost %d, other errors %d\n", i, i - slow - late,
		slow, late, lost, bad);
	t_result("placement: no parked app waits longer than one foreground "
		"session (AC12.10)", i == n && late == 0 && lost == 0
		&& bad == 0, NULL);
}

int main(int argc, char **argv)
{
	struct swinfo si;
	int n = 100;

	me = argv[0];
	if (argc == 2 && strcmp(argv[1], "-p") == 0) return do_pause();
	if (argc == 2 && strcmp(argv[1], "-w") == 0) return do_wait();
	if (argc == 2 && strcmp(argv[1], "-P") == 0) return do_park();
	if (argc == 2 && strcmp(argv[1], "-x") == 0) {
		sleep(3);
		return 7;
	}
	if (argc == 2 && strcmp(argv[1], "-F") == 0) {
		msleep((getpid() % 11) * 100);
		return 0;
	}
	if (getuid() != 0) {
		printf("swapper: run it as root\n");
		return 1;
	}
	if (swapinfo(&si) < 0 || si.mode != SW_PLAIN) {
		printf("swapper: swap is not on (swapon /dev/hd3)\n");
		return 1;
	}
	if (argc == 2 && strcmp(argv[1], "-f") == 0) {
		sleep(2);	/* the parent shell has waited 1 s (R12.5 a) */
		test_fill(1);
		return t_fail;
	}
	if (argc == 3 && strcmp(argv[1], "-c") == 0) {
		test_place(atoi(argv[2]));
		return t_fail;
	}
	if (argc == 3 && strcmp(argv[1], "-n") == 0) n = atoi(argv[2]);
	test_wake();
	test_kill(n);
	test_damage();
	test_never();
	test_fill(0);
	printf("swapper: %s\n", t_fail ? "FAIL" : "all passed");
	return t_fail;
}
