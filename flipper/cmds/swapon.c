/*	swapon - swap to the SD card			(Flipper port)
 *
 *	swapon				show the swap state
 *	swapon device [plain|crypt] [blk]
 *					swap to that partition (root); blk:
 *					swappable blocks for apps too (12h)
 *	swapon device blk		blocks only, no swap (12h, R12.38)
 *	swapon off			no more swap (root; nothing swapped)
 *	swapon -r kb			the idle reserve (root; 0: none)
 *	swapon -o pid			test: swap pid out now (root)
 *	swapon -X			test: act as if the card were pulled
 *
 * Spec Phase 12 (R12.1, R12.5, R12.28).  The device must be a primary
 * partition of the SD card of type 82 (/etc/setup makes /dev/hd3 one):
 * swap never goes to internal flash.  Until encryption (12b) lands the
 * only mode is plain: no encryption, a CRC-32 against card errors, and
 * set-uid programs and Secret: yes packages are never swapped.  /etc/rc
 * runs it from swap= in /etc/rc.conf.
 */
#include <lib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <minix/callnr.h>

static int mm(int req, int i1, int i3, void *p)
{
	message m;

	m.m1_i1 = i1;
	m.m1_i2 = req;
	m.m1_i3 = i3;
	m.m1_p1 = p;
	return _syscall(MM, MEMINFO, &m);
}

static void usage(void)
{
	fprintf(stderr, "Usage: swapon [device [plain|crypt] | off | -r kb"
				" | -o pid | -X]\n"
		"       swapon device [plain|crypt] blk\n");
	exit(1);
}

static int show(void)
{
	struct swinfo si;
	int i;

	if (mm(MI_SWAPINFO, 0, 0, &si) < 0) {
		perror("swapon");
		return 1;
	}
	printf("swap: %s", (si.mode & 0x7F) == SW_PLAIN ? "plain"
		: (si.mode & 0x7F) == SW_CRYPT ? "crypt" : "off");
	if (si.mode & 0x80) printf(" (lost: SD card removed or failing)");
	if (si.dev != 0)
		printf(", device %d/%d", (si.dev >> 8) & 0xFF, si.dev & 0xFF);
	printf(", reserve %u KB, %d of 16 slots used, %u out, %u in\n",
		si.res_kb, si.nproc, si.nout, si.nin);
	for (i = 0; i < si.nproc; i++)
		printf("  swapped: pid %5d %-8.8s %6lu bytes (slot %d)\n",
			si.p[i].pid, si.p[i].name, si.p[i].bytes, si.p[i].slot);
	return 0;
}

int main(int argc, char **argv)
{
	struct stat sb;
	int mode = SW_PLAIN, blk = 0;

	if (argc == 1) return show();
	if (strcmp(argv[1], "-r") == 0) {
		if (argc != 3) usage();
		if (mm(MI_SWAPRES, atoi(argv[2]), 0, NULL) < 0) {
			perror("swapon -r");
			return 1;
		}
		return 0;
	}
	if (strcmp(argv[1], "-o") == 0) {
		if (argc != 3) usage();
		if (mm(MI_SWAPTEST, 1, atoi(argv[2]), NULL) < 0) {
			perror("swapon -o");
			return 1;
		}
		return 0;
	}
	if (strcmp(argv[1], "-X") == 0) {
		if (mm(MI_SWAPTEST, 2, 0, NULL) < 0) {
			perror("swapon -X");
			return 1;
		}
		return 0;
	}
	if (strcmp(argv[1], "off") == 0) {
		if (mm(MI_SWAPON, 0, SW_OFF, NULL) < 0) {
			perror("swapon off");
			return 1;
		}
		return 0;
	}
	if (argc > 2 && strcmp(argv[argc - 1], "blk") == 0) {
		blk = SW_BLK;		/* R12.38: blocks on */
		if (--argc == 2) mode = SW_OFF;	/* blocks only */
	}
	if (argc > 3) usage();
	if (argc == 3) {
		if (strcmp(argv[2], "plain") == 0) mode = SW_PLAIN;
		else if (strcmp(argv[2], "crypt") == 0) mode = SW_CRYPT;
		else usage();
	}
	if (stat(argv[1], &sb) < 0 || !S_ISBLK(sb.st_mode)) {
		fprintf(stderr, "swapon: %s: not a block device\n", argv[1]);
		return 1;
	}
	if (mm(MI_SWAPON, (int) sb.st_rdev, mode | blk, NULL) < 0) {
		if (errno == ENOSYS)
			fprintf(stderr, "swapon: crypt needs swap encryption, "
				"which is not there yet: swap stays off\n");
		else if (errno == EBUSY)
			fprintf(stderr, "swapon: busy: processes are swapped "
				"or hold blocks\n");
		else if (errno == EINVAL)
			fprintf(stderr, "swapon: %s: not an SD card partition "
				"of type 82\n", argv[1]);
		else
			perror("swapon");
		return 1;
	}
	return 0;
}
