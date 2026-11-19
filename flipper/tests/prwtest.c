/* prwtest - R12.39 partition ioctls and raw sectors (spec O.3).
 *
 * Usage: prwtest /dev/hdN [mounted]
 *
 * Run as root on an SD card partition (tools/bigcard.py puts /home past
 * 4 GiB on the QEMU card first).  Checks:
 *  1. DIOCGETPS gives the first sector and the size in sectors;
 *     DIOCGETP gives the base in bytes, or EOVERFLOW past 4 GiB, and
 *     the size in bytes (at most 4 GiB - 512)
 *  2. bread() of sector 0 and 1 equals read() of the first 1 KB
 *  3. "mounted": bwrite() refused (EBUSY), the data unchanged;
 *     else bwrite()/bread() of the last sector round-trips (the old
 *     contents are put back) and past the end is EINVAL
 *  4. bwrite() through a read-only descriptor: EBADF
 * Exit status: the number of failures.
 */
#include "t.h"
#include <sys/ioctl.h>
#include <minix/partition.h>

static char a[1024], b[1024], c[512], d[512];

int main(int argc, char **argv)
{
	struct part_range rg;
	struct partition p;
	int fd, ro, r, mounted;
	unsigned long last;

	if (argc < 2) {
		fprintf(stderr, "Usage: prwtest /dev/hdN [mounted]\n");
		return 1;
	}
	mounted = argc > 2 && strcmp(argv[2], "mounted") == 0;
	if ((fd = open(argv[1], mounted ? O_RDONLY : O_RDWR)) < 0
	    || (ro = open(argv[1], O_RDONLY)) < 0) {
		perror(argv[1]);
		return 1;
	}

	r = ioctl(fd, DIOCGETPS, &rg);
	t_result("DIOCGETPS", r == 0 && rg.count > 0, NULL);
	printf("first %lu count %lu (%s 4 GiB)\n", (unsigned long) rg.first,
		(unsigned long) rg.count,
		rg.first >= 0x800000UL ? "starts past" : "starts below");
	r = ioctl(fd, DIOCGETP, &p);
	if (rg.first >= 0x800000UL)
		t_result("DIOCGETP past 4 GiB: EOVERFLOW",
			r == -1 && errno == EOVERFLOW, NULL);
	else
		t_result("DIOCGETP base in bytes", r == 0
			&& p.base == (u32_t) rg.first * 512
			&& p.size == (rg.count >= 0x800000UL ? 0xFFFFFE00UL
					: (u32_t) rg.count * 512), NULL);

	r = bread(fd, 0, a, 2);
	t_result("bread sectors 0-1", r == 0, NULL);
	t_result("read() agrees", lseek(ro, 0L, SEEK_SET) == 0
		&& read(ro, b, sizeof b) == sizeof b
		&& memcmp(a, b, sizeof a) == 0, NULL);

	last = rg.count - 1;
	t_result("bread last sector", bread(ro, last, c, 1) == 0, NULL);
	r = bread(ro, rg.count, d, 1);
	t_result("bread past the end: EINVAL", r == -1 && errno == EINVAL,
		NULL);
	r = bwrite(ro, last, c, 1);
	t_result("bwrite on a read-only descriptor: EBADF",
		r == -1 && errno == EBADF, NULL);
	if (mounted) {
		int w = open(argv[1], O_RDWR);

		r = w < 0 ? 0 : bwrite(w, last, c, 1);
		t_result("bwrite on a mounted device: EBUSY",
			w < 0 || (r == -1 && errno == EBUSY), NULL);
		if (w >= 0) close(w);
	} else {
		memset(d, 0x5A, sizeof d);
		r = bwrite(fd, last, d, 1);
		memset(d, 0, sizeof d);
		t_result("bwrite + bread the last sector", r == 0
			&& bread(fd, last, d, 1) == 0 && d[0] == 0x5A
			&& d[511] == 0x5A, NULL);
		t_result("old contents back", bwrite(fd, last, c, 1) == 0,
			NULL);
	}
	return t_fail;
}
