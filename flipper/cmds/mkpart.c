/*	mkpart - add partitions after the existing ones		(Flipper port)
 *
 * Usage: mkpart device type:size ...
 *	  mkpart -l device
 *
 * Keeps the partitions already in the MBR (in particular the root file
 * system the system is running from) and fills the following empty slots
 * with new partitions, one per argument, each starting on a 1 MiB boundary
 * after the previous one.  type is hex (81 MINIX, 82 swap); size is in MB,
 * or "*" for everything that is left.  Everything is kept below 4 GiB:
 * MINIX 2.0 addresses partitions with 32-bit byte offsets.  The kernel
 * reads the new table at the next boot.
 *
 * Unlike partition(8) this never moves an existing partition.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <minix/partition.h>
#include <ibm/partition.h>

#define SECTOR		512
#define ALIGN		2048UL			/* 1 MiB in sectors */
#define LIMIT_SECS	(0xFFFFFFFFUL / SECTOR)	/* 4 GiB - 1 byte */

static unsigned char mbr[SECTOR];
static char *prog;

static void die(const char *what)
{
	fprintf(stderr, "%s: %s\n", prog, what);
	exit(1);
}

static unsigned long get32(unsigned char *p)
{
	return p[0] | (p[1] << 8) | ((unsigned long) p[2] << 16)
						| ((unsigned long) p[3] << 24);
}

static void put32(unsigned char *p, unsigned long v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void list(void)
{
	int i;
	unsigned char *pe;

	for (i = 0; i < NR_PARTITIONS; i++) {
		pe = mbr + PART_TABLE_OFF + 16 * i;
		if (pe[4] == NO_PART) continue;
		printf("%d  type %02x  start %9lu  size %9lu  (%lu MB)\n",
			i + 1, pe[4], get32(pe + 8), get32(pe + 12),
			get32(pe + 12) / 2048);
	}
}

int main(int argc, char **argv)
{
	struct partition geom;
	unsigned long disk_secs, next, size, mb;
	unsigned char *pe;
	char *dev, *arg, *colon;
	int fd, i, a, lflag = 0, slot;

	prog = argv[0];
	if (argc > 1 && strcmp(argv[1], "-l") == 0) { lflag = 1; argv++; argc--; }
	if (argc < 2 || (!lflag && argc < 3)) {
		fprintf(stderr, "Usage: %s device type:MB|type:* ...\n"
				"       %s -l device\n", prog, prog);
		return 1;
	}
	dev = argv[1];
	if ((fd = open(dev, lflag ? O_RDONLY : O_RDWR)) < 0) die("cannot open device");
	if (read(fd, (char *) mbr, SECTOR) != SECTOR) die("cannot read MBR");
	if (mbr[510] != 0x55 || mbr[511] != 0xAA) die("no partition table");
	if (lflag) { list(); return 0; }

	if (ioctl(fd, DIOCGETP, &geom) < 0) die("cannot get device size");
	disk_secs = geom.size / SECTOR;
	if (disk_secs > LIMIT_SECS) disk_secs = LIMIT_SECS;

	/* Start after the end of the last partition in use. */
	next = ALIGN;
	slot = 0;
	for (i = 0; i < NR_PARTITIONS; i++) {
		pe = mbr + PART_TABLE_OFF + 16 * i;
		if (pe[4] == NO_PART) continue;
		if (get32(pe + 8) + get32(pe + 12) > next)
			next = get32(pe + 8) + get32(pe + 12);
		slot = i + 1;
	}

	for (a = 2; a < argc; a++, slot++) {
		if (slot >= NR_PARTITIONS) die("no free partition slot");
		arg = argv[a];
		if ((colon = strchr(arg, ':')) == NULL) die("bad argument");
		next = (next + ALIGN - 1) / ALIGN * ALIGN;
		if (next >= disk_secs) die("no space left on the device");
		if (strcmp(colon + 1, "*") == 0) {
			size = (disk_secs - next) / ALIGN * ALIGN;
		} else {
			mb = strtoul(colon + 1, NULL, 10);
			size = mb * ALIGN;
			if (size > disk_secs - next) die("partition does not fit");
		}
		if (size == 0) die("partition would be empty");
		pe = mbr + PART_TABLE_OFF + 16 * slot;
		memset(pe, 0, 16);
		pe[4] = (unsigned char) strtoul(arg, NULL, 16);
		/* CHS fields: "use LBA" values, as modern tools write. */
		pe[1] = pe[5] = 0xFE; pe[2] = pe[6] = 0xFF; pe[3] = pe[7] = 0xFF;
		put32(pe + 8, next);
		put32(pe + 12, size);
		next += size;
	}

	if (lseek(fd, 0L, SEEK_SET) != 0
	    || write(fd, (char *) mbr, SECTOR) != SECTOR) die("cannot write MBR");
	close(fd);
	sync();
	list();
	return 0;
}
