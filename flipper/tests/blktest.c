/* blktest - swappable memory blocks for apps (spec 12h, R12.34-R12.37).
 * Run in QEMU with blocks on (swapon /dev/hd3 plain blk, or
 * swapon /dev/hd3 blk); tools/blktest.py runs it.
 *
 *   blktest		as root: AC12.18 and AC12.19 (below)
 *   blktest -n		blocks off: every request gives ENOSYS (AC12.22)
 *   blktest -p		a set-uid or Secret: yes image: EPERM (AC12.22)
 *   blktest -h		not root: at most half of the block space (AC12.18)
 *   blktest -z		(helper, another program: a copy of this file) the
 *			first extent's blocks read as zeros (AC12.19)
 *   blktest -w		(helper) open 1 MB, PUT a pattern in block 0, exit
 *   blktest -d		(swap off) marks in a swap slot and in the block space
 *   blktest -D		R12.40: after MMSWAPON they read as zeros (the
 *			DISCARD of the whole space at idle; QEMU zero-fills)
 *   blktest -c		AC12.20: sectors written (QEMU's WINCHESTER counter):
 *			a whole-block PUT 9, BLK_IFCHANGED of the same data 0,
 *			exit 0
 *   blktest -g		cards >= 32 GB: blocks over hd3a, hd3b, hd3d (R12.34)
 *   blktest -l		<blk.h> and libblk (R12.38) on the real MM: the
 *			libc calls, then 256 KB through 1 KB frames
 *   blktest -k		AC12.21 while tools/blktest.py types keys (after
 *			"KEYS", 3 s, then Enter): a background GET is
 *			answered only after them, a foreground app's (-F)
 *			PUT and GET at once
 *   blktest -o		AC12.21: a background writer's PUTs stop at the
 *			write budget; the card pulled (QEMU DIOCFAKECARD):
 *			a foreground GET (-X) is ENXIO and the app lives on,
 *			a background GET waits until the card is back
 *
 * AC12.18: GET without a reservation is EBADF; after OPEN of 1 MB block 0
 * and the last block read as zeros, one past the end is ERANGE; an OPEN
 * larger than the space is ENOSPC, with BLK_ANY the largest free run;
 * runs of 3 and 5 extents do not overlap, and after the first one ends
 * a 4-extent run goes behind the 5; PUT then GET of 4 KB and of a 1 KB
 * range; a byte flipped on the card gives EIO for that sector only, and
 * the process lives on.
 * AC12.19: after the writer exits, a new process on the same extents
 * reads zeros; so does another program after exec; a forked child has
 * no reservation; a block number past the run is refused, whatever it is.
 * Exit status: the number of failures.
 */
#include <lib.h>
#include <minix/callnr.h>
#include <minix/blk.h>
#include <libblk.h>
#include "t.h"
#include <sys/ioctl.h>
#include <sys/times.h>
#include <poll.h>
#include <time.h>

#define BK_SKIP		(16 * 384UL)	/* sectors of the swap slots */
#define BK_BLOCK	9

static char buf[BLK_SIZE] __attribute__((aligned(4))), buf2[BLK_SIZE];

static int blk(int op, int flags, unsigned long id, int first, int count,
							void *b)
{
	struct blkreq rq;
	message m;

	memset(&rq, 0, sizeof(rq));
	rq.op = op;
	rq.flags = flags;
	rq.id = id;
	rq.first = first;
	rq.count = count;
	rq.buf = b;
	m.m1_i2 = MI_BLK;
	m.m1_p1 = (char *) &rq;
	return _syscall(MM, MEMINFO, &m);
}

static int info(struct blkinfo *bi)
{
	return blk(BLK_INFO, 0, 0, 0, 0, bi);
}

static int zeros(char *b, int n)
{
	while (n-- > 0) if (*b++ != 0) return 0;
	return 1;
}

static void fill(char *b, int n, int seed)
{
	int i;

	for (i = 0; i < n; i++) b[i] = (char) (i * 7 + seed);
}

/* Child: hold n extents, write the first extent's number to fd, wait for
 * a byte on the other pipe, exit. */
static int hold(int n, unsigned long ext, int *go)
{
	int p[2], pid;
	struct blkinfo bi;
	char c;
	long first = -1;

	if (pipe(p) < 0) return -1;
	if ((pid = fork()) == 0) {
		close(go[1]);
		if (blk(BLK_OPEN, 0, (unsigned long) n * ext * BLK_SIZE, 0, 0,
							NULL) >= 0
				&& info(&bi) == 0)
			first = (long) bi.run;
		write(p[1], &first, sizeof(first));
		read(go[0], &c, 1);
		_exit(0);
	}
	close(p[1]);
	if (read(p[0], &first, sizeof(first)) != sizeof(first)) first = -1;
	close(p[0]);
	return (int) first;
}

static int flip(unsigned long sector)
{
	int fd = open("/dev/hd3", O_RDWR);
	char c;

	if (fd < 0) return -1;
	if (lseek(fd, (off_t) (sector * 512 + 100), SEEK_SET) < 0
			|| read(fd, &c, 1) != 1) return -1;
	c ^= 0x55;
	if (lseek(fd, (off_t) (sector * 512 + 100), SEEK_SET) < 0
			|| write(fd, &c, 1) != 1) return -1;
	close(fd);
	sync();
	return 0;
}

