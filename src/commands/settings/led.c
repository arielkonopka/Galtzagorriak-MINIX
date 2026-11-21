/* LED (spec K.7, R7b.5): how notifications use the RGB LED, as stock's
 * LED settings.  Brightness scales the colours of a notification's light
 * (timed's alert writes "PATTERN #rrggbb" to /dev/led), Notify LED turns
 * that light off altogether, Test shows white at the brightness for a
 * moment.  Both are kept in /etc/flipper/led.conf on the card (written only
 * when the user changes them, on leaving the group); nothing is applied
 * at boot, the reader is the alert at each ring. */
#include <sys/types.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include "sgroup.h"

const struct srow led_rows[] = {
	{ "Brightness", S_INT, 0, 10, 100, 10, "%", "" },
	{ "Notify LED", S_ENUM, 0, 0, 1, 1, "", "on|off" },
	{ "Test", S_ACT, S_NOSAVE, 0, 0, 0, "", "" },
};

enum { R_BRIGHT, R_NOTIFY, R_TEST };

static void rgb(int v)
{
	unsigned char c[3];
	int fd;

	c[0] = c[1] = c[2] = (unsigned char) v;
	if ((fd = open("/dev/led", O_WRONLY)) < 0) return;
	(void) write(fd, (char *) c, 3);
	close(fd);
}

int led_fn(int op, int row, void *buf)
{
	(void) buf;
	if (op == SG_LOAD) {
		if (sg_val[R_BRIGHT] < 0) sg_val[R_BRIGHT] = 100;
		if (sg_val[R_NOTIFY] < 0) sg_val[R_NOTIFY] = 0;
	} else if (op == SG_APPLY && row == R_TEST) {
		rgb((sg_val[R_BRIGHT] * 255 + 50) / 100);
		(void) poll((struct pollfd *) 0, 0, 300);
		rgb(0);
	}
	return 0;
}
