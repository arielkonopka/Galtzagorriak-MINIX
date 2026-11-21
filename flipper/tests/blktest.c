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
#include "t.h"

#define BK_SKIP		(16 * 384UL)	/* sectors of the swap slots */
#define BK_BLOCK	9

static char buf[BLK_SIZE], buf2[BLK_SIZE];

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

int main(int argc, char **argv)
{
	struct blkinfo bi;
	unsigned long ext, last, sec;
	int r, a, b, c, go1[2], go2[2], go3[2], st, pid;
	char ch = 0;

	if (argc > 1 && strcmp(argv[1], "-n") == 0) return all_enosys();
	if (argc > 1 && strcmp(argv[1], "-p") == 0) return all_eperm();
	if (argc > 1 && strcmp(argv[1], "-h") == 0) return half();
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
	printf("blktest: %d failures\n", t_fail);
	return t_fail;
}
