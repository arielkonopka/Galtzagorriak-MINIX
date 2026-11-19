/* bread(), bwrite() - raw sectors of a disk partition	(R12.39, spec O.3)
 *
 * int bread(int fd, unsigned long sector, void *buf, unsigned count);
 * int bwrite(int fd, unsigned long sector, const void *buf, unsigned count);
 *
 * Read or write count sectors of 512 bytes, starting at the given sector
 * of the partition open on fd, with the DIOCPRW ioctl: also past 4 GiB,
 * where read() and lseek() cannot go.  Done in pieces of PRW_MAX sectors.
 * 0 when all were done, else -1 with errno.  FS refuses a write on a
 * mounted partition.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <minix/partition.h>

static int prw(int fd, unsigned long sector, char *buf, unsigned count,
								int wr)
{
	struct part_rw rw;
	unsigned n;

	while (count > 0) {
		n = count < PRW_MAX ? count : PRW_MAX;
		rw.sector = sector;
		rw.count = n;
		rw.write = wr;
		rw.buf = buf;
		if (ioctl(fd, DIOCPRW, (void *) &rw) < 0) return -1;
		sector += n;
		buf += (unsigned long) n * 512;
		count -= n;
	}
	return 0;
}

int bread(int fd, unsigned long sector, void *buf, unsigned count)
{
	return prw(fd, sector, (char *) buf, count, 0);
}

int bwrite(int fd, unsigned long sector, const void *buf, unsigned count)
{
	return prw(fd, sector, (char *) buf, count, 1);
}
