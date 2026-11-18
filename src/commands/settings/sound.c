/* Sound (spec K.7): the system's speaker volume, through /dev/speaker's
 * default-volume note (0xFFFE: kept by the kernel in a backup register,
 * used whenever a program has not set its own), and a test beep. */
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include "sgroup.h"

const struct srow sound_rows[] = {
	{ "Volume", S_INT, 0, 0, 100, 10, "%", "" },
	{ "Test beep", S_ACT, S_NOSAVE, 0, 0, 0, "", "" },
};

static void notes(const unsigned short *n, int k)
{
	unsigned char b[4 * 4];
	int fd, i;

	for (i = 0; i < k; i++) {
		b[4 * i] = (unsigned char) (n[2 * i] & 0xFF);
		b[4 * i + 1] = (unsigned char) (n[2 * i] >> 8);
		b[4 * i + 2] = (unsigned char) (n[2 * i + 1] & 0xFF);
		b[4 * i + 3] = (unsigned char) (n[2 * i + 1] >> 8);
	}
	if ((fd = open("/dev/speaker", O_WRONLY)) < 0) return;
	(void) write(fd, (char *) b, 4 * k);
	close(fd);
}

static void apply(int row)
{
	unsigned short n[8];

	n[0] = 0xFFFE;			/* the default volume, then a sound */
	n[1] = (unsigned short) sg_val[0];
	if (row == 0) {			/* a short tick at the new level */
		n[2] = 1000; n[3] = 40;
		notes(n, 2);
	} else if (row == 1) {		/* the test: three notes */
		n[2] = 880; n[3] = 120;
		n[4] = 1175; n[5] = 120;
		n[6] = 1760; n[7] = 200;
		notes(n, 4);
	} else
		notes(n, 1);		/* only the volume (at boot) */
}

int sound_fn(int op, int row, void *buf)
{
	(void) buf;
	if (op == SG_LOAD) {		/* the file's value (read before), or
					 * 100 (sg_val[] starts at -1) */
		if (sg_val[0] < 0) sg_val[0] = 100;
	} else if (op == SG_APPLY) apply(row);
	return 0;
}
