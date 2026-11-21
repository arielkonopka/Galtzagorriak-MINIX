/* Region (spec Q, RQ.1, K.7): the radio region the Sub-GHz transmit rules
 * come from, read-only here as in stock's Region screen.  Changing it is
 * `region set CC' (root, asks, logs: RQ.4); this group only shows
 *	Set		the code in /etc/flipper/region (given to the kernel
 *			at the next boot by /etc/rc)
 *	Kernel		the region the kernel applies now (SGIOCGREGION)
 *	Transmit	whether the kernel allows any transmit (no region:
 *			receive only)
 * Each row is read when the window is made (SG_TEXT): nothing in RAM. */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <minix/subghz.h>
#include "sgroup.h"

#define FILE_RG	"/etc/flipper/region"

const struct srow region_rows[] = {
	{ "Set", S_TEXT, S_NOSAVE, 0, 0, 0, "", "" },
	{ "Kernel", S_TEXT, S_NOSAVE, 0, 0, 0, "", "" },
	{ "Transmit", S_TEXT, S_NOSAVE, 0, 0, 0, "", "" },
};

static int kernel(void)			/* 'C' << 8 | 'C', 0 none, -1 ? */
{
	int fd, v = 0;

	if ((fd = open("/dev/subghz", O_RDONLY)) < 0) return -1;
	if (ioctl(fd, SGIOCGREGION, &v) != 0) v = -1;
	close(fd);
	return v;
}

int region_fn(int op, int row, void *buf)
{
	char *v = buf;
	int fd, n = 0, k;

	if (op != SG_TEXT) return 0;
	strcpy(v, "none");
	if (row == 0) {
		if ((fd = open(FILE_RG, O_RDONLY)) >= 0) {
			n = (int) read(fd, v, 2);
			close(fd);
		}
		if (n == 2 && v[0] >= 'A' && v[0] <= 'Z' && v[1] >= 'A'
		    && v[1] <= 'Z') v[2] = 0;
		else strcpy(v, "none");
		return 0;
	}
	k = kernel();
	if (k < 0) strcpy(v, "?");
	else if (row == 1 && k != 0) {
		v[0] = (char) (k >> 8 & 0xFF);
		v[1] = (char) (k & 0xFF);
		v[2] = 0;
	} else if (row == 2) strcpy(v, k ? "region rules" : "off (rx only)");
	return 0;
}
