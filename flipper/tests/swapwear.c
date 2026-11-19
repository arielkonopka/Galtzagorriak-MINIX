/* swapwear - less SD card wear from swap (spec 12g, R12.29-R12.31).
 * Run as root in QEMU with swap on (swapon /dev/hd3 plain), with this
 * program's text in flash (pkg flash /usr/tests/swapwear: text in RAM is
 * never swapped, R12.3).  tools/swaptest.py runs it.
 *
 *   swapwear		AC12.14 (below)
 *   swapwear -r S	AC12.15 for S seconds (the spec: 600)
 *   swapwear -P	(helper) a parked app: pause; on SIGUSR1 change one
 *			heap byte if /tmp/swpoke is there
 *   swapwear -T	(helper) a parked app with a 30 s timer
 *   swapwear -H	(helper) pause until killed
 *
 * AC12.14, sectors written counted in the WINCHESTER task (QEMU's
 * DIOCFAKECARD 3):
 *   1. a 32 KB process is swapped out (the whole image), back in by a
 *      signal, and out again without touching its heap: that swap-out
 *      writes 0 sectors.  The first wakes change a little of the image
 *      by themselves (libc's static sigreturn message, then a register
 *      saved on the stack), so two warm-up cycles come first;
 *   2. in again with one heap byte changed, out: the header and one 4 KB
 *      chunk, 9 sectors;
 *   3. AC12.13 for a skipped chunk: a byte flipped on the card in a chunk
 *      the last swap-out did not write: the swap-in kills it (CRC).
 * AC12.15 (-r S): the reserve on (all RAM, so free RAM is always under it).
 *   1. the parked app with a 30 s timer is swapped out by the reserve after
 *      10 s (control: it may be swapped);
 *   2. its timer brings it back (R12.30: SWAPHOT); for S seconds after
 *      that the reserve never swaps it out again;
 *   3. programs started until they need its memory: it is swapped out at
 *      once (on demand ignores SWAPHOT).
 * Exit status: the number of failures.
 */
#include <lib.h>
#include <sys/ioctl.h>
#include <minix/callnr.h>
#include <poll.h>
#include "t.h"

#define HEAP	(20 * 1024)		/* with libc, gap and stack: about 32 KB */
#define POKE	(12 * 1024)		/* the byte changed: inside a chunk */
#define POKEFILE "/tmp/swpoke"		/* there: the next wake changes it */

static unsigned char heap[HEAP];
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

static void msleep(int ms)		/* in FS (poll): never swapped */
{
	poll((struct pollfd *) NULL, 0, ms);
}

/* Sectors written to the card since boot (QEMU only), -1: unknown. */
static long written(void)
{
	static int fd = -1;
	int n = 3;

	if (fd < 0 && (fd = open("/dev/hd0", O_RDONLY)) < 0) return -1;
	if (ioctl(fd, DIOCFAKECARD, &n) < 0) return -1;
	return n;
}

/* Swap pid out as soon as it waits in MM again; the sectors written by
 * that swap-out, or -1. */
static long swap_out(int pid)
{
	long w0;
	int tries;

	for (tries = 0; tries < 50; tries++) {
		w0 = written();
		if (meminfo(MI_SWAPTEST, 1, pid, NULL) == 0)
			return w0 < 0 ? -1 : written() - w0;
		if (errno != EBUSY) break;
		msleep(100);
	}
	printf("  swap out pid %d: %s\n", pid, strerror(errno));
	return -1;
}

/* Wake pid with sig; back when it is in RAM (and waits again). */
static int wake(int pid, int sig)
{
	int i;

	kill(pid, sig);
	for (i = 0; i < 50; i++) {
		msleep(100);
		if (!is_swapped(pid, NULL)) {
			msleep(200);	/* back in pause() */
			return 1;
		}
	}
	return 0;
}

static int start(char *opt)
{
	int pid;

	fflush(stdout);
	if ((pid = vfork()) == 0) {
		execl(me, me, opt, (char *) NULL);
		_exit(127);
	}
	return pid;
}

/* Every wake runs the same code; only the file says whether the heap
 * byte changes. */
static void on_usr1(int s)
{
	if (access(POKEFILE, F_OK) == 0) heap[POKE]++;
}

static void on_sig(int s)
{
}

static void fill_heap(void)
{
	int i;

	for (i = 0; i < HEAP; i++) heap[i] = (unsigned char) (i * 37 + 11);
}

/* Helper -P: wait in pause(); each SIGUSR1 changes one heap byte if
 * POKEFILE is there. */
static int do_park(void)
{
	signal(SIGUSR1, on_usr1);
	fill_heap();
	for (;;) pause();
}

/* Helper -T: a parked app with a 30 s timer (AC12.15). */
static int do_timer(void)
{
	signal(SIGALRM, on_sig);
	fill_heap();
	for (;;) {
		alarm(30);
		pause();
	}
}

