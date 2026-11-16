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
#include <sys/ioctl.h>
#include <minix/rfid.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXD	512		/* ~3 EM frames; .bss under 4 KB (R2a.5) */
#define MAXH	(2 * MAXD)

static unsigned long d[MAXD];
static unsigned char h[MAXH];		/* half bits */
static unsigned char decbits[MAXH / 2];	/* decoded bits (em_decode, t5_word) */

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
/* Decode one phase (off) and grid of the capture durations d[0..nd-1] into
 * decbits[] (0/1), the RF/64 Manchester bits; returns the bit count.  Used
 * by em_decode (EM4100 frame) and the T5577 block decoders. */
static int t5_manchester(int nd, int off, int grid)
{
	int i, nh = 0, pos = grid, nb = 0;

	for (i = off; i + 1 < nd && nh < MAXH - 4; i += 2) {
		unsigned long t = d[i] + d[i + 1];
		int p = (int) ((t + 128) / 256), a, b;

		if (p < 2 || p > 4 || t < 400 || t > 1150) {
			nh = 0;			/* not Manchester: start again */
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
	for (i = grid ? 1 : 0; i + 1 < nh; i += 2) {
		if (h[i] == h[i + 1]) { nb = 0; continue; }
		decbits[nb++] = h[i + 1];
	}
	return nb;
}

static int em_decode(int nd, unsigned char id[5])
{
	int off, grid, inv, s, j, nb;

	for (off = 0; off < 2; off++)
	for (grid = 0; grid < 2; grid++) {
		nb = t5_manchester(nd, off, grid);
		for (inv = 0; inv < 2; inv++)
			for (s = 0; s + 64 <= nb; s++) {
				unsigned char b64[64];

				for (j = 0; j < 64; j++) b64[j] = decbits[s + j] ^ inv;
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

/* ---- reading a T5577 block (R6.5 part 3) ----
 *
 * The field is held up (RFIDIOCFIELD) across a read command and the tag's
 * reply: a write plays the command's field gaps, a read captures the
 * reply's comparator edges.  The command is built as stock t5577.c's
 * fixed-bit-length downlink: opcode (not-test, page), lock, block, each
 * bit a field-on run (24 or 56 carrier periods) then a gap (18).  A read
 * is the opcode alone; a regular read (opcode, no address) puts the tag
 * back to broadcasting its data blocks. */
#define T_PAGE0		0

static void t5_bits(unsigned long v, int n)	/* n bits, MSB first */
{
	int i;
	for (i = n - 1; i >= 0; i--) wbit((int) (v >> i) & 1);
}

/* Build a read command for one block into d[] (nt).  with_pass sends the
 * tag's password first, as a protected tag requires. */
static void t5_read_cmd(int block, int with_pass, unsigned long pass)
{
	nt = 0;
	put(1, T_WAIT);			/* let the tag charge */
	put(0, 30);			/* start gap: stock's 30 Tc exactly (a
					 * longer one can read as a reset) */
	wbit(1); wbit(T_PAGE0);		/* opcode: not test mode, page 0 */
	if (with_pass) t5_bits(pass, 32);
	wbit(0);			/* lock bit */
	t5_bits((unsigned long) block, 3);
}

/* Decode the capture durations d[0..nd-1] as ASK/Manchester (RF/64, as the
 * EM4100 front-end) and find the 32-bit word that repeats every 32 bits: a
 * block read's answer repeats its word forever, where the EM4100 broadcast
 * is 64 bits and does not.  Returns the repeat count (>=2 means a block
 * answered), the word (one arbitrary rotation) in *word.  Tries both edge
 * phases and grids; the word's polarity may be inverted (try ~word too). */
static int t5_word(int nd, unsigned long *word)
{
	int off, grid, i, nb, s, bestc = 0;
	unsigned long best = 0;

	for (off = 0; off < 2; off++)
	for (grid = 0; grid < 2; grid++) {
		nb = t5_manchester(nd, off, grid);
		for (s = 0; s + 64 <= nb; s++) {	/* a 32-bit window */
			unsigned long w = 0;
			int c, t2;

			for (i = 0; i < 32; i++)
				if (decbits[s + i] != decbits[s + 32 + i]) break;
			if (i < 32) continue;		/* not period 32 here */
			for (i = 0; i < 32; i++) w = (w << 1) | decbits[s + i];
			c = 1;
			for (t2 = s + 32; t2 + 32 <= nb; t2 += 32) {
				for (i = 0; i < 32; i++)
					if (decbits[t2 + i] != decbits[s + i]) break;
				if (i < 32) break;
				c++;
			}
			if (c > bestc) { bestc = c; best = w; }
		}
	}
	*word = best;
	return bestc;
}

/* 1 if the 32-bit word `expected` is what a block answered, at any bit
 * alignment or polarity: the answer repeats every 32 bits, so sliding one
 * bit covers every rotation, and the detector's polarity is unknowable, so
 * both readings count (stock t5577_block_holds).  `expected` must not be
 * 0 or ~0, which cannot be told from their own inverse. */
static int t5_holds(int nd, unsigned long expected)
{
	int off, grid, i, nb, s;

	for (off = 0; off < 2; off++)
	for (grid = 0; grid < 2; grid++) {
		nb = t5_manchester(nd, off, grid);
		for (s = 0; s + 32 <= nb; s++) {
			unsigned long w = 0;

			for (i = 0; i < 32; i++) w = (w << 1) | decbits[s + i];
			if (w == expected
			    || ((~w) & 0xFFFFFFFFUL) == expected) return 1;
		}
	}
	return 0;
}

/* Put the tag back to broadcasting its blocks (opcode alone). */
static void t5_regular_read(void)
{
	nt = 0;
	put(1, T_WAIT);
	put(0, T_START_GAP);
	wbit(1); wbit(T_PAGE0);
}

/* One held exchange: send the command, then capture the reply.  The command
 * in d[] is already on the wire before the reply is read, so the reply
 * reuses d[].  The field is already held.  Returns reply durations, or -1. */
static int t5_exchange(int fd, int with_pass, unsigned long pass, int block)
{
	int n;

	t5_read_cmd(block, with_pass, pass);
	if (send(fd) < 0) return -1;		/* the command's field gaps */
	if ((n = read(fd, d, sizeof(d))) < 0) {	/* the reply (settled) */
		perror("rfid: read");
		return -1;
	}
	return n / 4;
}

/* T5577 configuration words (block 0), from stock lib/lfrfid/tools/t5577.h
 * (proxmark3 bit definitions). */
#define T5_CFG_OPEN	0x00148040UL	/* RF/64 Manchester, 2 blocks, PWD clear:
				 * clearing the password bit is the whole of
				 * removing a password (proxmark3 does the same),
				 * and an EM4100 asks for this word anyway */
#define T5_CFG_BLANK	0x000880E0UL	/* RF/32 Manchester, 7 data blocks: the
					 * blank `lf t55xx wipe` leaves */
#define T5_PWD_BIT	0x00000010UL

/* Read block `blk` through the held field (open, command, capture, leave the
 * tag broadcasting, drop the field).  Returns reply edge count in d[], or -1. */
static int t5_read_block(int blk, int with_pass, unsigned long pass)
{
	int fd, n, on = 1, off = 0;

	if ((fd = open("/dev/rfid", O_RDWR)) < 0) { perror("rfid"); return -1; }
	if (ioctl(fd, RFIDIOCFIELD, &on) < 0) {
		perror("rfid: field"); close(fd); return -1;
	}
	n = t5_exchange(fd, with_pass, pass, blk);
	t5_regular_read();			/* leave it broadcasting */
	(void) send(fd);
	ioctl(fd, RFIDIOCFIELD, &off);
	close(fd);
	return n;
}

/* Write block 0 = `data`, sending `sendpass` first if the tag wants one
 * (0 for none), through the ordinary (field-dropped) write path. */
static int t5_write_block0(unsigned long data, unsigned long sendpass)
{
	int fd, r;

	if ((fd = open("/dev/rfid", O_WRONLY)) < 0) { perror("rfid"); return -1; }
	wsend = sendpass;
	wlock = 0;
	nt = 0;
	wblock(0, data);
	wreset();
	r = send(fd);
	close(fd);
	return r;
}

/* rfid clear [-with HEX] [-n N]: remove a T5577 password and return the tag
 * to a standard, programmable EM4100 configuration, by writing block 0 =
 * CONFIG_OPEN (which clears the password bit) and reading it back.  Sweeps
 * no password first, then the one given with -with, and stops at the first
 * that opens the tag - a password frame sent to an already-open tag is read
 * off the wrong bit positions and can hit the lock bit (stock scene_clear). */
static int clear_t5(unsigned long withpass, int haswith, int tries)
{
	int n, cand_i, t;

	n = t5_read_block(0, 0, 0);		/* see the tag first */
	if (n < 0) { printf("no tag, or its block 0 cannot be read\n"); return 1; }
	if (t5_holds(n, T5_CFG_OPEN)) {
		printf("already open: block 0 is CONFIG_OPEN (%08lX), no password\n",
			T5_CFG_OPEN);
		return 0;
	}

	/* no password, then the typed one; each tried up to `tries` times, as
	 * a T5577 write only lands when a gap falls where the tag listens */
	for (cand_i = 0; cand_i <= (haswith ? 1 : 0); cand_i++) {
		int wp = (cand_i == 1);
		unsigned long cand = wp ? withpass : 0;

		for (t = 0; t < tries; t++) {
			if (t5_write_block0(T5_CFG_OPEN, cand) < 0) return 1;
			n = t5_read_block(0, 0, 0);
			if (n >= 0 && t5_holds(n, T5_CFG_OPEN)) {
				printf("cleared%s: block 0 now CONFIG_OPEN, "
					"the tag is programmable again\n",
					wp ? " with the given password"
					   : " (it had no password)");
				return 0;
			}
		}
	}
	printf("not cleared: the tag did not open to %s%08lX.\n"
		"It is password-protected (password not known) or the block 0 "
		"lock bit is set (permanent).\n",
		haswith ? "the given password or " : "no password, ",
		haswith ? withpass : 0);
	return 1;
}

/* rfid dump [-with HEX] [BLOCK]: read a T5577 block (0 by default) and
 * print the reply's raw edge durations, to see the tag answering. */
static int dump_block(int block, int with_pass, unsigned long pass, int nocmd,
			int settle)
{
	int fd, n, i, on = 1, off = 0;

	if ((fd = open("/dev/rfid", O_RDWR)) < 0) {
		perror("rfid: /dev/rfid");
		return 1;
	}
	if (settle >= 20) ioctl(fd, RFIDIOCFIELD, &settle);	/* settle ms */
	if (ioctl(fd, RFIDIOCFIELD, &on) < 0) {
		perror("rfid: field");
		close(fd);
		return 1;
	}
	if (nocmd) {				/* just the held field + capture:
					 * a T5577 broadcasts EM4100 here */
		if ((n = read(fd, d, sizeof(d))) < 0) { perror("rfid"); n = -4; }
		else n /= 4;
	} else {
		n = t5_exchange(fd, with_pass, pass, block);
		t5_regular_read();		/* leave it broadcasting */
		(void) send(fd);
	}
	ioctl(fd, RFIDIOCFIELD, &off);
	close(fd);
	if (n < 0) return 1;
	{
		unsigned long w;
		int c = t5_word(n, &w);
		unsigned char id[5];
		int em = em_decode(n, id);

		printf("%s %d: %d edges", nocmd ? "field" : "block", block, n);
		if (c >= 2) printf(", word %08lX (x%d) ~%08lX",
					w, c, (~w) & 0xFFFFFFFFUL);
		if (em) printf(", EM4100 %02X %02X %02X %02X %02X",
				id[0], id[1], id[2], id[3], id[4]);
		if (c < 2 && !em) {
			printf(", raw;");
			for (i = 0; i < n && i < 50; i++) printf(" %lu", d[i]);
		}
		printf("\n");
	}
	return 0;
}

int main(int argc, char **argv)
{
	int fd, n, i, tries = 4, raw = 0, t;
	unsigned char id[5];

	if (argc >= 2 && strcmp(argv[1], "clear") == 0) {
		int haswith = 0, tries = 20;
		unsigned long withpass = 0;
		char *e;

		for (i = 2; i < argc; i++) {
			if (strcmp(argv[i], "-with") == 0 && i + 1 < argc) {
				withpass = strtoul(argv[++i], &e, 16);
				haswith = 1;
				if (*e != 0) goto use;
			} else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
				tries = atoi(argv[++i]);
			else goto use;
		}
		if (tries < 1) goto use;
		return clear_t5(withpass, haswith, tries);
	}

	if (argc >= 2 && strcmp(argv[1], "dump") == 0) {
		int block = 0, with = 0, nocmd = 0, settle = 0;
		unsigned long pass = 0;
		char *e;

		for (i = 2; i < argc; i++) {
			if (strcmp(argv[i], "-q") == 0) nocmd = 1;
			else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
				settle = atoi(argv[++i]);
			else if (strcmp(argv[i], "-with") == 0 && i + 1 < argc) {
				pass = strtoul(argv[++i], &e, 16);
				with = 1;
				if (*e != 0) goto use;
			} else {
				block = (int) strtoul(argv[i], &e, 10);
				if (*e != 0 || block < 0 || block > 7) goto use;
			}
		}
		return dump_block(block, with, pass, nocmd, settle);
	}

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
