/* beep - play notes on the Flipper's speaker (Galtzagorriak MINIX)
 *
 * Usage:	beep			a short beep (1 kHz, 100 ms)
 *		beep HZ MS [HZ MS]...	notes; HZ 0 is a rest
 *		beep -c			call for attention: three rising beeps
 *		beep -v VOL ...		the same at volume VOL, 0..100
 *
 * Uses /dev/speaker (kernel/arm/flipdev.c): 16-bit frequency and duration
 * per note, little endian; the write returns when the notes have played.
 * A note of 0xFFFF Hz sets the volume to its duration field.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXN	64

static unsigned char buf[4 * MAXN];
static int n;

static void note(unsigned hz, unsigned ms)
{
	if (n == MAXN) return;
	buf[4*n] = hz & 0xFF;
	buf[4*n+1] = hz >> 8;
	buf[4*n+2] = ms & 0xFF;
	buf[4*n+3] = ms >> 8;
	n++;
}

int main(int argc, char **argv)
{
	int fd, i;

	if (argc >= 3 && strcmp(argv[1], "-v") == 0) {
		note(0xFFFF, (unsigned) atoi(argv[2]));	/* the volume */
		argv += 2;
		argc -= 2;
	}
	if (argc == 1) {
		note(1000, 100);
	} else if (argc == 2 && strcmp(argv[1], "-c") == 0) {
		for (i = 0; i < 3; i++) {
			note(1500, 120); note(2000, 120); note(2500, 200);
			note(0, 400);
		}
	} else if (argc % 2 == 1) {
		for (i = 1; i + 1 < argc; i += 2)
			note((unsigned) atoi(argv[i]), (unsigned) atoi(argv[i+1]));
	} else {
		fprintf(stderr, "Usage: beep [-v VOL] [-c | HZ MS [HZ MS]...]\n");
		return 1;
	}
	if ((fd = open("/dev/speaker", O_WRONLY)) < 0) {
		perror("beep: /dev/speaker");
		return 1;
	}
	if (write(fd, (char *) buf, 4 * n) != 4 * n) {
		perror("beep");
		return 1;
	}
	return 0;
}
