/* sgfake - play a Sub-GHz remote from a recorded capture (QEMU only;
 * issue #302, tools/nattest.py subghz).
 *
 * Usage:	sgfake FILE
 *
 * Reads the RAW_Data lines of a .sub RAW capture and hands them, as
 * received frames, to the program reading /dev/subghzraw (subghz rx):
 * before each write SGIOCFAKE (<minix/subghz.h>) makes the kernel give
 * the frame to the waiting reader instead of sending it.  Levels of the
 * same sign are merged, a frame starts with the carrier on and ends
 * after a silence of at least GAP us or when FRAME durations are full.
 * Nobody reading: it retries for 5 s, then exit status 1.  On the
 * Flipper the ioctl is ENOTTY: exit status 1.
 */
#include "t.h"
#include <sys/ioctl.h>
#include <poll.h>
#include <minix/subghz.h>

#define FRAME	512		/* the reader's buffer (subghz rx) */
#define GAP	10000		/* us: a silence that ends a frame */

static unsigned long frame[FRAME];
static int nframe, fd;
static long sent;

static int flush(void)
{
	int v, n, tries;

	if (nframe == 0) return 0;
	for (tries = 0; tries < 100; tries++) {
		v = SG_FAKE_RAW;
		if (ioctl(fd, SGIOCFAKE, &v) < 0) return -1;
		n = write(fd, (char *) frame, nframe * 4);
		if (n == nframe * 4) {
			nframe = 0;
			sent++;
			return 0;
		}
		if (n >= 0 || errno != EAGAIN) return -1;
		(void) poll((struct pollfd *) NULL, 0, 50);	/* not read yet */
	}
	errno = EAGAIN;
	return -1;
}

static int put(long v)
{
	int on = v > 0;
	unsigned long us = v > 0 ? v : -v;

	if (us == 0) return 0;
	if (us > SG_DURATION_MAX) us = SG_DURATION_MAX;
	if (nframe == 0 && !on) return 0;		/* off first: nothing */
	if (((nframe - 1) & 1) == !on) {		/* the same level */
		frame[nframe - 1] += us;
		if (frame[nframe - 1] > SG_DURATION_MAX)
			frame[nframe - 1] = SG_DURATION_MAX;
		return 0;
	}
	frame[nframe++] = us;
	if (!on && (us >= GAP || nframe == FRAME)) return flush();
	return 0;
}

int main(int argc, char **argv)
{
	static const char tag[] = "RAW_Data:";
	FILE *f;
	int c, col = 0, raw = 0, neg = 0, digits = 0;
	long v = 0;

	if (argc != 2) {
		printf("usage: sgfake FILE\n");
		return 2;
	}
	if ((f = fopen(argv[1], "r")) == NULL) {
		printf("sgfake: %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	if ((fd = open("/dev/subghzraw", O_WRONLY)) < 0) {
		printf("sgfake: /dev/subghzraw: %s\n", strerror(errno));
		return 1;
	}
	while ((c = getc(f)) != EOF) {
		if (raw && (c == '-' || (c >= '0' && c <= '9'))) {
			if (c == '-') neg = 1;
			else {
				v = v * 10 + (c - '0');
				digits = 1;
			}
		} else {
			if (digits && put(neg ? -v : v) < 0) break;
			v = 0;
			neg = digits = 0;
		}
		if (c == '\n') {
			col = 0;
			raw = 0;
		} else if (!raw && col >= 0) {
			if (c == tag[col]) {
				if (tag[++col] == 0) raw = 1;
			} else col = -1;
		}
	}
	if (c == EOF && digits) (void) put(neg ? -v : v);
	if (c == EOF && nframe > 0 && (nframe & 1) == 1)
		frame[nframe++] = GAP;		/* end with the carrier off */
	if (c != EOF || flush() < 0) {
		printf("sgfake: %s\n", strerror(errno));
		return 1;
	}
	printf("sgfake: %ld frames\n", sent);
	return 0;
}
