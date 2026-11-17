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
 *		rfid clear [-with HEX] [-n N]
 *					remove a T5577 password and make the
 *					tag programmable again: write block 0 =
 *					CONFIG_OPEN (clears the password bit),
 *					verified by reading it back.  Tries no
 *					password, -with HEX, then the proxmark3
 *					dictionary (known copier passwords); it
 *					stops at the first that opens the tag
 *		rfid -c 125|psk|134	read with that field: 125 kHz (the
 *					default), PSK tags' 62.5 kHz at 25 %,
 *					FDX-B's 134.2 kHz (R9.0.4); -r shows
 *					the timings (decoders: T19)
 *		rfid dump [-with HEX] [BLOCK]
 *					read one T5577 block (0 by default) and
 *					print its 32-bit word (block 0 is the
 *					configuration); -with sends a password
 *
 * A block read holds the 125 kHz field up (ioctl RFIDIOCFIELD) across the
 * read command (field gaps, a write) and the tag's reply (comparator edges,
 * a read); the reply is the block's 32-bit word repeated, decoded here as
 * the EM4100 front-end decodes Manchester.  A read-only EM4100, or a
 * password-protected T5577 read without -with, ignores the command and keeps
 * broadcasting its number instead.
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

/* The 64 EM4100 bits of a number into b[64]: nine 1s, ten rows of 4 data
 * bits and their even parity, four column parities, a 0. */
static void em_bits(unsigned char id[5], unsigned char b[64])
{
	int r, c, p;

	for (r = 0; r < 9; r++) b[r] = 1;
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
}

