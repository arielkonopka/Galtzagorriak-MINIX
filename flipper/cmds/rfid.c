/* rfid - read a 125 kHz tag (Galtzagorriak MINIX, spec R6.5; /dev/rfid)
 *
 * Usage:	rfid [-r] [-n TRIES]	read: print the tag's EM4100 number
 *					(as the stock app: 5 bytes, hex);
 *					-r: also the timings; up to TRIES
 *					reads of 0.5 s (default 4)
 *		rfid write [OPTS] EM B0 B1 B2 B3 B4
 *					make a blank tag (fob, card) an
 *					EM4100 with that number: as T5577,
 *					else as EM4305 (as stock), read back
 *		rfid copy [OPTS]	read a tag, then write its number
 *					to the blank one put on next
 *		  -n TRIES		rounds, default 20 (a tag that
 *					wants a password may need many)
 *		  -pass HEX		also give the T5577 a password, so
 *					nothing rewrites it without that
 *					word (some door entry systems try
 *					to overwrite a copy they find).
 *					Write it down: it is needed to
 *					change the tag afterwards
 *		  -with HEX		the password the tag has now, to
 *					change a tag written with -pass (it
 *					keeps the password unless -pass
 *					gives a new one)
 *		  -lock			the T5577 lock bit instead: the
 *					blocks can then never be written
 *					again, by anyone, you included
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
#define T_START_GAP	50		/* stock 30; 8-50 allowed: the field
				 * decays slowly (seen with the comparator) */
#define T_WRITE_GAP	30		/* stock 18; 8-30 allowed */
#define T_DATA_0	24
#define T_DATA_1	56
#define T_PROGRAM	700
#define T5577_EM4100	0x00148040UL	/* RF/64, Manchester, blocks 1-2 */
#define T5577_PWD	0x00000010UL	/* block 0: a write needs block 7 */

static int nt;			/* durations in d[], field on first */

static void put_us(int on, unsigned long us)
{
	if (nt > 0 && ((nt - 1) & 1) == !on) {	/* the same state: longer */
		d[nt - 1] += us;
		return;
	}
	if (nt < MAXD) d[nt++] = us;
}

