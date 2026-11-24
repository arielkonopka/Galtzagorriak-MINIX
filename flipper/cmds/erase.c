/*	erase - tell the SD card a partition is empty	(Flipper port)
 *
 * Usage: erase [-q] /dev/hdN
 *
 * Spec R12.33: sends ERASE (CMD32/CMD33/CMD38) over the whole partition,
 * so a card that was once full (a camera's, a phone's) gets its whole free
 * pool back for wear levelling.  Everything on the partition is gone; run
 * mkfs after.  /etc/setup runs it over /usr, swap and /home on the first
 * boot, before mkfs.  It is an install-time step: never at run time.
 *
 * Root only (the DIOCERASE ioctl, on a device only root may open).  It
 * refuses the whole card (/dev/hd0), a partition that is mounted or holds
 * a mounted subpartition, and the swap partition while swap is on (EBUSY).
 * -q: no message when the card or the driver cannot erase (ENOTTY), only
 * the exit status 2, so setup goes on without it.
 * Exit status: 0 erased, 1 error, 2 the card or driver cannot erase.
 */
#include <lib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <minix/callnr.h>
#include <minix/partition.h>

#define SECTOR		512
#define MINOR_HD1A	128		/* drvlib: hd1a, hd1b, ... */
#define WINI_MAJOR	3

static char *prog;

static void die(const char *what, int err)
{
	fprintf(stderr, "%s: %s: %s\n", prog, what, strerror(err));
	exit(1);
}

/* Which primary partition (1-4) a minor of the SD card is in; 0: the
 * whole card. */
static int primary(int minor)
{
	if (minor >= MINOR_HD1A) return (minor - MINOR_HD1A) / 4 % 4 + 1;
	return minor;
}

/* Do the two minors of the card share sectors? */
static int overlap(int a, int b)
{
	if (a == b || primary(a) == 0 || primary(b) == 0) return 1;
	if (primary(a) != primary(b)) return 0;
	return a < MINOR_HD1A || b < MINOR_HD1A;   /* one holds the other */
}

static int busy_dev(dev_t d, dev_t target)
{
	return ((d >> 8) & 0xFF) == WINI_MAJOR
			&& overlap(d & 0xFF, target & 0xFF);
}

/* Is anything on the target's sectors in use: a file system mounted from
 * it (or from a part of it), or swap? */
static int in_use(dev_t target)
{
	static char *dirs[] = { "/", "/usr", "/home", "/tmp", "/mnt", NULL };
	char line[128], dev[64];
	struct stat st;
	struct swinfo si;
	message m;
	FILE *f;
	char **p;

	for (p = dirs; *p != NULL; p++)
		if (stat(*p, &st) == 0 && busy_dev(st.st_dev, target))
			return 1;
	if ((f = fopen("/etc/mtab", "r")) != NULL) {
		while (fgets(line, sizeof(line), f) != NULL) {
			if (sscanf(line, "%63s", dev) == 1 && stat(dev, &st) == 0
			    && S_ISBLK(st.st_mode) && busy_dev(st.st_rdev, target)) {
				fclose(f);
				return 1;
			}
		}
		fclose(f);
	}
	m.m1_i1 = 0;
	m.m1_i2 = MI_SWAPINFO;
	m.m1_i3 = 0;
	m.m1_p1 = (char *) &si;
	if (_syscall(MM, MEMINFO, &m) >= 0 && (si.mode & 0x7F) != SW_OFF
			&& si.dev != 0 && busy_dev((dev_t) si.dev, target))
		return 1;
	return 0;
}

int main(int argc, char **argv)
{
	struct part_range rg;
	struct stat st;
	char *dev;
	int fd, quiet = 0;

	prog = argv[0];
	if (argc > 1 && strcmp(argv[1], "-q") == 0) { quiet = 1; argv++; argc--; }
	if (argc != 2) {
		fprintf(stderr, "Usage: %s [-q] /dev/hdN\n", prog);
		return 1;
	}
	dev = argv[1];
	if (getuid() != 0 && geteuid() != 0) die(dev, EPERM);
	if (stat(dev, &st) < 0) die(dev, errno);
	if (!S_ISBLK(st.st_mode) || ((st.st_rdev >> 8) & 0xFF) != WINI_MAJOR)
		die(dev, ENODEV);		/* SD card partitions only */
	if (primary(st.st_rdev & 0xFF) == 0 || in_use(st.st_rdev))
		die(dev, EBUSY);
	if ((fd = open(dev, O_RDWR)) < 0) die(dev, errno);
	if (ioctl(fd, DIOCGETPS, &rg) < 0) die(dev, errno);	/* R12.39 */
	rg.first = 0;			/* from the partition's start */
	if (rg.count == 0) die(dev, EINVAL);
	if (ioctl(fd, DIOCERASE, &rg) < 0) {
		if (errno == ENOTTY) {
			if (!quiet) fprintf(stderr,
				"%s: %s: this card or driver cannot erase\n",
				prog, dev);
			return 2;
		}
		die(dev, errno);
	}
	close(fd);
	return 0;
}
