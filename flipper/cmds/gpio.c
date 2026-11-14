/* gpio - the Flipper's GPIO header pins (Galtzagorriak MINIX)
 *
 * Usage:	gpio			levels of all eight pins
 *		gpio PIN		level of one pin (prints 0 or 1)
 *		gpio PIN MODE		MODE: 0 or 1 (output low/high), in, up,
 *					down (input, pulls), off (high impedance)
 *
 * PIN is the header pin number (2 3 4 5 6 7 15 16) or the port name
 * (pa7 pa6 pa4 pb3 pb2 pc3 pc1 pc0).  Uses /dev/gpio (kernel/arm/flipdev.c).
 */
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *hdr[8] = { "2", "3", "4", "5", "6", "7", "15", "16" };
static char *port[8] = { "pa7", "pa6", "pa4", "pb3", "pb2", "pc3", "pc1", "pc0" };
static char *mode[6] = { "0", "1", "in", "up", "down", "off" };

static void usage(void)
{
	fprintf(stderr, "Usage: gpio [PIN [0|1|in|up|down|off]]\n"
		"  PIN: header pin 2 3 4 5 6 7 15 16, or pa7 pa6 pa4 pb3 pb2 "
		"pc3 pc1 pc0\n");
	exit(1);
}

static int lookup(char **tab, int n, char *s)
{
	int i;
	char lc[8];

	for (i = 0; s[i] != 0 && i < 7; i++)
		lc[i] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] - 'A' + 'a' : s[i];
	lc[i] = 0;
	for (i = 0; i < n; i++) if (strcmp(tab[i], lc) == 0) return i;
	return -1;
}

int main(int argc, char **argv)
{
	int fd, pin = -1, op, i;
	char lv[8];
	unsigned char cmd[2];

	if (argc > 3) usage();
	if (argc >= 2) {
		if ((pin = lookup(hdr, 8, argv[1])) < 0
				&& (pin = lookup(port, 8, argv[1])) < 0)
			usage();
	}
	if ((fd = open("/dev/gpio", argc == 3 ? O_WRONLY : O_RDONLY)) < 0) {
		perror("gpio: /dev/gpio");
		return 1;
	}
	if (argc == 3) {
		if ((op = lookup(mode, 6, argv[2])) < 0) usage();
		cmd[0] = pin;
		cmd[1] = op;
		if (write(fd, (char *) cmd, 2) != 2) {
			perror("gpio");
			return 1;
		}
		return 0;
	}
	if (read(fd, lv, 8) != 8) {
		perror("gpio");
		return 1;
	}
	if (pin >= 0) {
		printf("%c\n", lv[pin]);
		return 0;
	}
	for (i = 0; i < 8; i++)
		printf("%-3s (pin %2s)  %c\n", port[i], hdr[i], lv[i]);
	return 0;
}