static void put(int on, unsigned long tc)
{
	put_us(on, tc * 8);
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

static int wlock;			/* -lock: the T5577 lock bit */
static unsigned long wpass;		/* -pass: its password, or 0 */
static unsigned long wsend;		/* -with HEX: the one it has now */

static void wblock(int blk, unsigned long data)
{
	int i;

	put(1, T_WAIT);
	put(0, T_START_GAP);
	wbit(1); wbit(0);			/* opcode 10: page 0 */
	if (wsend)				/* the tag's password first */
		for (i = 31; i >= 0; i--) wbit((int) (wsend >> i) & 1);
	wbit(wlock);				/* for ever, if set */
	for (i = 31; i >= 0; i--) wbit((int) (data >> i) & 1);
	for (i = 2; i >= 0; i--) wbit((blk >> i) & 1);
	put(1, T_PROGRAM + T_WAIT);
	wreset();
}

/* EM4305 (EM4x05), the other common blank, as stock em4305.c: a 0 is a
 * short gap, a 1 none; a word is 32 bits in 4 lines of 8 with line and
 * column parity.  EM4100 goes into word 4 (configuration) and 5-6. */
#define E_1		32
#define E_0_OFF		23
#define E_0_ON		18
#define E_STOP_OFF	55
#define E_STOP_ON	18
#define EM4305_EM4100	(0x40UL | 31 | 6UL << 14)  /* Manchester, RF/64 */

static void ebit(int b)
{
	if (b) put(1, E_1);
	else { put(0, E_0_OFF); put(1, E_0_ON); }
}

static void eword(int addr, unsigned long data)
{
	unsigned long long w = 0;
	int i, j, p;

	for (i = 0; i < 4; i++) {		/* lines: 8 bits, parity */
		p = 0;
		for (j = 0; j < 8; j++) {
			int bit = (int) (data >> (i * 8 + j)) & 1;

			w = w << 1 | bit;
			p ^= bit;
		}
		w = w << 1 | p;
	}
	for (i = 0; i < 8; i++) {		/* columns */
		p = 0;
		for (j = 0; j < 4; j++) p ^= (int) (data >> (j * 8 + i)) & 1;
		w = w << 1 | p;
	}
	w <<= 1;				/* bit 44: 0 */
	put_us(1, 8000);			/* power up */
	put(0, E_STOP_OFF); put(1, E_STOP_ON);	/* field stop */
	ebit(0);				/* start */
	ebit(0); ebit(1); ebit(0); ebit(1);	/* write 010 (LSB first), parity */
	p = 0;
	for (i = 0; i < 4; i++) { ebit(addr >> i & 1); p ^= addr >> i & 1; }
	ebit(0); ebit(0); ebit(p);
	for (i = 0; i < 45; i++) ebit((int) (w >> (44 - i)) & 1);
	put_us(1, 1480 + 9340);			/* power check, EEPROM write */
}

static int send(int fd)
{
	if (nt % 2 == 0) put_us(1, 8);		/* odd: ends with the field */
	if (write(fd, d, nt * 4) != nt * 4) {
		perror("rfid: write");
		return -1;
	}
	return 0;
}

static int verify(int fd, unsigned char id[5], unsigned char got[5])
{
	int t, n;

	for (t = 0; t < 4; t++) {		/* read it back */
		if ((n = read(fd, d, sizeof(d))) < 0) break;
		if (em_decode(n / 4, got)) return memcmp(got, id, 5) == 0 ? 1 : 0;
	}
	return -1;
}

static int write_em(unsigned char id[5], int tries)
{
	unsigned char b[64], got[5];
	unsigned long blk[2], rev[2];
	int r, c, p, i, fd, v;

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
	blk[0] = blk[1] = rev[0] = rev[1] = 0;
	for (i = 0; i < 64; i++) blk[i / 32] = blk[i / 32] << 1 | b[i];
	for (i = 63; i >= 0; i--) rev[i / 32] = rev[i / 32] << 1 | b[i];
	if ((fd = open("/dev/rfid", O_RDWR)) < 0) {
		perror("rfid: /dev/rfid");
		return 1;
	}
	/* Rounds: a tag with a password answers a plain write only when the
	 * gap happens to fall where it listens, which may take many tries
	 * (the stock app does the same while the button is held). */
	for (v = -1; tries-- > 0; ) {
		nt = 0;				/* T5577 first, as stock */
		wblock(1, blk[0]);		/* the data first and the */
		wblock(2, blk[1]);		/* config last: once PWD is */
		if (wpass) wblock(7, wpass);	/* set, a write needs it */
		wblock(0, T5577_EM4100
			| (wpass || (wsend && !wlock) ? T5577_PWD : 0));
		wreset();
		if (send(fd) < 0) return 1;
		if ((v = verify(fd, id, got)) == 1) {
			printf("written (T5577%s): EM4100 %02X %02X %02X %02X %02X\n",
				wlock ? ", locked for ever"
				      : wpass ? ", password set" : "",
				id[0], id[1], id[2], id[3], id[4]);
			return 0;
		}
		nt = 0;				/* then EM4305 */
		eword(4, EM4305_EM4100);
		eword(5, rev[0]);
		eword(6, rev[1]);
		if (send(fd) < 0) return 1;
		if ((v = verify(fd, id, got)) == 1) {
			printf("written (EM4305): EM4100 %02X %02X %02X %02X %02X\n",
				id[0], id[1], id[2], id[3], id[4]);
			return 0;
		}
	}
	if (v == 0)
		printf("the tag still reads EM4100 %02X %02X %02X %02X %02X: "
			"not written (locked, or a password?)\n",
			got[0], got[1], got[2], got[3], got[4]);
	else printf("written, but it does not read back as EM4100\n");
	return 1;
}

/* Read a tag, then write its number to the blank one put on next. */
static int copy_em(int tries)
{
	unsigned char id[5], got[5];
	int fd, n, t;

	if ((fd = open("/dev/rfid", O_RDONLY)) < 0) {
		perror("rfid: /dev/rfid");
		return 1;
	}
	for (t = 0; t < 20; t++) {
		if ((n = read(fd, d, sizeof(d))) < 0) {
			perror("rfid");
			return 1;
		}
		if (em_decode(n / 4, id)) break;
	}
	if (t == 20) {
		printf("no tag to copy (hold it to the back)\n");
		return 1;
	}
	printf("read EM4100 %02X %02X %02X %02X %02X\n",
		id[0], id[1], id[2], id[3], id[4]);
	printf("now put the blank one on\n");
	for (t = 0; t < 600; t++) {		/* until that one is away */
		if ((n = read(fd, d, sizeof(d))) < 0) break;
		if (!em_decode(n / 4, got) || memcmp(got, id, 5) != 0) break;
	}
	close(fd);
	return write_em(id, tries);
}

int main(int argc, char **argv)
{
	int fd, n, i, tries = 4, raw = 0, t;
	unsigned char id[5];

	if (argc >= 2 && (strcmp(argv[1], "write") == 0
				|| strcmp(argv[1], "copy") == 0)) {
		int tries = 20, cp = argv[1][0] == 'c';

		for (i = 2; i < argc && argv[i][0] == '-'; i++) {
			char *e;

			if (strcmp(argv[i], "-lock") == 0) wlock = 1;
			else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
				tries = atoi(argv[++i]);
			else if (strcmp(argv[i], "-pass") == 0 && i + 1 < argc) {
				wpass = strtoul(argv[++i], &e, 16);
				if (*e != 0 || wpass == 0) goto use;
			} else if (strcmp(argv[i], "-with") == 0 && i + 1 < argc) {
				wsend = strtoul(argv[++i], &e, 16);
				if (*e != 0 || wsend == 0) goto use;
			} else goto use;
		}
		if (tries < 1 || (wlock && wpass)) goto use;
		if (cp) {
			if (i != argc) goto use;
			return copy_em(tries);
		}
		if (i + 6 != argc || strcmp(argv[i], "EM") != 0) goto use;
		for (i++, n = 0; n < 5; n++) {
			char *e;
			unsigned long v = strtoul(argv[i + n], &e, 16);

			if (*e != 0 || v > 255) goto use;
			id[n] = (unsigned char) v;
		}
		return write_em(id, tries);
	}
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-r") == 0) raw = 1;
		else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
			tries = atoi(argv[++i]);
		else {
use:			fprintf(stderr, "usage: rfid [-r] [-n TRIES]\n"
				"       rfid write [-n N] [-pass HEX] "
				"[-with HEX] [-lock] EM B0 B1 B2 B3 B4\n"
				"       rfid copy [-n N] [-pass HEX] "
				"[-with HEX] [-lock]\n");
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