static void test_changed(void)
{
	int pid, st, fd, k, slot = -1;
	long w, pos;
	unsigned char c;
	char how[64];

	pid = start("-P");
	msleep(500);
	w = swap_out(pid);
	sprintf(how, "%ld sectors", w);
	t_result("swap out a 32 KB process", w > 9 && is_swapped(pid, NULL),
									how);
	for (k = 1; k <= 2; k++) {
		t_result("  back in", wake(pid, SIGUSR1), NULL);
		printf("  warm-up %d: out again, %ld sectors\n", k,
							swap_out(pid));
	}
	t_result("  back in", wake(pid, SIGUSR1), NULL);
	w = swap_out(pid);
	sprintf(how, "%ld sectors", w);
	t_result("out again, nothing changed: 0 sectors written (AC12.14)",
		w == 0 && is_swapped(pid, NULL), how);
	close(creat(POKEFILE, 0644));
	t_result("  back in", wake(pid, SIGUSR1), NULL);
	unlink(POKEFILE);
	w = swap_out(pid);
	sprintf(how, "%ld sectors", w);
	is_swapped(pid, &slot);
	t_result("one heap byte changed: the header and one chunk, "
		"9 sectors (AC12.14)", w == 9 && slot >= 0, how);

	/* AC12.13 in a chunk that last swap-out skipped: image sector 1 is
	 * in chunk 0 (data), the poked byte is 12 KB further up. */
	pos = (long) slot * 192L * 1024 + 512 + 512 + 100;
	fd = open("/dev/hd3", O_RDWR);
	if (slot < 0 || fd < 0 || lseek(fd, pos, SEEK_SET) != pos
	    || read(fd, &c, 1) != 1 || lseek(fd, pos, SEEK_SET) != pos
	    || (c ^= 0xFF, write(fd, &c, 1) != 1)) {
		t_result("skipped chunk: flip a byte on /dev/hd3", 0, NULL);
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		return;
	}
	close(fd);
	sync();
	kill(pid, SIGUSR1);
	st = -1;
	waitpid(pid, &st, 0);
	t_result("a byte flipped in a skipped chunk: killed on swap-in "
		"(AC12.13)", st != -1 && WIFSIGNALED(st)
		&& WTERMSIG(st) == SIGKILL, t_status(st));
}

static void test_hot(int secs)
{
	struct swinfo si;
	int pid, st, i, n, out = 0, h[12];
	unsigned res;
	char how[64];

	swapinfo(&si);
	res = si.res_kb;
	pid = start("-T");
	meminfo(MI_SWAPRES, 192, 0, NULL);	/* all RAM: always below */
	for (i = 0; i < 25 && !is_swapped(pid, NULL); i++) msleep(1000);
	t_result("the reserve swaps the parked app out (control)",
		is_swapped(pid, NULL), NULL);
	for (i = 0; i < 40 && is_swapped(pid, NULL); i++) msleep(1000);
	t_result("  its 30 s timer brings it back", !is_swapped(pid, NULL),
									NULL);
	printf("  watching it for %d s\n", secs);
	fflush(stdout);
	for (i = 0; i < secs; i++) {
		msleep(1000);
		if (is_swapped(pid, NULL)) out++;
	}
	sprintf(how, "swapped in %d of %d checks", out, secs);
	t_result("woken from the card: no reserve swap-out of it (AC12.15)",
		out == 0, how);

	/* On demand ignores SWAPHOT: start programs until it goes. */
	meminfo(MI_SWAPRES, 0, 0, NULL);
	for (n = 0; n < 12 && !is_swapped(pid, NULL); n++)
		if ((h[n] = start("-H")) < 0) break;
	t_result("an exec that needs its memory swaps it out at once "
		"(AC12.15)", is_swapped(pid, NULL), NULL);
	for (i = 0; i < n; i++) kill(h[i], SIGKILL);
	for (i = 0; i < n; i++) waitpid(h[i], &st, 0);
	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
	meminfo(MI_SWAPRES, (int) res, 0, NULL);
}

int main(int argc, char **argv)
{
	struct swinfo si;

	me = argv[0];
	if (argc == 2 && strcmp(argv[1], "-P") == 0) return do_park();
	if (argc == 2 && strcmp(argv[1], "-T") == 0) return do_timer();
	if (argc == 2 && strcmp(argv[1], "-H") == 0) {
		fill_heap();
		for (;;) pause();
	}
	if (getuid() != 0) {
		printf("swapwear: run it as root\n");
		return 1;
	}
	if (swapinfo(&si) < 0 || si.mode != SW_PLAIN) {
		printf("swapwear: swap is not on (swapon /dev/hd3)\n");
		return 1;
	}
	if (written() < 0) {
		printf("swapwear: no sector counter (QEMU only)\n");
		return 1;
	}
	if (argc == 3 && strcmp(argv[1], "-r") == 0) test_hot(atoi(argv[2]));
	else test_changed();
	printf("swapwear: %s\n", t_fail ? "FAIL" : "all passed");
	return t_fail;
}
