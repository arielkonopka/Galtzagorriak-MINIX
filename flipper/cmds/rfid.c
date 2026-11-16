/* rfid - read a 125 kHz tag (Galtzagorriak MINIX, spec R6.5; /dev/rfid)
 *
 * Usage:	rfid [-r] [-n TRIES]	read: print the tag's EM4100 number
 *					(as the stock app: 5 bytes, hex);
 *					-r: also the timings; up to TRIES
 *					reads of 0.5 s (default 4)
 *
 * /dev/rfid gives the times between the changes of the tag's signal (us,
 * 32-bit), the level of the first unknown.  EM4100 (EM4102, and blank
 * T5577 fobs in their default mode): Manchester, RF/64 (512 us a bit), 64
 * bits: nine 1s, ten rows of 4 data bits + even parity, 4 column parity
 * bits, a 0.  Other kinds (HID, Indala: FSK, PSK) are shown as a summary.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXD	512		/* ~3 EM frames; .bss under 4 KB (R2a.5) */
#define MAXH	(2 * MAXD)

static unsigned long d[MAXD];
static unsigned char h[MAXH];		/* half bits */

static int em_frame(const unsigned char *b, int n, unsigned char id[5])
{
	/* the 64 bits from b (0/1), checked; 1 if good */
	int r, c, i, p;

	if (n < 64) return 0;
	for (i = 0; i < 9; i++) if (b[i] != 1) return 0;
	for (r = 0; r < 10; r++) {
		p = 0;
		for (c = 0; c < 5; c++) p ^= b[9 + r * 5 + c];
		if (p) return 0;
	}
	for (c = 0; c < 4; c++) {
		p = b[59 + c];
		for (r = 0; r < 10; r++) p ^= b[9 + r * 5 + c];
		if (p) return 0;
	}
	if (b[63] != 0) return 0;
	memset(id, 0, 5);
	for (r = 0; r < 10; r++)
		for (c = 0; c < 4; c++)
			if (b[9 + r * 5 + c])
				id[r / 2] |= 1 << (7 - (r % 2) * 4 - c);
	return 1;
}

static int em_decode(int nd, unsigned char id[5])
{
	static unsigned char bits[MAXD];
	int lvl, nh, i, ph, inv, nb;

	for (lvl = 0; lvl < 2; lvl++) {
		int l = lvl;

		nh = 0;			/* half bits from the durations */
		for (i = 0; i < nd && nh < MAXH - 2; i++) {
			unsigned long t = d[i];
			int k = t < 128 ? 0 : t < 384 ? 1 : t < 700 ? 2 : 0;

			if (k == 0) { nh = 0; continue; }	/* not EM: again */
			while (k-- > 0) h[nh++] = l;
			l = !l;
		}
		for (ph = 0; ph < 2; ph++) {
			nb = 0;
			for (i = ph; i + 1 < nh; i += 2) {
				if (h[i] == h[i + 1]) { nb = -1; break; }
				bits[nb++] = h[i + 1];
			}
			if (nb < 0) continue;
			for (inv = 0; inv < 2; inv++) {
				int s;

				for (s = 0; s + 64 <= nb; s++) {
					unsigned char b[64];
					int j;

					for (j = 0; j < 64; j++)
						b[j] = bits[s + j] ^ inv;
					if (em_frame(b, 64, id)) return 1;
				}
			}
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	int fd, n, i, tries = 4, raw = 0, t;
	unsigned char id[5];

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-r") == 0) raw = 1;
		else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
			tries = atoi(argv[++i]);
		else {
			fprintf(stderr, "usage: rfid [-r] [-n TRIES]\n");
			return 2;
		}
	}
	if ((fd = open("/dev/rfid", O_RDONLY)) < 0) {
		perror("rfid: /dev/rfid");
		return 1;
	}
	for (t = 0; t < tries; t++) {
		if ((n = read(fd, d, sizeof(d))) < 0) {
			perror("rfid");
			return 1;
		}
		n /= 4;
		if (raw) {
			printf("%d:", n);
			for (i = 0; i < n && i < 200; i++) printf(" %lu", d[i]);
			printf("\n");
		}
		if (em_decode(n, id)) {
			printf("EM4100 %02X %02X %02X %02X %02X\n",
				id[0], id[1], id[2], id[3], id[4]);
			return 0;
		}
	}
	if (n == 0) printf("no tag (no signal)\n");
	else {
		unsigned long lo = ~0UL, hi = 0, sum = 0;

		for (i = 0; i < n; i++) {
			if (d[i] < lo) lo = d[i];
			if (d[i] > hi) hi = d[i];
			sum += d[i];
		}
		printf("no EM4100; %d changes, %lu-%lu us, mean %lu us\n",
			n, lo, hi, sum / n);
	}
	return 1;
}
