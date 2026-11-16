/* rfid - read a 125 kHz tag (Galtzagorriak MINIX, spec R6.5; /dev/rfid)
 *
 * Usage:	rfid [-r] [-n TRIES]	read: print the tag's EM4100 number
 *					(as the stock app: 5 bytes, hex);
 *					-r: also the timings; up to TRIES
 *					reads of 0.5 s (default 4)
 *		rfid write EM B0 B1 B2 B3 B4
 *					make a blank T5577 tag (fob, card)
 *					an EM4100 with that number, then
 *					read it back
 *
 * Writing a T5577 is done by gaps in the 125 kHz field, with the stock
 * firmware's timings (lib/lfrfid/tools/t5577.c, in carrier periods of
 * 8 us): block 0 the configuration (EM4100: RF/64, Manchester, 2 blocks),
 * blocks 1 and 2 the 64 bits as the tag sends them.
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

/* The comparator's edges one way are exact (the envelope crosses the
 * threshold steeply there), the other way they drift while it settles:
 * so only the periods between edges of one kind are used, each 2, 3 or 4
 * half bits (256 us).  Within a period the level is first one way, then
 * the other; in Manchester a run of two half bits must straddle a bit
 * boundary, which decides a period of 3 by where it starts in the bit
 * grid (2 and 4 are clear).  Both edge kinds, both grids and both
 * polarities are tried; the frame's parities pick the right one. */
static int em_decode(int nd, unsigned char id[5])
{
	static unsigned char bits[MAXH / 2];
	int off, grid, i, nh, pos, nb, inv, s, j;

	for (off = 0; off < 2; off++)
	for (grid = 0; grid < 2; grid++) {
		nh = 0;
		pos = grid;			/* the half bit's place in a bit */
		for (i = off; i + 1 < nd && nh < MAXH - 4; i += 2) {
			unsigned long t = d[i] + d[i + 1];
			int p = (int) ((t + 128) / 256), a, b;

			if (p < 2 || p > 4 || t < 400 || t > 1150) {
				nh = 0;		/* not EM: start again */
				pos = grid;
				continue;
			}
			if (p == 2) a = b = 1;
			else if (p == 4) {
				if ((pos & 1) == 0) { nh = 0; pos = grid; continue; }
				a = b = 2;
			} else if (pos & 1) { a = 2; b = 1; }
			else { a = 1; b = 2; }
			while (a-- > 0) h[nh++] = 1;
			while (b-- > 0) h[nh++] = 0;
			pos += p;
		}
		/* pairs from the grid: h[0] is at place grid */
		nb = 0;
		for (i = grid ? 1 : 0; i + 1 < nh; i += 2) {
			if (h[i] == h[i + 1]) { nb = 0; continue; }
			bits[nb++] = h[i + 1];
		}
		for (inv = 0; inv < 2; inv++)
			for (s = 0; s + 64 <= nb; s++) {
				unsigned char b64[64];

				for (j = 0; j < 64; j++) b64[j] = bits[s + j] ^ inv;
				if (em_frame(b64, 64, id)) return 1;
			}
	}
	return 0;
}

/* ---- writing a T5577 ---- */
#define T_WAIT		400
#define T_START_GAP	30
#define T_WRITE_GAP	18
#define T_DATA_0	24
#define T_DATA_1	56
#define T_PROGRAM	700
#define T5577_EM4100	0x00148040UL	/* RF/64, Manchester, blocks 1-2 */

static int nt;			/* durations in d[], field on first */

static void put(int on, unsigned long tc)
{
	unsigned long us = tc * 8;

	if (nt > 0 && ((nt - 1) & 1) == !on) {	/* the same state: longer */
		d[nt - 1] += us;
		return;
	}
	if (nt < MAXD) d[nt++] = us;
}

static void wbit(int b)
{
	put(1, b ? T_DATA_1 : T_DATA_0);
	put(0, T_WRITE_GAP);
}

static void wreset(void)
{
	put(0, T_START_GAP);
	wbit(1);
	wbit(0);
}

static void wblock(int blk, unsigned long data)
{
	int i;

	put(1, T_WAIT);
	put(0, T_START_GAP);
	wbit(1); wbit(0);			/* opcode 10: page 0 */
	wbit(0);				/* not locked */
	for (i = 31; i >= 0; i--) wbit((int) (data >> i) & 1);
	for (i = 2; i >= 0; i--) wbit((blk >> i) & 1);
	put(1, T_PROGRAM + T_WAIT);
	wreset();
}

static int write_em(unsigned char id[5])
{
	unsigned char b[64];
	unsigned long blk[2];
	int r, c, p, i, fd, n, t;
	unsigned char got[5];

	for (i = 0; i < 9; i++) b[i] = 1;
	for (r = 0; r < 10; r++) {
		int nib = id[r / 2] >> (r % 2 ? 0 : 4) & 15;

		p = 0;
		for (c = 0; c < 4; c++) {
			b[9 + r * 5 + c] = nib >> (3 - c) & 1;
			p ^= b[9 + r * 5 + c];
		}
		b[9 + r * 5 + 4] = p;
	}
	for (c = 0; c < 4; c++) {
		p = 0;
		for (r = 0; r < 10; r++) p ^= b[9 + r * 5 + c];
		b[59 + c] = p;
	}
	b[63] = 0;
	blk[0] = blk[1] = 0;
	for (i = 0; i < 64; i++) blk[i / 32] = blk[i / 32] << 1 | b[i];
	nt = 0;
	wblock(0, T5577_EM4100);
	wblock(1, blk[0]);
	wblock(2, blk[1]);
	wreset();
	put(1, T_WAIT);				/* odd: ends with the field */
	if ((fd = open("/dev/rfid", O_RDWR)) < 0) {
		perror("rfid: /dev/rfid");
		return 1;
	}
	if (write(fd, d, nt * 4) != nt * 4) {
		perror("rfid: write");
		return 1;
	}
	for (t = 0; t < 4; t++) {		/* read it back */
		if ((n = read(fd, d, sizeof(d))) < 0) break;
		if (em_decode(n / 4, got)) {
			if (memcmp(got, id, 5) == 0) {
				printf("written: EM4100 %02X %02X %02X %02X %02X\n",
					id[0], id[1], id[2], id[3], id[4]);
				return 0;
			}
			printf("the tag reads EM4100 %02X %02X %02X %02X %02X: "
				"not written (locked, or not a T5577?)\n",
				got[0], got[1], got[2], got[3], got[4]);
			return 1;
		}
	}
	printf("written, but it does not read back as EM4100\n");
	return 1;
}

int main(int argc, char **argv)
{
	int fd, n, i, tries = 4, raw = 0, t;
	unsigned char id[5];

	if (argc == 8 && strcmp(argv[1], "write") == 0
				&& strcmp(argv[2], "EM") == 0) {
		for (i = 0; i < 5; i++) {
			char *e;
			unsigned long v = strtoul(argv[3 + i], &e, 16);

			if (*e != 0 || v > 255) goto use;
			id[i] = (unsigned char) v;
		}
		return write_em(id);
	}
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-r") == 0) raw = 1;
		else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
			tries = atoi(argv[++i]);
		else {
use:			fprintf(stderr, "usage: rfid [-r] [-n TRIES]\n"
				"       rfid write EM B0 B1 B2 B3 B4\n");
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
