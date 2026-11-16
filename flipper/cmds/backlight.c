/* backlight - the LCD backlight's brightness and idle timeout (Galtzagorriak
 * MINIX; owner request 2026-11-15).
 *
 * Usage:	backlight			show the settings and the state
 *		backlight force off|on|auto	the forced mode (R6.10; the
 *						same as the Left+Up / Right+Down
 *						gestures), until the next boot
 *		backlight LEVEL [SECONDS]	brightness 0..255; off after that
 *						many seconds without a key or
 *						button (0: never; default 15;
 *						10 % dimmer each second of
 *						the last 5)
 *
 * The kernel turns the light off after the timeout and on again at any
 * key or button; the settings are kept over resets (an RTC backup
 * register).  A settings app will use the same /dev/backlight.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BL_OFF		0x08000000UL	/* as hal.h HAL_BL_* */
#define BL_STEP(s)	((s) >> 24 & 7)
#define BL_LEVEL(s)	((s) >> 16 & 0xFF)
#define BL_TIMEOUT(s)	((s) & 0x3FFF)
#define BL_MODE(s)	((s) >> 14 & 3)

int main(int argc, char **argv)
{
	unsigned char b[4];
	unsigned long st;
	long level, secs;
	int fd, n;

	if (argc == 3 && strcmp(argv[1], "force") == 0) {
		int m = strcmp(argv[2], "off") == 0 ? 1 : strcmp(argv[2], "on")
				== 0 ? 2 : strcmp(argv[2], "auto") == 0 ? 0 : -1;

		if (m < 0 || (fd = open("/dev/backlight", O_RDWR)) < 0
				|| read(fd, (char *) &st, 4) != 4) {
			fprintf(stderr, "backlight: force off|on|auto (root)\n");
			return 1;
		}
		b[0] = BL_LEVEL(st);
		b[1] = BL_TIMEOUT(st) & 0xFF;
		b[2] = BL_TIMEOUT(st) >> 8;
		b[3] = m;
		lseek(fd, 0L, SEEK_SET);
		if (write(fd, (char *) b, 4) != 4) {
			perror("backlight");
			return 1;
		}
		close(fd);
		argc = 1;
	}
	if (argc > 3 || (argc > 1 && (argv[1][0] < '0' || argv[1][0] > '9'))) {
		fprintf(stderr, "Usage: backlight [LEVEL [SECONDS]] | force "
			"off|on|auto\n");
		return 1;
	}
	if ((fd = open("/dev/backlight", argc > 1 ? O_RDWR : O_RDONLY)) < 0) {
		perror("backlight: /dev/backlight");
		return 1;
	}
	if (argc > 1) {
		level = atol(argv[1]);
		secs = argc > 2 ? atol(argv[2]) : -1;
		if (level < 0 || level > 255 || secs > 16383) {
			fprintf(stderr, "backlight: LEVEL 0..255, SECONDS 0..16383\n");
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
	if (BL_MODE(st) == 1) printf("; forced off");
	else if (BL_MODE(st) == 2) printf("; forced on");
	if (st & BL_OFF) printf("; now off\n");
	else if (BL_STEP(st)) printf("; now dimmed %lu %% (idle)\n",
						100 - 10 * BL_STEP(st));
	else printf("; now on\n");
	return 0;
}