/* R12.40: the DISCARD at MMSWAPON.  Sectors in slot 5 and in the block
 * space (inside the first 64 MB partition's extents). */
static const unsigned long wipe_sec[] = { 5 * 384UL + 1, BK_SKIP + 100,
						BK_SKIP + 60000UL };

static int wipe(int check)
{
	int fd = open("/dev/hd3", O_RDWR), i, ok = 1;

	if (fd < 0) return 0;
	for (i = 0; i < 3; i++) {
		memset(buf, 0x5A, 512);
		if (lseek(fd, (off_t) (wipe_sec[i] * 512), SEEK_SET) < 0) ok = 0;
		else if (!check) ok &= write(fd, buf, 512) == 512;
		else ok &= read(fd, buf, 512) == 512 && zeros(buf, 512);
	}
	close(fd);
	sync();
	return ok;
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

/* AC12.20.  Each count is tried up to 3 times: update(8) may sync
 * something meanwhile; only an exact count passes. */
static int count(void)
{
	long w0, w;
	int k, ok, go[2], back[2], pid, st;
	char ch = 0;

	if (blk(BLK_OPEN, 0, 64UL * 1024, 0, 0, NULL) <= 0) {
		t_result("AC12.20: blk_open", 0, strerror(errno));
		return t_fail;
	}
	fill(buf, BLK_SIZE, 5);
	for (ok = 0, k = 0; k < 3 && !ok; k++) {
		buf[0]++;
		sync();
		w0 = written();
		ok = blk(BLK_PUT, 0, 1, 0, 0, buf) == 0
			&& (w = written()) >= 0 && w0 >= 0 && w - w0 == 9;
	}
	t_result("AC12.20: a whole-block PUT writes 9 sectors", ok, NULL);
	for (ok = 0, k = 0; k < 3 && !ok; k++) {
		sync();
		w0 = written();
		ok = blk(BLK_PUT, BLK_IFCHANGED, 1, 0, 0, buf) == 0
			&& (w = written()) >= 0 && w0 >= 0 && w == w0;
	}
	t_result("AC12.20: BLK_IFCHANGED of unchanged data writes 0 sectors",
		ok, NULL);
	blk(BLK_CLOSE, 0, 0, 0, 0, NULL);
	for (ok = 0, k = 0; k < 3 && !ok; k++) {
		pipe(go);
		pipe(back);
		if ((pid = fork()) == 0) {
			ch = blk(BLK_OPEN, 0, 64UL * 1024, 0, 0, NULL) > 0
				&& blk(BLK_PUT, 0, 0, 0, 0, buf) == 0;
			write(back[1], &ch, 1);
			read(go[0], &ch, 1);
			_exit(0);
		}
		close(back[1]);
		close(go[0]);
		ch = 0;
		if (read(back[0], &ch, 1) != 1) ch = 0;
		sync();
		w0 = written();
		write(go[1], &ch, 1);
		waitpid(pid, &st, 0);
		w = written();
		ok = ch == 1 && w0 >= 0 && w == w0;
		close(go[1]);
		close(back[0]);
		sleep(1);
	}
	t_result("AC12.20: exit of a block writer writes nothing", ok, NULL);
	return t_fail;
}

static int all_enosys(void)
{
	struct blkinfo bi;

	t_result("AC12.22: blk=off: OPEN is ENOSYS",
		blk(BLK_OPEN, 0, 4096, 0, 0, NULL) < 0 && errno == ENOSYS, NULL);
	t_result("AC12.22: blk=off: INFO is ENOSYS",
		info(&bi) < 0 && errno == ENOSYS, NULL);
	return t_fail;
}

static int all_eperm(void)
{
	t_result("AC12.22: set-uid / Secret: yes image: OPEN is EPERM",
		blk(BLK_OPEN, 0, 4096, 0, 0, NULL) < 0 && errno == EPERM,
		NULL);
	return t_fail;
}

static int half(void)
{
	struct blkinfo bi;
	unsigned long eb;
	int r;

	if (info(&bi) < 0) {
		t_result("INFO", 0, strerror(errno));
		return t_fail;
	}
	eb = bi.extent * BLK_SIZE;
	r = blk(BLK_OPEN, 0, (bi.space / 2) * BLK_SIZE + eb, 0, 0, NULL);
	t_result("AC12.18: not root: more than half of the space is ENOSPC",
		r < 0 && errno == ENOSPC, NULL);
	r = blk(BLK_OPEN, 0, (bi.space / 2) * BLK_SIZE, 0, 0, NULL);
	t_result("AC12.18: not root: half of the space is granted",
		r == (int) (bi.space / 2), NULL);
	return t_fail;
}

/* R12.38: the libc calls and libblk over MM's blocks (the pool: buf). */

static int lib(void)
{
	struct blkinfo bi;
	struct vm v;
	unsigned long off, size = 256UL * 1024, sum1 = 0, sum2 = 0;
	unsigned char *p;
	int r, ok = 1;

	t_result("R12.38: blk_get without blk_open is EBADF",
		blk_get(0, buf) < 0 && errno == EBADF, NULL);
	r = blk_open(64UL * 1024, 0);
	t_result("R12.38: blk_open(64 KB)", r >= 16, NULL);
	t_result("R12.38: blk_info", blk_info(&bi) == 0
		&& bi.blocks == (unsigned long) r && bi.size == BLK_SIZE, NULL);
	fill(buf, BLK_SIZE, 5);
	t_result("R12.38: blk_put then blk_get", blk_put(3, buf) == 0
		&& blk_get(3, buf2) == 0 && memcmp(buf, buf2, BLK_SIZE) == 0,
		NULL);
	t_result("R12.38: blk_putf(BLK_IFCHANGED) of the same data",
		blk_putf(3, buf, BLK_IFCHANGED) == 0, NULL);
	fill(buf, 1024, 6);
	t_result("R12.38: blk_write then blk_read of 1 KB at 2 KB",
		blk_write(3, 2048, buf, 1024) == 0
		&& blk_read(3, 2048, buf2, 1024) == 0
		&& memcmp(buf, buf2, 1024) == 0, NULL);
	t_result("R12.38: blk_read off or len not a multiple of 512: EINVAL",
		blk_read(3, 100, buf2, 512) < 0 && errno == EINVAL
		&& blk_read(3, 0, buf2, 100) < 0 && errno == EINVAL
		&& blk_read(3, 3584, buf2, 1024) < 0 && errno == EINVAL, NULL);
	fill(buf, BLK_SIZE, 5);
	t_result("R12.40: blk_discard of block 3, then it reads as zeros",
		blk_put(3, buf) == 0 && blk_discard(3, 1) == 0
		&& blk_get(3, buf2) == 0 && zeros(buf2, BLK_SIZE), NULL);
	t_result("R12.40: blk_discard past the run is ERANGE",
		blk_discard((unsigned long) r, 1) < 0 && errno == ERANGE
		&& blk_discard(0, 0) < 0 && errno == ERANGE, NULL);
	t_result("R12.38: blk_close", blk_close() == 0
		&& blk_get(0, buf) < 0 && errno == EBADF, NULL);

	r = vm_open(&v, buf, sizeof(buf), size, 1024, 0);
	t_result("libblk: vm_open(256 KB, 1 KB frames), 3 frames",
		r == 0 && v.nframes == 3 && v.size >= size, NULL);
	if (r < 0) return t_fail;
	for (off = 0; off < size && ok; off += 64) {
		p = vm_get(&v, off, VM_RW);
		if (p == NULL) { ok = 0; break; }
		memset(p, (int) (off >> 6), 64);
		sum1 += (off >> 6) & 0xFF;
	}
	t_result("libblk: 256 KB written through the frames", ok
		&& vm_flush(&v) == 0, NULL);
	for (off = 0; off < size && ok; off += 64) {
		p = vm_get(&v, off, VM_RD);
		if (p == NULL || p[63] != p[0]) { ok = 0; break; }
		sum2 += p[0];
	}
	t_result("libblk: ... and read back (checksum)", ok && sum1 == sum2,
		NULL);
	t_result("libblk: vm_close", vm_close(&v) == 0
		&& blk_info(&bi) == 0 && bi.blocks == 0, NULL);
	printf("blktest -l: %d failures\n", t_fail);
	return t_fail;
}

/* Cards >= 32 GB (R12.34), tools/blktest.py: swap on hd3a, blocks over
 * hd3a, hd3b and hd3d (hd3c is not a swap partition): 64 extents each, a
 * run never crosses a subpartition, data goes to the right one. */
static int big(void)
{
	struct blkinfo bi;
	int p[2], st;
	long run = -1;
	pid_t c;

	t_result("R12.34: INFO: 3 of the 4 subpartitions (hd3c is not swap)",
		info(&bi) == 0 && bi.extent > 0 && bi.space == 192 * bi.extent,
		NULL);
	t_result("R12.34: BLK_ANY: one subpartition at most (64 extents)",
		blk(BLK_OPEN, BLK_ANY, 0xFFFFFFF0UL, 0, 0, NULL)
			== (int) (64 * bi.extent)
		&& info(&bi) == 0 && bi.run == 0, NULL);
	pipe(p);
	if ((c = fork()) == 0) {
		pid_t g;

		/* hd3b, then its child gets hd3d */
		if (blk(BLK_OPEN, BLK_ANY, 0xFFFFFFF0UL, 0, 0, NULL) <= 0
				|| info(&bi) != 0) _exit(1);
		write(p[1], &bi.run, sizeof(bi.run));
		if ((g = fork()) == 0) {
			unsigned long last;

			if (blk(BLK_OPEN, BLK_ANY, 0xFFFFFFF0UL, 0, 0, NULL)
				<= 0 || info(&bi) != 0) _exit(1);
			write(p[1], &bi.run, sizeof(bi.run));
			last = bi.blocks - 1;
			fill(buf, BLK_SIZE, 7);
			if ((st = blk(BLK_PUT, 0, last, 0, 0, buf)) != 0)
				printf("PUT %lu: %d errno %d\n", last, st, errno);
			if (blk(BLK_PUT, 0, 0, 0, 0, buf) != 0) _exit(2);
			memset(buf2, 0, BLK_SIZE);
			if ((st = blk(BLK_GET, 0, last, 0, 0, buf2)) != 0)
				printf("GET %lu: %d errno %d\n", last, st, errno);
			if (st != 0 || memcmp(buf, buf2, BLK_SIZE) != 0) _exit(3);
			_exit(0);
		}
		waitpid(g, &st, 0);
		_exit(WIFEXITED(st) ? WEXITSTATUS(st) : 1);
	}
	close(p[1]);
	if (read(p[0], &run, sizeof(run)) != sizeof(run)) run = -1;
	t_result("R12.34: the next run is hd3b's (extent 64)", run == 64, NULL);
	if (read(p[0], &run, sizeof(run)) != sizeof(run)) run = -1;
	t_result("R12.34: then hd3d's (extent 192; hd3c skipped)", run == 192,
		NULL);
	close(p[0]);
	waitpid(c, &st, 0);
	t_result("R12.34: PUT/GET of hd3d's last block",
		WIFEXITED(st) && WEXITSTATUS(st) == 0, NULL);
	blk(BLK_CLOSE, 0, 0, 0, 0, NULL);
	printf("blktest -g: %d failures\n", t_fail);
	return t_fail;
}


/* Real time since boot, ticks (times(2)). */
static long now(void)
{
	struct tms tb;

	return (long) times(&tb);
}

/* Sleep ms milliseconds (in FS: poll, not an MM wait). */
static void msleep(int ms)
{
	poll((struct pollfd *) NULL, 0, ms);
}

/* QEMU: the card in (0) or pulled out (1), DIOCFAKECARD on /dev/hd0. */
static int card(int state)
{
	static int fd = -1;

	if (fd < 0 && (fd = open("/dev/hd0", O_RDONLY)) < 0) return -1;
	return ioctl(fd, DIOCFAKECARD, &state);
}

/* The caller's next child execs as the foreground app (R12.22). */
static void fgexec(void)
{
	message m;

	m.m1_i2 = MI_FGEXEC;
	_syscall(MM, MEMINFO, &m);
}

/* -F: a foreground app while keys arrive: its own run, a PUT and a GET
 * answered at once (within 0.5 s, not after the keys). */
static int fg_keys(void)
{
	long t;
	int ok;

	if (blk(BLK_OPEN, 0, 64UL * 1024, 0, 0, NULL) <= 0) return 2;
	fill(buf, BLK_SIZE, 11);
	t = now();
	ok = blk(BLK_PUT, 0, 0, 0, 0, buf) == 0
		&& blk(BLK_GET, 0, 0, 0, 0, buf2) == 0
		&& memcmp(buf, buf2, BLK_SIZE) == 0;
	t = now() - t;
	printf("foreground PUT+GET while keys arrive: %ld ticks\n", t);
	return ok && t <= CLK_TCK / 2 ? 0 : 1;
}

/* -X rfd wfd: a foreground app while the card is pulled.  'r' when its
 * block is written; on a byte a GET ('e': ENXIO); on the next one (the
 * card is back) the GET again, which must give the block. */
static int fg_card(int rfd, int wfd)
{
	char c;
	int r;

	if (blk(BLK_OPEN, 0, 64UL * 1024, 0, 0, NULL) <= 0) return 2;
	fill(buf, BLK_SIZE, 13);
	if (blk(BLK_PUT, 0, 0, 0, 0, buf) != 0) return 3;
	write(wfd, "r", 1);
	if (read(rfd, &c, 1) != 1) return 4;
	r = blk(BLK_GET, 0, 0, 0, 0, buf2);
	c = r < 0 && errno == ENXIO ? 'e' : 'x';
	write(wfd, &c, 1);
	if (read(rfd, &c, 1) != 1) return 5;
	return blk(BLK_GET, 0, 0, 0, 0, buf2) == 0
		&& memcmp(buf, buf2, BLK_SIZE) == 0 ? 0 : 6;
}

/* AC12.21, keys: tools/blktest.py types for 3 s after "KEYS". */
static int keys(void)
{
	char line[256];
	int p[2], st, fst = -1;
	pid_t c, f;
	long tk, tg = -1;

	if (pipe(p) < 0) return 1;
	if ((c = fork()) == 0) {	/* a background caller */
		long t;

		if (blk(BLK_OPEN, 0, 64UL * 1024, 0, 0, NULL) <= 0) _exit(2);
		st = blk(BLK_GET, 0, 0, 0, 0, buf2);
		t = now();
		write(p[1], &t, sizeof(t));
		_exit(st == 0 && zeros(buf2, BLK_SIZE) ? 0 : 1);
	}
	fgexec();
	if ((f = fork()) == 0) {
		execl("/usr/tests/blktest", "blktest", "-F", (char *) NULL);
		_exit(9);
	}
	printf("KEYS\n");
	fflush(stdout);
	(void) read(0, line, sizeof(line));	/* the keys, then Enter */
	tk = now();
	st = waitpid(f, &fst, WNOHANG) == f;
	t_result("AC12.21: a foreground app is answered at once while keys "
		"arrive", st && WIFEXITED(fst) && WEXITSTATUS(fst) == 0,
		st ? t_status(fst) : "still running after the keys");
	if (!st) waitpid(f, &fst, 0);
	if (read(p[0], &tg, sizeof(tg)) != sizeof(tg)) tg = -1;
	waitpid(c, &st, 0);
	printf("keys ended at %ld, the background GET came back at %ld\n",
		tk, tg);
	t_result("AC12.21: a background GET is answered only after the keys",
		WIFEXITED(st) && WEXITSTATUS(st) == 0 && tg >= tk,
		t_status(st));
	printf("blktest -k: %d failures\n", t_fail);
	return t_fail;
}

/* AC12.21: the write budget, and the card pulled. */
static int card_out(void)
{
	char c = 0, a[8], b[8];
	int p[2], q[2], s[2], st, n, bg;
	pid_t w, f, g;

	/* A background writer: one byte down the pipe per PUT done. */
	if (pipe(p) < 0) return 1;
	if ((w = fork()) == 0) {
		unsigned long i;

		close(p[0]);
		if (blk(BLK_OPEN, 0, 256UL * 1024, 0, 0, NULL) <= 0) _exit(2);
		fill(buf, BLK_SIZE, 17);
		for (i = 0; ; i = (i + 1) % 32) {
			buf[0] = (char) i;
			if (blk(BLK_PUT, 0, i, 0, 0, buf) != 0) _exit(3);
			write(p[1], "p", 1);
		}
	}
	close(p[1]);
	msleep(8000);
	kill(w, SIGKILL);
	waitpid(w, &st, 0);
	for (n = 0; read(p[0], &c, 1) == 1; n++) ;
	close(p[0]);
	printf("a background writer did %d PUTs in 8 s\n", n);
	/* 256 KB a minute, a whole-block PUT counts 5 KB (R12.37) */
	t_result("AC12.21: background PUTs stop at the write budget",
		WIFSIGNALED(st) && n <= 256 / 5 + 1, t_status(st));

	/* The card pulled: a foreground app gets ENXIO and lives on; a
	 * background caller waits until the card is back. */
	if (pipe(p) < 0 || pipe(q) < 0 || pipe(s) < 0) return 1;
	sprintf(a, "%d", p[0]);
	sprintf(b, "%d", q[1]);
	fgexec();
	if ((f = fork()) == 0) {
		execl("/usr/tests/blktest", "blktest", "-X", a, b,
							(char *) NULL);
		_exit(9);
	}
	if ((g = fork()) == 0) {
		if (blk(BLK_OPEN, 0, 64UL * 1024, 0, 0, NULL) <= 0) _exit(2);
		fill(buf, BLK_SIZE, 19);
		if (blk(BLK_PUT, 0, 0, 0, 0, buf) != 0) _exit(3);
		write(s[1], "r", 1);
		if (read(s[0], &c, 1) != 1) _exit(4);	/* the card is out */
		st = blk(BLK_GET, 0, 0, 0, 0, buf2);
		_exit(st == 0 && memcmp(buf, buf2, BLK_SIZE) == 0 ? 0 : 5);
	}
	st = read(q[0], &c, 1) == 1 && c == 'r'
		&& read(s[0], &c, 1) == 1 && c == 'r';
	t_result("AC12.21: both callers have a block", st, NULL);
	st = st && card(1) == 0;
	write(p[1], "g", 1);
	write(s[1], "g", 1);
	c = 0;
	if (st) (void) read(q[0], &c, 1);
	t_result("AC12.21: card out: a foreground GET is ENXIO at once",
		c == 'e', NULL);
	msleep(2000);
	bg = waitpid(g, &n, WNOHANG) == 0;
	card(0);
	write(p[1], "b", 1);
	t_result("AC12.21: card out: a background GET waits (2 s)", bg,
		bg ? NULL : t_status(n));
	waitpid(f, &st, 0);
	t_result("AC12.21: card back: the foreground app lived on, its block "
		"reads back", WIFEXITED(st) && WEXITSTATUS(st) == 0,
		t_status(st));
	if (bg) waitpid(g, &n, 0);
	t_result("AC12.21: card back: the background GET completes with "
		"the block", WIFEXITED(n) && WEXITSTATUS(n) == 0, t_status(n));
	printf("blktest -o: %d failures\n", t_fail);
	return t_fail;
}


int main(int argc, char **argv)
{
	struct blkinfo bi;
	unsigned long ext, last, sec;
	int r, a, b, c, go1[2], go2[2], go3[2], st, pid;
	long first;
	char ch = 0;

	if (argc > 1 && strcmp(argv[1], "-4") == 0) {
		/* Security review 2026-11-23, M1 (blktest.py -4): on a swap
		 * partition past 4 GiB the block space still ends below
		 * 4 GiB, where MM's 32-bit byte offsets reach (16 slots of
		 * 384 sectors, then 9 sectors a block). */
		r = info(&bi);
		printf("space %lu blocks, extent %lu\n", bi.space, bi.extent);
		t_result("M1: the block space ends below 4 GiB", r == 0
			&& bi.space > 0 && bi.space <= (0x7FFFFFUL - 6144) / 9,
			NULL);
		r = blk(BLK_OPEN, BLK_ANY, 0xFFFFFFFFUL, 0, 0, NULL);
		fill(buf, BLK_SIZE, 9);
		t_result("M1: the first block of a full run: PUT, GET", r > 0
			&& blk(BLK_PUT, 0, 0, 0, 0, buf) == 0
			&& blk(BLK_GET, 0, 0, 0, 0, buf2) == 0
			&& memcmp(buf, buf2, BLK_SIZE) == 0, NULL);
		return t_fail;
	}
	if (argc > 1 && strcmp(argv[1], "-n") == 0) return all_enosys();
	if (argc > 1 && strcmp(argv[1], "-p") == 0) return all_eperm();
	if (argc > 1 && strcmp(argv[1], "-h") == 0) return half();
	if (argc > 1 && strcmp(argv[1], "-l") == 0) return lib();
	if (argc > 1 && strcmp(argv[1], "-g") == 0) return big();
	if (argc > 1 && strcmp(argv[1], "-k") == 0) return keys();
	if (argc > 1 && strcmp(argv[1], "-o") == 0) return card_out();
	if (argc > 1 && strcmp(argv[1], "-F") == 0) return fg_keys();
	if (argc > 3 && strcmp(argv[1], "-X") == 0)
		return fg_card(atoi(argv[2]), atoi(argv[3]));
	if (argc > 1 && strcmp(argv[1], "-d") == 0) return !wipe(0);
	if (argc > 1 && strcmp(argv[1], "-c") == 0) return count();
	if (argc > 1 && strcmp(argv[1], "-D") == 0) {
		t_result("R12.40: MMSWAPON discarded a slot and the block space",
			wipe(1), NULL);
		return t_fail;
	}
	if (argc > 1 && strcmp(argv[1], "-w") == 0) {
		fill(buf, BLK_SIZE, 1);
		if (blk(BLK_OPEN, 0, 1024UL * 1024, 0, 0, NULL) < 0
				|| blk(BLK_PUT, 0, 0, 0, 0, buf) < 0) return 1;
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "-z") == 0) {
		memset(buf, 0x77, sizeof(buf));
		r = blk(BLK_OPEN, 0, 1024UL * 1024, 0, 0, NULL);
		t_result("AC12.19: another program after exec: block 0 reads "
			"as zeros", r > 0 && info(&bi) == 0 && bi.run == 0
			&& blk(BLK_GET, 0, 0, 0, 0, buf) == 0
			&& zeros(buf, BLK_SIZE), NULL);
		return t_fail;
	}

	if (info(&bi) < 0) {
		t_result("INFO", 0, strerror(errno));
		return t_fail;
	}
	ext = bi.extent;
	printf("blocks: %lu in all, %lu per extent, largest free %lu\n",
		bi.space, ext, bi.largest);
	t_result("INFO: 256 extents, 4 KB blocks, nothing reserved",
		bi.space == 256 * ext && bi.size == BLK_SIZE && bi.blocks == 0
		&& bi.largest == bi.space, NULL);

	/* AC12.18 */
	t_result("AC12.18: GET without blk_open is EBADF",
		blk(BLK_GET, 0, 0, 0, 0, buf) < 0 && errno == EBADF, NULL);
	r = blk(BLK_OPEN, 0, 1024UL * 1024, 0, 0, NULL);
	last = (unsigned long) r - 1;
	t_result("AC12.18: blk_open(1 MB) rounds up to whole extents",
		r > 0 && r % (int) ext == 0 && r >= 256 && r < 256 + (int) ext,
		NULL);
	memset(buf, 0x55, sizeof(buf));
	t_result("AC12.18: block 0 reads as zeros",
		blk(BLK_GET, 0, 0, 0, 0, buf) == 0 && zeros(buf, BLK_SIZE),
		NULL);
	memset(buf, 0x55, sizeof(buf));
	t_result("AC12.18: the last block reads as zeros",
		blk(BLK_GET, 0, last, 0, 0, buf) == 0 && zeros(buf, BLK_SIZE),
		NULL);
	t_result("AC12.18: one past the end is ERANGE",
		blk(BLK_GET, 0, last + 1, 0, 0, buf) < 0 && errno == ERANGE,
		NULL);
	t_result("AC12.19: any block number past the run is ERANGE",
		blk(BLK_GET, 0, 0xFFFFFFFFUL, 0, 0, buf) < 0 && errno == ERANGE
		&& blk(BLK_PUT, 0, last + ext, 0, 0, buf) < 0
		&& errno == ERANGE, NULL);
	t_result("AC12.18: a buffer outside the caller's memory is EFAULT",
		blk(BLK_GET, 0, 0, 0, 0, (void *) 0x100) < 0
		&& errno == EFAULT, NULL);

	fill(buf, BLK_SIZE, 3);
	r = blk(BLK_PUT, 0, 5, 0, 0, buf);
	memset(buf2, 0, sizeof(buf2));
	t_result("AC12.18: PUT then GET of 4 KB",
		r == 0 && blk(BLK_GET, 0, 5, 0, 0, buf2) == 0
		&& memcmp(buf, buf2, BLK_SIZE) == 0, NULL);
	fill(buf, 1024, 9);
	r = blk(BLK_PUT, 0, 7, 2, 2, buf);
	memset(buf2, 0x11, sizeof(buf2));
	t_result("AC12.18: PUT then GET of a 1 KB range",
		r == 0 && blk(BLK_GET, 0, 7, 2, 2, buf2) == 0
		&& memcmp(buf, buf2, 1024) == 0, NULL);
	memset(buf2, 0x11, sizeof(buf2));
	t_result("AC12.18: the sectors around a range PUT read as zeros",
		blk(BLK_GET, 0, 7, 0, 0, buf2) == 0 && zeros(buf2, 1024)
		&& memcmp(buf, buf2 + 1024, 1024) == 0
		&& zeros(buf2 + 2048, 2048), NULL);
	t_result("AC12.20: PUT of unchanged data with BLK_IFCHANGED is OK",
		blk(BLK_PUT, BLK_IFCHANGED, 7, 2, 2, buf) == 0, NULL);

	/* A byte flipped on the card: EIO for that sector only. */
	if (info(&bi) < 0) bi.run = 0;
	sec = BK_SKIP + (bi.run * ext + 5) * BK_BLOCK + 1 + 3;
	r = flip(sec);
	t_result("AC12.18: a flipped byte in sector 3: GET of it is EIO",
		r == 0 && blk(BLK_GET, 0, 5, 3, 1, buf2) < 0 && errno == EIO,
		NULL);
	fill(buf, BLK_SIZE, 3);
	t_result("AC12.18: ... the other sectors still read back",
		blk(BLK_GET, 0, 5, 0, 3, buf2) == 0
		&& memcmp(buf, buf2, 1536) == 0
		&& blk(BLK_GET, 0, 5, 4, 4, buf2) == 0
		&& memcmp(buf + 2048, buf2, 2048) == 0, NULL);
	t_result("AC12.18: ... and the process lives on", 1, NULL);

	/* Grow and shrink. */
	r = blk(BLK_OPEN, 0, 2UL * 1024 * 1024, 0, 0, NULL);
	t_result("R12.34: a second blk_open grows the run in place",
		r >= 512 && info(&bi) == 0 && bi.run == 0, NULL);
	fill(buf, BLK_SIZE, 3);
	t_result("R12.34: ... and keeps the data",
		blk(BLK_GET, 0, 5, 0, 3, buf2) == 0
		&& memcmp(buf, buf2, 1536) == 0, NULL);
	r = blk(BLK_OPEN, 0, 4096, 0, 0, NULL);
	t_result("R12.34: a smaller blk_open shrinks it to one extent",
		r == (int) ext && info(&bi) == 0 && bi.blocks == ext, NULL);
	t_result("R12.34: blk_close",
		blk(BLK_CLOSE, 0, 0, 0, 0, NULL) == 0 && info(&bi) == 0
		&& bi.blocks == 0, NULL);

	/* ENOSPC, BLK_ANY. */
	r = blk(BLK_OPEN, 0, (bi.space + ext) * BLK_SIZE, 0, 0, NULL);
	t_result("AC12.18: more than the space is ENOSPC",
		r < 0 && errno == ENOSPC, NULL);
	r = blk(BLK_OPEN, BLK_ANY, (bi.space + ext) * BLK_SIZE, 0, 0, NULL);
	t_result("AC12.18: with BLK_ANY the largest free run (255 extents)",
		r == (int) (255 * ext), NULL);
	blk(BLK_CLOSE, 0, 0, 0, 0, NULL);

	/* Runs of 3 and 5 extents, then 4 after the first ends. */
	pipe(go1);
	pipe(go2);
	pipe(go3);
	a = hold(3, ext, go1);
	b = hold(5, ext, go2);
	t_result("AC12.18: runs of 3 and 5 extents do not overlap",
		a == 0 && b == 3, NULL);
	write(go1[1], &ch, 1);
	wait(&st);
	c = hold(4, ext, go3);
	t_result("AC12.18: after the first ends, 4 extents go behind the 5",
		c == 8, NULL);
	write(go2[1], &ch, 1);
	write(go3[1], &ch, 1);
	wait(&st);
	wait(&st);

	/* AC12.19: a new process on the writer's extents reads zeros. */
	if ((pid = fork()) == 0) {
		execl(argv[0], argv[0], "-w", (char *) NULL);
		_exit(99);
	}
	waitpid(pid, &st, 0);
	t_result("AC12.19: the writer PUT block 0 and exited",
		WIFEXITED(st) && WEXITSTATUS(st) == 0, NULL);
	/* R12.40: its run is free once MM has DISCARDed it at idle. */
	sleep(2);
	memset(buf, 0x77, sizeof(buf));
	r = blk(BLK_OPEN, 0, 1024UL * 1024, 0, 0, NULL);
	t_result("AC12.19: after the writer exits, its block 0 reads as zeros",
		r > 0 && info(&bi) == 0 && bi.run == 0
		&& blk(BLK_GET, 0, 0, 0, 0, buf) == 0 && zeros(buf, BLK_SIZE),
		NULL);
	fill(buf, BLK_SIZE, 1);
	blk(BLK_PUT, 0, 0, 0, 0, buf);
	if ((pid = fork()) == 0) {
		r = blk(BLK_GET, 0, 0, 0, 0, buf2);
		_exit(r < 0 && errno == EBADF ? 0 : 1);
	}
	waitpid(pid, &st, 0);
	t_result("AC12.19: a forked child has no reservation (EBADF)",
		WIFEXITED(st) && WEXITSTATUS(st) == 0, NULL);
	/* exec of another program (a copy of this file: another inode) in
	 * this very process: the run ends, the copy's reads give zeros. */
	blk(BLK_CLOSE, 0, 0, 0, 0, NULL);
	if ((pid = fork()) == 0) {
		fill(buf, BLK_SIZE, 1);
		blk(BLK_OPEN, 0, 1024UL * 1024, 0, 0, NULL);
		blk(BLK_PUT, 0, 0, 0, 0, buf);
		execl("/tmp/blk2", "blk2", "-z", (char *) NULL);
		_exit(99);
	}
	waitpid(pid, &st, 0);
	t_result("AC12.19: exec of another program: zeros (see blk2 above)",
		WIFEXITED(st) && WEXITSTATUS(st) == 0, NULL);
	/* AC12.19: after a pid wrap (the MI_SWAPTEST 3 hook) a process with
	 * the writer's pid, slot and program reads zeros on its extents. */
	if ((pid = fork()) == 0) {
		fill(buf, BLK_SIZE, 3);
		_exit(blk(BLK_OPEN, 0, 1024UL * 1024, 0, 0, NULL) > 0
			&& info(&bi) == 0 && bi.run < 255
			&& blk(BLK_PUT, 0, 0, 0, 0, buf) == 0 ? bi.run : 255);
	}
	waitpid(pid, &st, 0);
	a = WIFEXITED(st) && WEXITSTATUS(st) != 255;
	first = WEXITSTATUS(st);
	sleep(2);
	{
		message m;

		m.m1_i1 = 3;
		m.m1_i2 = MI_SWAPTEST;
		m.m1_i3 = pid;
		b = _syscall(MM, MEMINFO, &m) == 0;
	}
	pipe(go1);
	if ((c = fork()) == 0) {
		memset(buf, 0x77, sizeof(buf));
		ch = blk(BLK_OPEN, 0, 1024UL * 1024, 0, 0, NULL) > 0
			&& info(&bi) == 0 && (long) bi.run == first
			&& blk(BLK_GET, 0, 0, 0, 0, buf) == 0
			&& zeros(buf, BLK_SIZE);
		write(go1[1], &ch, 1);
		_exit(0);
	}
	close(go1[1]);
	ch = 0;
	if (read(go1[0], &ch, 1) != 1) ch = 0;
	close(go1[0]);
	waitpid(c, &st, 0);
	t_result("AC12.19: after a pid wrap the same pid reads zeros",
		a && b && c == pid && ch == 1, NULL);
	/* R12.35: a run that is live at a pid wrap keeps its data: block 0
	 * PUT before the wrap reads back after it; block 1, PUT between two
	 * wraps, reads back after the second (the stamp before the last
	 * wrap is the run's own). */
	{
		message m;

		a = blk(BLK_OPEN, 0, 1024UL * 1024, 0, 0, NULL) > 0;
		fill(buf, BLK_SIZE, 5);
		a = a && blk(BLK_PUT, 0, 0, 0, 0, buf) == 0;
		m.m1_i1 = 3;
		m.m1_i2 = MI_SWAPTEST;
		m.m1_i3 = 0;
		a = a && _syscall(MM, MEMINFO, &m) == 0;
		memset(buf2, 0, sizeof(buf2));
		a = a && blk(BLK_GET, 0, 0, 0, 0, buf2) == 0
			&& memcmp(buf2, buf, BLK_SIZE) == 0;
		fill(buf, BLK_SIZE, 6);
		a = a && blk(BLK_PUT, 0, 1, 0, 0, buf) == 0;
		m.m1_i1 = 3;
		m.m1_i2 = MI_SWAPTEST;
		m.m1_i3 = 0;
		a = a && _syscall(MM, MEMINFO, &m) == 0;
		memset(buf2, 0, sizeof(buf2));
		a = a && blk(BLK_GET, 0, 1, 0, 0, buf2) == 0
			&& memcmp(buf2, buf, BLK_SIZE) == 0;
		blk(BLK_CLOSE, 0, 0, 0, 0, NULL);
		sleep(2);		/* its DISCARD at idle (R12.40) */
		t_result("R12.35: a live run keeps its blocks across a pid wrap",
			a, NULL);
	}
	/* AC12.26 (R12.40): a process PUTs 10 blocks and exits; at idle MM
	 * DISCARDs its run (QEMU fills it with zeros), then frees it. */
	pipe(go1);
	if ((pid = fork()) == 0) {
		int k;

		first = -1;
		fill(buf, BLK_SIZE, 4);
		if (blk(BLK_OPEN, 0, 1024UL * 1024, 0, 0, NULL) > 0
				&& info(&bi) == 0) first = (long) bi.run;
		for (k = 0; k < 10; k++)
			if (blk(BLK_PUT, 0, k, 0, 0, buf) < 0) first = -1;
		write(go1[1], &first, sizeof(first));
		_exit(0);
	}
	close(go1[1]);
	if (read(go1[0], &first, sizeof(first)) != sizeof(first)) first = -1;
	close(go1[0]);
	waitpid(pid, &st, 0);
	sleep(2);
	r = 0;
	if (first >= 0) {
		int fd = open("/dev/hd3", O_RDONLY);
		int k;

		for (k = 0; k < 10 && fd >= 0; k++) {
			sec = BK_SKIP + ((unsigned long) first * ext + k)
							* BK_BLOCK;
			if (lseek(fd, (off_t) (sec * 512), SEEK_SET) < 0
					|| read(fd, buf2, BLK_SIZE) != BLK_SIZE
					|| !zeros(buf2, BLK_SIZE)) break;
		}
		r = fd >= 0 && k == 10;
		if (fd >= 0) close(fd);
	}
	t_result("AC12.26: the writer's 10 blocks were discarded at idle",
		r, NULL);
	t_result("AC12.26: ... and its extents are free again",
		blk(BLK_OPEN, 0, 1024UL * 1024, 0, 0, NULL) > 0
		&& info(&bi) == 0 && (long) bi.run == first, NULL);
	blk(BLK_CLOSE, 0, 0, 0, 0, NULL);
	printf("blktest: %d failures\n", t_fail);
	return t_fail;
}