static int write_em(unsigned char id[5], int tries)
{
	unsigned char b[64], got[5];
	unsigned long blk[2], rev[2];
	int r, c, p, i, fd, v;

	em_bits(id, b);
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

#include "t5577_dict.h"

/* Try one candidate password: write block 0 = CONFIG_OPEN sending `cand`
 * (0 = no password), then read block 0 back with no password.  The write
 * clears the password bit only if `cand` is the tag's password (or it had
 * none), and only then does a no-password read return CONFIG_OPEN.  1 if
 * the tag opened, 0 if not, -1 on an IO error. */
static int clear_try(unsigned long cand)
{
	int n;

	if (t5_write_block0(T5_CFG_OPEN, cand) < 0) return -1;
	n = t5_read_block(0, 0, 0);
	if (n < 0) return -1;
	return t5_holds(n, T5_CFG_OPEN) ? 1 : 0;
}

/* rfid clear [-with HEX] [-n N]: remove a T5577 password and return the tag
 * to a standard, programmable EM4100 configuration, by writing block 0 =
 * CONFIG_OPEN (which clears the password bit) and reading it back.  Tries no
 * password, the one given with -with, then the proxmark3 dictionary (known
 * copier and default passwords), and stops at the first that opens the tag -
 * a password frame sent to an already-open tag is read off the wrong bit
 * positions and can hit the lock bit (stock scene_clear). */
static int clear_t5(unsigned long withpass, int haswith, int tries)
{
	int n, t, r;
	unsigned long w;
	unsigned i;

	n = t5_read_block(0, 0, 0);		/* see the tag first */
	if (n >= 0 && t5_word(n, &w) >= 2 && t5_holds(n, T5_CFG_OPEN)) {
		printf("already open: block 0 is CONFIG_OPEN (%08lX), "
			"no password\n", T5_CFG_OPEN);
		return 0;
	}

	/* No password, then the one typed, each tried a few times (a T5577
	 * write only lands when a gap falls where the tag listens). */
	for (t = 0; t < tries; t++) {
		if ((r = clear_try(0)) < 0) return 1;
		if (r) { printf("cleared: the tag had no password; it is "
				"programmable again\n"); return 0; }
	}
	if (haswith) for (t = 0; t < tries; t++) {
		if ((r = clear_try(withpass)) < 0) return 1;
		if (r) { printf("cleared with %08lX: programmable again\n",
				withpass); return 0; }
	}

	/* The dictionary: one try each (the field is re-raised per write, so a
	 * flaky write just misses that word; the common passwords are few). */
	printf("sweeping %u known passwords...\n", (unsigned) T5_DICT_N);
	for (i = 0; i < T5_DICT_N; i++) {
		if ((i & 15) == 0 && i) printf("  %u/%u\n", i, (unsigned) T5_DICT_N);
		if ((r = clear_try(t5_dict[i])) < 0) return 1;
		if (r) {
			printf("cleared with %08lX (a known password): "
				"programmable again\n", t5_dict[i]);
			return 0;
		}
	}
	printf("not cleared: no password opened the tag.\n"
		"Either the block 0 lock bit is set (permanent, not clearable "
		"by any tool), or it carries a password not in the list - "
		"a hardware copier may have set a random one.\n");
	return 1;
}

/* 1 if `w` can be a T5577 basic-mode block 0 (datasheet layout): master
 * key 0, 6 or 9, bits 27..21, 17, 8 and 2..1 clear, a known modulation and
 * at least one block in the broadcast (otherwise the tag sends nothing). */
static int t5_cfg_ok(unsigned long w)
{
	unsigned key = (unsigned) (w >> 28) & 15, mod = (unsigned) (w >> 12) & 0x1F;

	if (key != 0 && key != 6 && key != 9) return 0;
	if (w & 0x0FE20106UL) return 0;
	if (((w >> 5) & 7) == 0) return 0;
	return mod <= 8 || mod == 0x10 || mod == 0x18;
}

/* Block 0 from a word read at an unknown rotation and polarity: the one
 * known configuration (with or without the password bit) among the 64
 * readings, else the only plausible one.  0 if none or several fit. */
static int t5_cfg_pick(unsigned long w, unsigned long *cfg)
{
	unsigned long x, y, got = 0;
	int inv, r, k = 0;

	for (inv = 0; inv < 2; inv++)
	for (r = 0; r < 32; r++) {
		x = inv ? (~w & 0xFFFFFFFFUL) : w;
		y = r ? (((x << r) | (x >> (32 - r))) & 0xFFFFFFFFUL) : x;
		if ((y & ~T5_PWD_BIT) == T5_CFG_OPEN
		    || (y & ~T5_PWD_BIT) == T5_CFG_BLANK) {
			*cfg = y;
			return 1;
		}
		if (t5_cfg_ok(y) && y != got) { got = y; k++; }
	}
	if (k != 1) return 0;
	*cfg = got;
	return 1;
}

/* Print what a block 0 word says, in the datasheet's terms. */
static void t5_cfg_print(unsigned long c)
{
	static char *mods[] = { "direct", "PSK1", "PSK2", "PSK3", "FSK1",
		"FSK2", "FSK1a", "FSK2a", "Manchester" };
	static int rates[] = { 8, 16, 32, 40, 50, 64, 100, 128 };
	unsigned mod = (unsigned) (c >> 12) & 0x1F;

	printf("  RF/%d, %s, %d data block%s, password %s%s\n",
		rates[(c >> 18) & 7],
		mod <= 8 ? mods[mod] : mod == 0x10 ? "biphase" : "biphase57",
		(int) ((c >> 5) & 7), ((c >> 5) & 7) == 1 ? "" : "s",
		(c & T5_PWD_BIT) ? "on" : "off",
		(c & ~T5_PWD_BIT) == T5_CFG_OPEN ? " (EM4100 layout)" : "");
}

/* rfid dump [-with HEX] [BLOCK]: read one T5577 block (0 by default) and
 * print its 32-bit word.  A T5577 answers a block read with its word; a tag
 * that ignores the command (a read-only EM4100, or a password-protected one
 * read without its password) is still broadcasting its EM4100 number.
 * The capture starts after the reader settles, partway into the repeating
 * answer, and the detector's polarity is unknowable, so the word comes out
 * at an unknown rotation, maybe inverted.  Block 0 has a fixed layout and is
 * put right by trying every reading; other blocks are printed as read. */
static int dump_block(int block, int with_pass, unsigned long pass)
{
	int n;
	unsigned long w, c;
	unsigned char id[5];

	n = t5_read_block(block, with_pass, pass);
	if (n < 0) return 1;
	if (t5_word(n, &w) >= 2) {
		if (block == 0 && t5_cfg_pick(w, &c)) {
			printf("block 0: %08lX\n", c);
			t5_cfg_print(c);
		} else
			printf("block %d: %08lX (read at an unknown start: may be "
				"rotated or inverted)\n", block, w);
	} else if (em_decode(n, id))
		printf("block %d: no T5577 word; the tag is broadcasting "
			"EM4100 %02X %02X %02X %02X %02X (read-only, or "
			"password-protected and read without -with)\n",
			block, id[0], id[1], id[2], id[3], id[4]);
	else
		printf("block %d: no answer (hold the tag to the back)\n", block);
	return 0;
}

/* rfid emulate EM B0 B1 B2 B3 B4: be an EM4100 tag of that number for a
 * reader, until stopped (Ctrl-C).  The reader's field is the clock; the
 * kernel loads the antenna in step with it (RFIDIOCEMU), so the waveform
 * cannot drift.  Each EM4100 bit is two Manchester half bits, each a
 * 32-carrier-period segment the antenna is loaded for or not; the 128
 * segments (dur = length, pulse = how long loaded) play round and round.
 * The two arrays live in d[], which emulation does not otherwise use. */
static int emulate_em(unsigned char id[5])
{
	unsigned char b[64];
	unsigned long *dur = &d[0], *pulse = &d[256];
	struct rfid_emu e;
	int k, fd;

	em_bits(id, b);
	for (k = 0; k < 64; k++) {		/* bit -> loaded, then not */
		dur[2 * k] = dur[2 * k + 1] = 31;	/* 32 periods each */
		pulse[2 * k] = b[k] ? 32 : 0;
		pulse[2 * k + 1] = b[k] ? 0 : 32;
	}
	e.dur = dur;
	e.pulse = pulse;
	e.n = 128;
	if ((fd = open("/dev/rfid", O_RDWR)) < 0) {
		perror("rfid: /dev/rfid");
		return 1;
	}
	if (ioctl(fd, RFIDIOCEMU, &e) < 0) {
		perror("rfid: emulate");
		close(fd);
		return 1;
	}
	printf("emulating EM4100 %02X %02X %02X %02X %02X; hold to a reader, "
		"Ctrl-C to stop\n", id[0], id[1], id[2], id[3], id[4]);
	for (;;) sleep(60);		/* Ctrl-C ends it; close stops the field */
	return 0;			/* not reached */
}

int main(int argc, char **argv)
{
	int carrier = RFID_CARRIER_125;
	int fd, n, i, tries = 4, raw = 0, t;
	unsigned char id[5];

	if (argc >= 2 && strcmp(argv[1], "emulate") == 0) {
		unsigned char id[5];

		if (argc != 8 || strcmp(argv[2], "EM") != 0) goto use;
		for (n = 0; n < 5; n++) {
			char *e;
			unsigned long v = strtoul(argv[3 + n], &e, 16);

			if (*e != 0 || v > 255) goto use;
			id[n] = (unsigned char) v;
		}
		return emulate_em(id);
	}

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
		int block = 0, with = 0;
		unsigned long pass = 0;
		char *e;

		for (i = 2; i < argc; i++) {
			if (strcmp(argv[i], "-with") == 0 && i + 1 < argc) {
				pass = strtoul(argv[++i], &e, 16);
				with = 1;
				if (*e != 0) goto use;
			} else {
				block = (int) strtoul(argv[i], &e, 10);
				if (*e != 0 || block < 0 || block > 7) goto use;
			}
		}
		return dump_block(block, with, pass);
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
		else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
			i++;		/* R9.0.4: the read carrier */
			if (strcmp(argv[i], "125") == 0)
				carrier = RFID_CARRIER_125;
			else if (strcmp(argv[i], "psk") == 0)
				carrier = RFID_CARRIER_PSK;
			else if (strcmp(argv[i], "134") == 0)
				carrier = RFID_CARRIER_134;
			else goto use;
		} else {
use:			fprintf(stderr, "usage: rfid [-r] [-n TRIES] [-c 125|psk|134]\n"
				"       rfid write [-n N] [-pass HEX] "
				"[-with HEX] [-lock] EM B0 B1 B2 B3 B4\n"
				"       rfid copy [-n N] [-pass HEX] "
				"[-with HEX] [-lock]\n"
				"       rfid clear [-with HEX] [-n N]\n"
				"       rfid dump [-with HEX] [BLOCK]\n"
				"       rfid emulate EM B0 B1 B2 B3 B4\n");
			return 2;
		}
	}
	if ((fd = open("/dev/rfid", O_RDONLY)) < 0) {
		perror("rfid: /dev/rfid");
		return 1;
	}
	if (carrier != RFID_CARRIER_125
			&& ioctl(fd, RFIDIOCCARRIER, &carrier) != 0) {
		perror("rfid: carrier");
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
