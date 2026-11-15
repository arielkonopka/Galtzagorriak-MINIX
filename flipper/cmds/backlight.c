/* backlight - the LCD backlight's brightness and idle timeout (Galtzagorriak
 * MINIX; owner request 2026-11-15).
 *
 * Usage:	backlight			show the settings and the state
 *		backlight LEVEL [SECONDS]	brightness 0..255; off after that
 *						many seconds without a key or
 *						button (0: never; default 300)
 *
 * The kernel turns the light off after the timeout and on again at any
 * key or button; the settings are kept over resets (an RTC backup
 * register).  A settings app will use the same /dev/backlight.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define BL_OFF		0x08000000UL	/* as hal.h HAL_BL_* */
#define BL_LEVEL(s)	((s) >> 16 & 0xFF)
#define BL_TIMEOUT(s)	((s) & 0xFFFF)

int main(int argc, char **argv)
{
	unsigned char b[3];
	unsigned long st;
	long level, secs;
	int fd, n;

	if (argc > 3 || (argc > 1 && (argv[1][0] < '0' || argv[1][0] > '9'))) {
		fprintf(stderr, "Usage: backlight [LEVEL [SECONDS]]\n");
		return 1;
	}
	if ((fd = open("/dev/backlight", argc > 1 ? O_RDWR : O_RDONLY)) < 0) {
		perror("backlight: /dev/backlight");
		return 1;
	}
	if (argc > 1) {
		level = atol(argv[1]);
		secs = argc > 2 ? atol(argv[2]) : -1;
		if (level < 0 || level > 255 || secs > 65535) {
			fprintf(stderr, "backlight: LEVEL 0..255, SECONDS 0..65535\n");
			return 1;
		}
		b[0] = (unsigned char) level;
		b[1] = (unsigned char) (secs & 0xFF);
		b[2] = (unsigned char) (secs >> 8 & 0xFF);
		n = secs >= 0 ? 3 : 1;
		if (write(fd, (char *) b, n) != n) {
			perror("backlight");
			return 1;
		}
	}
	if (read(fd, (char *) &st, 4) != 4) {
		perror("backlight");
		return 1;
	}
	printf("brightness %lu, ", BL_LEVEL(st));
	if (BL_TIMEOUT(st) == 0) printf("never off");
	else printf("off after %lu s without a key", BL_TIMEOUT(st));
	printf("; now %s\n", st & BL_OFF ? "off (idle)" : "on");
	return 0;
}
