/*	mkpart - add partitions after the existing ones		(Flipper port)
 *
 * Usage: mkpart [-n sectors] device type:size[@end] ...
 *	  mkpart -l device
 *	  mkpart -s device
 *	  mkpart [-n sectors] -g N device
 *
 * Keeps the partitions already in the MBR (in particular the root file
 * system the system is running from) and fills the following empty slots
 * with new partitions, one per argument, each starting on a 4 MiB boundary
 * after the previous one (spec R12.32: the usual SD allocation unit, so
 * the card erases and rewrites whole units; sizes stay in MB).
 * type is hex (81 MINIX, 82 swap); size is in MB, "N%" for that share of
 * the space that is left, or "*" for everything that is left.  "@end"
 * puts the partition at the end of the card instead (on a 4 MiB
 * boundary); the arguments after it fill the space before it (spec
 * R12.34: swap at the end, "82:50%@end").  The MBR slots follow the
 * argument order, whatever the place on the card.  Partitions may start
 * and end anywhere on the card up to 2 TiB (R12.39: the driver counts in
 * sectors); a swap partition (82) stays below 4 GiB - 1 MiB in size (a
 * "%" or "*" size is cut to that), a MINIX one (81) may be larger (a V3
 * /home, spec O).  The kernel reads the new table at the next boot.
 *
 * -l lists the table, -s prints the device's size in MB, -g N makes
 * partition N reach up to the next partition after it (or the end of the
 * card), for setup -H (spec O.7).  -n sectors: as if the device had that
 * many sectors, and nothing is written (a dry run; prints the table).
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
#define SWAP_PART	0x82
#define ALIGN		8192UL			/* 4 MiB in sectors */
#define MB		2048UL			/* 1 MiB in sectors */
#define SWAP_SECS	(0x800000UL - MB)	/* 4 GiB - 1 MiB */

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
		printf("%d  type %02x  start %10lu  size %10lu  (%lu MB)\n",
			i + 1, pe[4], get32(pe + 8), get32(pe + 12),
			get32(pe + 12) / 2048);
	}
}

static unsigned long sizearg(char *sz, unsigned long type, unsigned long room)
{
	unsigned long size, mb;
	char *end;

	if (sz[0] == '*' && (sz[1] == 0 || sz[1] == '@')) {
		size = room / MB * MB;
		if (type == SWAP_PART && size > SWAP_SECS) size = SWAP_SECS;
		return size;
	}
	mb = strtoul(sz, &end, 10);
	if (*end == '%') {
		if (mb == 0 || mb > 100) die("bad share");
		size = room / 100 * mb / MB * MB;
		if (type == SWAP_PART && size > SWAP_SECS) size = SWAP_SECS;
		return size;
	}
	if (end == sz || (*end != 0 && *end != '@')) die("bad size");
	if (mb > room / MB) die("partition does not fit");
	size = mb * MB;
	if (type == SWAP_PART && size > SWAP_SECS)
		die("a swap partition is below 4 GiB");
	return size;
}

int main(int argc, char **argv)
{
	struct part_range geom;
	unsigned long disk_secs, next, limit, size, type, start, fake = 0;
	unsigned char *pe;
	char *dev, *arg, *colon, *at;
	int fd, i, a, lflag = 0, sflag = 0, grow = 0, slot;

	prog = argv[0];
	for (;;) {
		if (argc > 2 && strcmp(argv[1], "-n") == 0) {
			fake = strtoul(argv[2], NULL, 10);
			argv += 2; argc -= 2;
		} else if (argc > 2 && strcmp(argv[1], "-g") == 0) {
			grow = atoi(argv[2]);
			if (grow < 1 || grow > NR_PARTITIONS) die("bad -g");
			argv += 2; argc -= 2;
		} else if (argc > 1 && strcmp(argv[1], "-l") == 0) {
			lflag = 1; argv++; argc--;
		} else if (argc > 1 && strcmp(argv[1], "-s") == 0) {
			sflag = 1; argv++; argc--;
		} else break;
	}
	if (argc < 2 || (!lflag && !sflag && !grow && argc < 3)) {
		fprintf(stderr, "Usage: %s [-n sectors] device type:MB|N%%|*[@end] ...\n"
				"       %s -l device\n"
				"       %s -s device\n"
				"       %s [-n sectors] -g N device\n",
				prog, prog, prog, prog);
		return 1;
	}
	dev = argv[1];
	if ((fd = open(dev, (lflag || sflag || fake) ? O_RDONLY : O_RDWR)) < 0)
		die("cannot open device");
	if (read(fd, (char *) mbr, SECTOR) != SECTOR) die("cannot read MBR");
	if (mbr[510] != 0x55 || mbr[511] != 0xAA) die("no partition table");
	if (lflag) { list(); return 0; }

	if (ioctl(fd, DIOCGETPS, &geom) < 0) die("cannot get device size");
	disk_secs = fake ? fake : geom.count;	/* sectors (R12.39) */
	if (sflag) { printf("%lu\n", disk_secs / MB); return 0; }

	if (grow) {
		/* Partition N up to the next one that starts after it. */
		pe = mbr + PART_TABLE_OFF + 16 * (grow - 1);
		if (pe[4] == NO_PART) die("no such partition");
		start = get32(pe + 8);
		limit = disk_secs;
		for (i = 0; i < NR_PARTITIONS; i++) {
			unsigned char *q = mbr + PART_TABLE_OFF + 16 * i;
			if (i == grow - 1 || q[4] == NO_PART) continue;
			if (get32(q + 8) >= start && get32(q + 8) < limit)
				limit = get32(q + 8);
		}
		size = (limit - start) / MB * MB;
		if (pe[4] == SWAP_PART && size > SWAP_SECS) size = SWAP_SECS;
		if (size < get32(pe + 12)) size = get32(pe + 12);
		put32(pe + 12, size);
	} else {
		/* Start after the end of the last partition in use. */
		next = ALIGN;
		limit = disk_secs;
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
			if (next >= limit) die("no space left on the device");
			type = strtoul(arg, NULL, 16);
			at = strchr(colon + 1, '@');
			if (at != NULL && strcmp(at, "@end") != 0) die("bad place");
			size = sizearg(colon + 1, type, limit - next);
			if (size == 0) die("partition would be empty");
			if (at != NULL) {
				start = (limit - size) / ALIGN * ALIGN;
				if (start < next) die("partition does not fit");
				limit = start;
			} else {
				start = next;
				next += size;
			}
			pe = mbr + PART_TABLE_OFF + 16 * slot;
			memset(pe, 0, 16);
			pe[4] = (unsigned char) type;
			/* CHS fields: "use LBA" values, as modern tools write. */
			pe[1] = pe[5] = 0xFE; pe[2] = pe[6] = 0xFF; pe[3] = pe[7] = 0xFF;
			put32(pe + 8, start);
			put32(pe + 12, size);
		}
	}

	if (!fake && (lseek(fd, 0L, SEEK_SET) != 0
	    || write(fd, (char *) mbr, SECTOR) != SECTOR)) die("cannot write MBR");
	close(fd);
	if (!fake) sync();
	list();
	return 0;
}
