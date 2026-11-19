/* rfid - read a 125 kHz tag (Galtzagorriak MINIX, spec R6.5; /dev/rfid)
 *
 * Usage:	rfid [-r] [-n TRIES] [-o FILE]
 *					read: print the tag's protocol and
 *					data (hex, as the stock app), then
 *					the data as the stock info screen
 *					shows it; -r: also the timings; up
 *					to TRIES reads of 0.5 s (default 4,
 *					more while a tag is half read);
 *					-o: also save it as a .rfid file
 *		rfid write [OPTS] PROTO HEX...
 *		rfid write [OPTS] -f FILE
 *					make a blank tag (fob, card) that
 *					tag: as T5577, else as EM4305 (as
 *					stock), read back.  PROTO: a name
 *					of `rfid list` (case, spaces and /
 *					do not matter; EM = EM4100); HEX:
 *					its data bytes, or a .rfid file
 *		  -hitag		straight to the Hitag micro magic chips
 *					(ID8265, ID8210, H5.5: R9.0.7), which
 *					are otherwise tried after T5577 and
 *					EM4305 (not with -pass, -with, -lock)
 *		rfid copy [OPTS]	read a tag, then write it to the
 *					blank one put on next
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
 *					FDX-B's 134.2 kHz (R9.0.4); the
 *					decoders run on 125 kHz captures,
 *					so -r shows the timings (T19b)
 *		rfid raw FILE [-c MODE] [-t SECS]
 *					record the signal as it is (default
 *					10 s) in Unleashed's .ask.raw/.psk.raw
 *					format, for tags no decoder knows
 *					(R9.0.8; files on the SD card)
 *		rfid -s SECS [-c MODE]	capture without a gap (R9.0.5) for
 *					SECS and report halves, edges,
 *					overruns and the durations' range
 *		rfid dump [-with HEX] [BLOCK]
 *					read one T5577 block (0 by default) and
 *					print its 32-bit word (block 0 is the
 *					configuration); -with sends a password
 *		rfid emulate PROTO HEX...
 *		rfid emulate -f FILE	be that tag for a reader (Ctrl-C ends)
 *		rfid list		the protocols, their modulation and
 *					what works with them today
 *
 * The protocols are the Unleashed firmware's (liblfrfid, src/lib/lfrfid,
 * spec 9.0): all 25 decode, encode and fill T5577/EM4305 blocks.  What is
 * not done yet (T19b: PSK decoding on the -c psk carrier, captures longer
 * than 512 edges, emulation longer than 512 segments) limits reading to the ASK
 * protocols (Manchester, biphase, direct), read-back after a write to
 * those, and emulation to frames of up to 512 segments; the others say so.
 * EM4100 at RF/64 keeps its own decoder, writer and emulation (R6.5).
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
 * bits, a 0.  Other kinds go to liblfrfid; what none decodes is shown as a
 * summary.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <time.h>
#include <minix/rfid.h>
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <furi.h>			/* liblfrfid's shim: FuriString */
#include <lfrfid_read.h>

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

static int nt;			/* durations in dp[], field on first */
static unsigned long *dp = d;	/* d[], or Hitag's longer list */
static int dmax = MAXD;

static void put_us(int on, unsigned long us)
{
	if (nt > 0 && ((nt - 1) & 1) == !on) {	/* the same state: longer */
		dp[nt - 1] += us;
		return;
	}
	if (nt < dmax) dp[nt++] = us;
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
static int whitag;			/* -hitag: straight to Hitag micro */
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
	if (write(fd, dp, nt * 4) != nt * 4) {
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

/* ---- Hitag micro (ID8265, ID8210, H5.5): R9.0.7 ----
 * "Magic" ID82xx chips that send an EM4100 number by themselves (TTF) once
 * pages 0, 1 hold its 64 bits and page 0xFF the config.  The writer is
 * Unleashed's lib/lfrfid/tools/hitagmicro.c (from Proxmark3): open loop,
 * nothing is read back but the EM4100 afterwards.  Reader to tag is field
 * gaps: every bit cell starts with a 64 us gap, then the field stays on
 * 96 us (a 0) or 160 us (a 1); a frame starts with a 0 and a code
 * violation (gap, 288 us on) and ends with a gap.  A frame is 5 flag bits,
 * a 6-bit command, its data, a CRC-16 (0x1021, init 0, output reflected),
 * fields LSB first, data and block bytes MSB first.  Each block is written
 * in its own powered session (UID, SYSINFO, READ config, LOGIN, WRITE x2);
 * config last, so a failed run keeps the old config; then a few clean
 * power cycles latch it. */
#define HM_GAP		64
#define HM_B0		96
#define HM_B1		160
#define HM_VIOL		288
#define HM_FLAGS	0x04
#define HM_MAXD		1024		/* a session's durations (malloc) */

static unsigned char hm_tx[80];		/* a frame's bits */
static int hm_n;

static void hm_lsb(unsigned v, int nbits)
{
	int i;

	for (i = 0; i < nbits; i++) hm_tx[hm_n++] = (v >> i) & 1;
}

static void hm_msb(const unsigned char *b, int nbits)
{
	int i;

	for (i = 0; i < nbits; i++) hm_tx[hm_n++] = b[i / 8] >> (7 - i % 8) & 1;
}

static void hm_crc(void)
{
	unsigned r = 0, out = 0;
	int i;

	for (i = 0; i < hm_n; i++) {		/* MSB-first, bit by bit */
		unsigned top = (r >> 15 & 1) ^ hm_tx[i];

		r = (r << 1) & 0xFFFF;
		if (top) r ^= 0x1021;
	}
	for (i = 0; i < 16; i++) out |= ((r >> i) & 1) << (15 - i);	/* refout */
	hm_lsb(out, 16);
}

/* The frame into the field list: SOF, the bits, EOF; then the field on
 * for wait_us, while the tag answers (nobody listens). */
static void hm_send(unsigned long wait_us)
{
	int i;

	put_us(0, HM_GAP); put_us(1, HM_B0);		/* SOF: a 0, */
	put_us(0, HM_GAP); put_us(1, HM_VIOL);		/* a violation */
	for (i = 0; i < hm_n; i++) {
		put_us(0, HM_GAP);
		put_us(1, hm_tx[i] ? HM_B1 : HM_B0);
	}
	put_us(0, HM_GAP);				/* EOF */
	put_us(1, wait_us);
}

static void hm_cmd(unsigned cmd, int page, const unsigned char *data)
{
	hm_n = 0;
	hm_lsb(HM_FLAGS, 5);
	hm_lsb(cmd, 6);
	if (page >= 0) hm_lsb((unsigned) page, 8);
	if (cmd == 0x12) hm_lsb(0, 8);			/* READ: count 0 */
	if (data != NULL) hm_msb(data, 32);
	hm_crc();
}

/* One block in a powered session.  Returns -1 on a device error. */
static int hm_block(int fd, const unsigned char *pass, int page,
						const unsigned char *data)
{
	nt = 0;
	put_us(1, 3000);				/* charge */
	hm_cmd(0x02, -1, NULL);	hm_send(21000);		/* READ UID */
	hm_cmd(0x17, -1, NULL);	hm_send(13000);		/* SYSINFO */
	hm_cmd(0x12, 0xFF, NULL); hm_send(17000);	/* READ config */
	hm_cmd(0x28, -1, pass);	hm_send(9000);		/* LOGIN */
	hm_cmd(0x14, page, data); hm_send(30000);	/* WRITE, twice: */
	hm_send(30000);			/* the 8265 commits now and then */
	if (send(fd) < 0) return -1;	/* the field goes off after it */
	(void) poll((struct pollfd *) NULL, 0, 20);	/* the tag resets */
	return 0;
}

/* Write an EM4100 frame (b[64]) to a Hitag micro chip with the password of
 * one variant; clock: 64, 32 or 16 (the config's data rate). */
static int hm_write(int fd, const unsigned char b[64], const unsigned char *pass,
								int clock)
{
	unsigned char blk[2][4], cfg[4];
	int i;

	memset(blk, 0, sizeof blk);
	for (i = 0; i < 64; i++)
		if (b[i]) blk[i / 32][i % 32 / 8] |= 0x80 >> (i % 8);
	cfg[0] = clock == 32 ? 0x85 : clock == 16 ? 0x45 : 0x05;
	cfg[1] = cfg[2] = cfg[3] = 0;
	(void) poll((struct pollfd *) NULL, 0, 100);	/* a cold chip */
	if (hm_block(fd, pass, 0x00, blk[0]) < 0
	    || hm_block(fd, pass, 0x01, blk[1]) < 0
	    || hm_block(fd, pass, 0xFF, cfg) < 0) return -1;
	for (i = 0; i < 6; i++) {			/* latch the config */
		nt = 0;
		put_us(1, 50000);
		if (send(fd) < 0) return -1;
		(void) poll((struct pollfd *) NULL, 0, 20);
	}
	return 0;
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
	for (v = -1; !whitag && tries-- > 0; ) {
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
	if (!wpass && !wlock && !wsend) {	/* then Hitag micro (R9.0.7) */
		static const struct { unsigned char pw[4]; const char *name; }
		    hv[3] = { { { 0, 0, 0, 0 }, "8265" },
			      { { 0x9A, 0xC4, 0x99, 0x9C }, "8210" },
			      { { 0x49, 0x6B, 0x0E, 0x59 }, "H5.5" } };
		unsigned long *hd = malloc(HM_MAXD * sizeof(unsigned long));

		if (hd == NULL) {
			fprintf(stderr, "rfid: no memory for Hitag\n");
			return 1;
		}
		dp = hd;
		dmax = HM_MAXD;
		for (i = 0; i < 3; i++) {
			if (hm_write(fd, b, hv[i].pw, 64) < 0) return 1;
			dp = d;			/* the read-back uses d[] */
			dmax = MAXD;
			if ((v = verify(fd, id, got)) == 1) {
				printf("written (Hitag micro %s): EM4100 %02X %02X "
					"%02X %02X %02X\n", hv[i].name, id[0],
					id[1], id[2], id[3], id[4]);
				free(hd);
				return 0;
			}
			dp = hd;
			dmax = HM_MAXD;
		}
		dp = d;
		dmax = MAXD;
		free(hd);
	}
	if (v == 0)
		printf("the tag still reads EM4100 %02X %02X %02X %02X %02X: "
			"not written (locked, or a password?)\n",
			got[0], got[1], got[2], got[3], got[4]);
	else printf("written, but it does not read back as EM4100\n");
	return 1;
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

/* ---- every Unleashed protocol (liblfrfid, spec R9.0.1, R9.0.3) ---- */

static ProtocolDict *dict;	/* the 25 protocols; heap, while rfid runs */

static ProtocolDict *lf(void)
{
	if (dict == NULL) dict = lfrfid_dict_alloc();
	return dict;
}

/* Letters and digits of a name, lower case: "EM4100/32" = "em4100_32". */
static int same_name(const char *a, const char *b)
{
	for (;;) {
		while (*a && !isalnum((unsigned char) *a)) a++;
		while (*b && !isalnum((unsigned char) *b)) b++;
		if (tolower((unsigned char) *a) != tolower((unsigned char) *b))
			return 0;
		if (*a == 0) return 1;
		a++, b++;
	}
}

/* A protocol by name (EM = EM4100), or -1. */
static int lf_find(const char *name)
{
	int i;

	if (same_name(name, "EM")) return LFRFIDProtocolEM4100;
	for (i = 0; i < LFRFIDProtocolMax; i++)
		if (same_name(name, protocol_dict_get_name(lf(), i))) return i;
	return -1;
}

static size_t lf_size(int p)
{
	return protocol_dict_get_data_size(lf(), p);
}

/* "NAME B0 B1 ..." (the stock "Data:" bytes), then the stock info text. */
static void lf_print(int p, const uint8_t *data)
{
	FuriString *s;
	const char *t;
	size_t i;

	printf("%s", protocol_dict_get_name(lf(), p));
	for (i = 0; i < lf_size(p); i++) printf(" %02X", data[i]);
	printf("\n");
	s = furi_string_alloc();
	protocol_dict_set_data(lf(), p, data, lf_size(p));
	protocol_dict_render_data(lf(), s, p);
	for (t = furi_string_get_cstr(s); *t; ) {
		const char *e = strchr(t, '\n');
		int n = e ? (int) (e - t) : (int) strlen(t);

		if (n > 0) printf("  %.*s\n", n, t);
		t += n + (e != NULL);
	}
	furi_string_free(s);
}

/* The T5577 configuration word the protocol's writer uses. */
static uint32_t lf_config(int p, const uint8_t *data)
{
	LFRFIDWriteRequest req;

	protocol_dict_set_data(lf(), p, data, lf_size(p));
	if (!lfrfid_normalize(lf(), p, &req)) return 0;
	return req.t5577.block[0];
}

/* Can today's /dev/rfid capture (125 kHz, 512 edges) read it?  ASK: the
 * comparator sees Manchester, biphase or direct.  FSK has to be told apart
 * at 64/80 us in a frame longer than one capture, PSK needs the 62.5 kHz
 * carrier: T19b (R9.0.4, R9.0.5). */
static int lf_readable(uint32_t cfg)
{
	switch (t5577_config_modulation(cfg)) {
	case LFRFID_T5577_MODULATION_MANCHESTER:
	case LFRFID_T5577_MODULATION_BIPHASE:
	case LFRFID_T5577_MODULATION_DIPHASE:
	case LFRFID_T5577_MODULATION_DIRECT:
		return 1;
	}
	return 0;
}

static const char *lf_modulation(uint32_t cfg)
{
	switch (t5577_config_modulation(cfg)) {
	case LFRFID_T5577_MODULATION_DIRECT:	return "direct";
	case LFRFID_T5577_MODULATION_PSK1:	return "PSK1";
	case LFRFID_T5577_MODULATION_PSK2:	return "PSK2";
	case LFRFID_T5577_MODULATION_PSK3:	return "PSK3";
	case LFRFID_T5577_MODULATION_FSK1:	return "FSK1";
	case LFRFID_T5577_MODULATION_FSK2:	return "FSK2";
	case LFRFID_T5577_MODULATION_FSK1a:	return "FSK1a";
	case LFRFID_T5577_MODULATION_FSK2a:	return "FSK2a";
	case LFRFID_T5577_MODULATION_MANCHESTER: return "Manchester";
	case LFRFID_T5577_MODULATION_BIPHASE:	return "biphase";
	case LFRFID_T5577_MODULATION_DIPHASE:	return "diphase";
	}
	return "?";
}

static int lf_rate(uint32_t cfg)
{
	static const unsigned char rf[8] = { 8, 16, 32, 40, 50, 64, 100, 128 };

	return rf[(cfg & LFRFID_T5577_BITRATE_MASK) >> LFRFID_T5577_BITRATE_SHIFT];
}

/* The readers of one read: the capture's first level is unknown, so it is
 * fed both ways, each with its own count of equal reads (lfrfid_read.c:
 * the stock rule, validate_count + 1 in a row, across captures). */
static LfrfidReader rd[2];
static int rd_on;

/* One capture d[0..n-1] through liblfrfid: the protocol once validated
 * (its data in *data), else -1.  *half: a protocol is half validated. */
static int lf_capture(int n, uint8_t *data, int *half)
{
	int pol;
	ProtocolId p;

	if (!rd_on) {
		lfrfid_reader_start(&rd[0], lf(), LFRFIDFeatureASK);
		lfrfid_reader_start(&rd[1], lf(), LFRFIDFeatureASK);
		rd_on = 1;
	}
	for (pol = 0; pol < 2; pol++) {
		p = lfrfid_reader_feed_edges(&rd[pol], (const uint32_t *) d,
						(size_t) n, pol == 0);
		lfrfid_reader_gap(&rd[pol]);	/* the next capture is apart */
		if (p != PROTOCOL_NO) {
			memcpy(data, rd[pol].data, lf_size(p));
			return p;
		}
	}
	*half = rd[0].last != PROTOCOL_NO || rd[1].last != PROTOCOL_NO;
	return -1;
}

/* Read a tag with fd open: EM4100 RF/64 by its own decoder (R6.5), else
 * any ASK protocol.  The protocol (data in data[]), or -1.  TRIES reads,
 * up to 3 times as many while a protocol is half validated. */
static int read_any(int fd, int tries, int raw, uint8_t *data, int *nlast)
{
	int t, n = 0, i, p, half = 0;
	unsigned char id[5];

	rd_on = 0;
	for (t = 0; t < tries || (half && t < 3 * tries); t++) {
		if ((n = read(fd, d, sizeof(d))) < 0) {
			perror("rfid");
			return -2;
		}
		n /= 4;
		if (raw) {
			printf("%d:", n);
			for (i = 0; i < n && i < 200; i++) printf(" %lu", d[i]);
			printf("\n");
		}
		if (em_decode(n, id)) {
			memcpy(data, id, 5);
			*nlast = n;
			return LFRFIDProtocolEM4100;
		}
		if ((p = lf_capture(n, data, &half)) >= 0) {
			*nlast = n;
			return p;
		}
	}
	*nlast = n;
	return -1;
}

/* ---- .rfid files (the stock format: Key type = the Unleashed name) ---- */

static int save_rfid(const char *path, int p, const uint8_t *data)
{
	FILE *f;
	size_t i;

	if ((f = fopen(path, "w")) == NULL) { perror(path); return -1; }
	fprintf(f, "Filetype: Flipper RFID key\nVersion: 1\nKey type: %s\n"
		"# Data size for %s is %u\nData:", protocol_dict_get_name(lf(), p),
		protocol_dict_get_name(lf(), p), (unsigned) lf_size(p));
	for (i = 0; i < lf_size(p); i++) fprintf(f, " %02X", data[i]);
	fprintf(f, "\n");
	if (fclose(f) != 0) { perror(path); return -1; }
	return 0;
}

/* Hex bytes from the strings s[0..n-1]: words split at blanks, each one
 * byte of 1 or 2 digits (as `write EM 0 DE AD BE EF` always took) or an
 * even run of digits ("00DEADBEEF").  The count, or -1 on anything else or
 * more than max. */
static int parse_hex(char **s, int n, uint8_t *out, int max)
{
	int k = 0, i, len, j;
	const char *c;

	for (i = 0; i < n; i++)
		for (c = s[i]; *c; c += len) {
			if (isspace((unsigned char) *c)) { len = 1; continue; }
			for (len = 0; c[len] && !isspace((unsigned char) c[len]); len++)
				if (!isxdigit((unsigned char) c[len])) return -1;
			if (len > 2 && len % 2) return -1;
			for (j = 0; j < len; j += 2) {
				char two[3];

				if (k >= max) return -1;
				two[0] = c[j];
				two[1] = len == 1 ? 0 : c[j + 1];
				two[2] = 0;
				out[k++] = (uint8_t) strtoul(two, (char **) NULL, 16);
			}
		}
	return k;
}

static int load_rfid(const char *path, uint8_t *data)
{
	char line[200], *v;
	FILE *f;
	int p = -1, n = -1;

	if ((f = fopen(path, "r")) == NULL) { perror(path); return -1; }
	while (fgets(line, sizeof line, f) != NULL) {
		if (strncmp(line, "Key type:", 9) == 0) {
			for (v = line + 9; *v == ' '; v++) ;
			v[strcspn(v, "\r\n")] = 0;
			if ((p = lf_find(v)) < 0)
				fprintf(stderr, "rfid: %s: unknown key type %s\n",
					path, v);
		} else if (strncmp(line, "Data:", 5) == 0) {
			v = line + 5;
			n = parse_hex(&v, 1, data, LFRFID_DATA_MAX);
		}
	}
	fclose(f);
	if (p < 0) return -1;
	if (n != (int) lf_size(p)) {
		fprintf(stderr, "rfid: %s: %s data is %u bytes\n", path,
			protocol_dict_get_name(lf(), p), (unsigned) lf_size(p));
		return -1;
	}
	return p;
}

/* PROTO HEX... or -f FILE from argv[i..argc-1]: the protocol, data in
 * data[], or -1 after saying why. */
static int tag_args(int argc, char **argv, int i, uint8_t *data)
{
	int p, n;

	if (i + 2 == argc && strcmp(argv[i], "-f") == 0)
		return load_rfid(argv[i + 1], data);
	if (i >= argc) return -1;
	if ((p = lf_find(argv[i])) < 0) {
		fprintf(stderr, "rfid: no protocol %s (see rfid list)\n", argv[i]);
		return -1;
	}
	n = parse_hex(argv + i + 1, argc - i - 1, data, LFRFID_DATA_MAX);
	if (n != (int) lf_size(p)) {
		fprintf(stderr, "rfid: %s takes %u data bytes\n",
			protocol_dict_get_name(lf(), p), (unsigned) lf_size(p));
		return -1;
	}
	return p;
}

/* Read the tag back as protocol p with data: 1 yes, 0 something else
 * (got[] then holds what), -1 nothing. */
static int verify_lf(int fd, int p, const uint8_t *data, int *gotp,
	uint8_t *got)
{
	int n;

	*gotp = read_any(fd, 4, 0, got, &n);
	if (*gotp < 0) return -1;
	return *gotp == p && memcmp(got, data, lf_size(p)) == 0;
}

/* rfid write PROTO HEX...: any protocol, through its Unleashed writer
 * (write_data fills the T5577 blocks, or the EM4305 words), played as the
 * EM4100 write is.  The data blocks first and the configuration last (once
 * the PWD bit is set a write needs the password).  A tag of an ASK
 * protocol is read back, as stock; FSK and PSK ones cannot be read until
 * T19b, so they are written once and said to be unverified. */
static int write_lf(int p, uint8_t *data, int tries, int em_only)
{
	LFRFIDWriteRequest req, ereq;
	uint8_t got[LFRFID_DATA_MAX];
	const char *name = protocol_dict_get_name(lf(), p);
	int fd, i, v = -1, gp, can_em, readable;
	uint32_t cfg;

	protocol_dict_set_data(lf(), p, data, lf_size(p));
	if (!lfrfid_normalize(lf(), p, &req)) {
		fprintf(stderr, "rfid: %s cannot be written\n", name);
		return 1;
	}
	protocol_dict_get_data(lf(), p, data, lf_size(p)); /* as written */
	memset(&ereq, 0, sizeof ereq);
	ereq.write_type = LFRFIDWriteTypeEM4305;
	can_em = protocol_dict_get_write_data(lf(), p, &ereq);
	cfg = req.t5577.block[0];
	readable = lf_readable(cfg);
	if (wpass && req.t5577.blocks_to_write > LFRFID_T5577_PASSWORD_BLOCK) {
		fprintf(stderr, "rfid: %s fills block 7: no room for -pass\n", name);
		return 1;
	}
	if (em_only && !can_em) {
		fprintf(stderr, "rfid: %s has no EM4305 layout\n", name);
		return 1;
	}
	if ((fd = open("/dev/rfid", O_RDWR)) < 0) {
		perror("rfid: /dev/rfid");
		return 1;
	}
	if (!readable) tries = 1;
	while (tries-- > 0) {
		if (!em_only) {
			nt = 0;
			for (i = 1; i < (int) req.t5577.blocks_to_write; i++)
				wblock(i, req.t5577.block[i]);
			if (wpass) wblock(7, wpass);
			wblock(0, cfg | (wpass || (wsend && !wlock) ? T5577_PWD : 0));
			wreset();
			if (send(fd) < 0) return 1;
			if (!readable) {
				printf("written (T5577%s), not read back: %s is %s, "
					"which rfid reads after T19b\n",
					wlock ? ", locked for ever"
					      : wpass ? ", password set" : "",
					name, lf_modulation(cfg));
				return 0;
			}
			if ((v = verify_lf(fd, p, data, &gp, got)) == 1) {
				printf("written (T5577%s): ", wlock ? ", locked for ever"
					: wpass ? ", password set" : "");
				lf_print(p, data);
				return 0;
			}
		}
		if (!can_em) continue;
		nt = 0;				/* then EM4305 */
		for (i = 0; i < EM4x05_WORD_COUNT; i++)
			if (ereq.em4305.mask & (1u << i))
				eword(i, ereq.em4305.word[i]);
		if (send(fd) < 0) return 1;
		if (!readable) {
			printf("written (EM4305), not read back: %s is %s\n",
				name, lf_modulation(cfg));
			return 0;
		}
		if ((v = verify_lf(fd, p, data, &gp, got)) == 1) {
			printf("written (EM4305): ");
			lf_print(p, data);
			return 0;
		}
	}
	if (v == 0) {
		printf("not written (locked, or a password?); the tag still reads ");
		lf_print(gp, got);
	} else printf("written, but it does not read back as %s\n", name);
	return 1;
}

/* rfid copy: read a tag (any ASK protocol), then write it to the blank one
 * put on next. */
static int copy_any(int tries)
{
	uint8_t data[LFRFID_DATA_MAX], got[LFRFID_DATA_MAX];
	int fd, p, q, n, t;

	if ((fd = open("/dev/rfid", O_RDONLY)) < 0) {
		perror("rfid: /dev/rfid");
		return 1;
	}
	if ((p = read_any(fd, 20, 0, data, &n)) < 0) {
		if (p == -1) printf("no tag to copy (hold it to the back)\n");
		return 1;
	}
	printf("read ");
	lf_print(p, data);
	printf("now put the blank one on\n");
	for (t = 0; t < 600; t++) {		/* until that one is away */
		q = read_any(fd, 1, 0, got, &n);
		if (q == -2) break;
		if (q >= 0 && (q != p || memcmp(got, data, lf_size(p)) != 0)) break;
		/* EM4100 decodes from every capture: anything else means the
		 * tag went (as before); a longer frame may need several, so
		 * only (nearly) no signal means that */
		if (q < 0 && (p == LFRFIDProtocolEM4100 || n < 16)) break;
	}
	close(fd);
	if (p == LFRFIDProtocolEM4100) return write_em(data, tries);
	return write_lf(p, data, tries, 0);
}

/* rfid emulate -runs EM ...: the EM4100, played from a run list
 * (RFIDIOCEMURUN, R9.0.6) through the smallest ring (16 pairs: a refill
 * every 8 segments, ~2 ms), so a reader that reads it proves the refill
 * path the FSK and PSK frames use. */
static int emulate_em_runs(unsigned char id[5])
{
	unsigned char b[64], h[128];
	struct rfid_emu_run *ru = (struct rfid_emu_run *) &d[0];  /* <= 1 KB */
	unsigned long *ring = &d[256];
	struct rfid_emu_runs q;
	int k, n = 0, fd;

	em_bits(id, b);
	for (k = 0; k < 64; k++) {		/* half bits: loaded or not */
		h[2 * k] = b[k] != 0;
		h[2 * k + 1] = b[k] == 0;
	}
	for (k = 0; k < 128; k++) {		/* equal neighbours: one run */
		if (n > 0 && (ru[n - 1].pulse != 0) == h[k]) {
			ru[n - 1].count++;
			continue;
		}
		ru[n].period_m1 = 31;
		ru[n].pulse = h[k] ? 32 : 0;
		ru[n].count = 1;
		ru[n].pad = 0;
		n++;
	}
	q.runs = ru;
	q.nruns = n;
	q.ring = ring;
	q.ring_n = 16;
	if ((fd = open("/dev/rfid", O_RDWR)) < 0) {
		perror("rfid: /dev/rfid");
		return 1;
	}
	if (ioctl(fd, RFIDIOCEMURUN, &q) < 0) {
		perror("rfid: emulate (runs)");
		close(fd);
		return 1;
	}
	printf("emulating EM4100 %02X %02X %02X %02X %02X from %d runs; hold to "
		"a reader, Ctrl-C to stop\n", id[0], id[1], id[2], id[3], id[4], n);
	for (;;) sleep(60);
	return 0;
}


/* Play a run list (4 uint16_t a run, struct rfid_emu_run) through a ring
 * of ring_n pairs until Ctrl-C. */
static int play_runs(uint16_t *runs, int n, int ring_n, const char *what)
{
	struct rfid_emu_runs q;
	unsigned long *ring;
	int fd;

	if ((ring = malloc(2 * ring_n * sizeof(*ring))) == NULL) {
		fprintf(stderr, "rfid: no memory for the ring\n");
		return 1;
	}
	q.runs = (struct rfid_emu_run *) runs;
	q.nruns = n;
	q.ring = ring;
	q.ring_n = ring_n;
	if ((fd = open("/dev/rfid", O_RDWR)) < 0) {
		perror("rfid: /dev/rfid");
		return 1;
	}
	if (ioctl(fd, RFIDIOCEMURUN, &q) < 0) {
		perror("rfid: emulate (runs)");
		close(fd);
		return 1;
	}
	printf("%s(%d runs); hold to a reader, Ctrl-C to stop\n", what, n);
	for (;;) sleep(60);		/* Ctrl-C ends it; close stops it */
	return 0;			/* not reached */
}

/* rfid emulate PROTO HEX...: one frame of the protocol's Unleashed encoder,
 * played round and round by the kernel at the reader's clock: as
 * RFIDIOCEMU segments (lfrfid_emu_segments, as stock's pulse_glue) when
 * the frame has at most 512, else (FSK and PSK frames: 540 to 3534) as a
 * run list of equal segments through a refilled ring (RFIDIOCEMURUN,
 * R9.0.6). */
/* The runs go into the segment buffer, which the segments did not fit:
 * 2 KB hold 256 runs of 8 B, the longest frame needs 201 (Indala224,
 * make lfrfidtest).  A separate 4 KB list ran rfid out of memory once the
 * freed 2 KB could not hold it (twoflip: "lfrfid: out of memory"). */
#define MAXRUNS	(MAXD * sizeof(unsigned long) / (4 * sizeof(uint16_t)))

static int emulate_lf(int p, uint8_t *data)
{
	unsigned long *pulse;
	struct rfid_emu e;
	uint16_t *runs;
	int nseg, n, fd;

	if ((pulse = malloc(MAXD * sizeof(*pulse))) == NULL) return 1;
	nseg = lfrfid_emu_segments(p, data, lf_size(p), (uint32_t *) d,
					(uint32_t *) pulse, MAXD);
	if (nseg < 0) {			/* a long frame: runs */
		runs = (uint16_t *) pulse;	/* the same 2 KB */
		n = lfrfid_emu_runs(p, data, lf_size(p), runs, (int) MAXRUNS);
		if (n < 0) {
			fprintf(stderr, "rfid: a %s frame is more than %d runs "
				"(%s)\n", protocol_dict_get_name(lf(), p), (int) MAXRUNS,
				lf_modulation(lf_config(p, data)));
			return 1;
		}
		printf("emulating ");
		lf_print(p, data);
		return play_runs(runs, n, 128, "");
	}
	e.dur = d;
	e.pulse = pulse;
	e.n = nseg;
	if ((fd = open("/dev/rfid", O_RDWR)) < 0) {
		perror("rfid: /dev/rfid");
		return 1;
	}
	if (ioctl(fd, RFIDIOCEMU, &e) < 0) {
		perror("rfid: emulate");
		close(fd);
		return 1;
	}
	printf("emulating ");
	lf_print(p, data);
	printf("(%d segments); hold to a reader, Ctrl-C to stop\n", nseg);
	for (;;) sleep(60);		/* Ctrl-C ends it; close stops it */
	return 0;			/* not reached */
}

/* rfid list: the protocols, how they are sent, what works today, and the
 * heap the decoders of a read take (R9.0.1 budget: 6 KB). */
static int list(void)
{
	uint8_t zero[LFRFID_DATA_MAX];
	size_t heap;
	int i;

	lfrfid_heap_bytes = 0;
	(void) lf();
	heap = lfrfid_heap_bytes;
	memset(zero, 0, sizeof zero);
	printf("%-12s %-10s %-10s %5s %5s  %s\n", "PROTOCOL", "MAKER",
		"MODULATION", "RATE", "BYTES", "READ");
	for (i = 0; i < LFRFIDProtocolMax; i++) {
		uint32_t cfg = lf_config(i, zero);

		printf("%-12s %-10.10s %-10s RF/%-3d %4u  %s\n",
			protocol_dict_get_name(lf(), i),
			protocol_dict_get_manufacturer(lf(), i),
			lf_modulation(cfg), lf_rate(cfg), (unsigned) lf_size(i),
			lf_readable(cfg) ? "yes" : "after T19b");
	}
	printf("decoders: %u B of heap while reading\n", (unsigned) heap);
	return 0;
}


/* ---- streaming capture (R9.0.5) ----
 * rfid -s SECONDS [-c MODE]: capture without a gap for that long and say
 * how it went (halves, edges, overruns, the range of the durations): the
 * device check of AC9.0.3, and the front end the T19 decoders will use. */
#define RS_WORDS	1024		/* halves of 512 edges: >= 16 ms of FSK */

static int stream_test(int secs, int carrier)
{
	struct rfid_stream st;
	unsigned long *rs_ring = malloc(RS_WORDS * sizeof(unsigned long));
	unsigned long prev = 0, d, lo = ~0UL, hi = 0, edges = 0;
	unsigned long halves = 0, over = 0;
	time_t end;
	int fd, h, i, first = 1;

	if (rs_ring == NULL) {
		fprintf(stderr, "rfid: no memory for the ring\n");
		return 1;
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
	st.ring = rs_ring;
	st.words = RS_WORDS;
	if (ioctl(fd, RFIDIOCSTREAM, &st) != 0) {
		perror("rfid: stream");
		return 1;
	}
	end = time((time_t *) 0) + secs;
	while (time((time_t *) 0) < end) {
		struct pollfd pf;

		pf.fd = fd; pf.events = POLLIN; pf.revents = 0;
		if (poll(&pf, 1, 1000) != 1) continue;	/* no tag: few edges */
		if (read(fd, (char *) &h, 4) != 4) {
			if (errno == EOVERFLOW) { over++; first = 1; continue; }
			perror("rfid: read");
			break;
		}
		halves++;
		for (i = h * (RS_WORDS / 2); i < (h + 1) * (RS_WORDS / 2); i++) {
			if (!first) {
				d = rs_ring[i] - prev;
				if (d < lo) lo = d;
				if (d > hi) hi = d;
				edges++;
			}
			prev = rs_ring[i];
			first = 0;
		}
	}
	st.words = 0;
	(void) ioctl(fd, RFIDIOCSTREAM, &st);
	close(fd);
	printf("stream %d s: %lu halves, %lu edges, %lu overruns", secs,
		halves, edges, over);
	if (edges) printf(", %lu-%lu us", lo, hi);
	printf("\n");
	return over != 0;
}

/* ---- RAW read (R9.0.8) ----
 * rfid raw FILE [-c MODE] [-t SECS]: the tag's signal as it is, for tags
 * no decoder knows, in Unleashed's .ask.raw/.psk.raw format (lib/lfrfid/
 * lfrfid_raw_file.c): a header (magic "RIFL", version 1, the carrier's
 * frequency and duty as floats, the largest block), then blocks of a
 * 32-bit length and varint pairs (LEB128): the time high, then the
 * period, us, as stock's capture reports them (high width from a rising
 * edge, period from rising to rising).  The edges come from the stream
 * (R9.0.5); its level before the first edge gives their polarity.  A
 * stream overrun skips whole halves of 512 edges, so the polarity stays;
 * only the pair across the gap is dropped. */
#define RAW_BLOCK	2048

static volatile int raw_stop;

static void raw_sig(int sig)
{
	raw_stop = 1;
}

static int raw_flush(int out, unsigned char *blk, int *n)
{
	unsigned long len = (unsigned long) *n;

	if (*n == 0) return 0;
	if (write(out, (char *) &len, 4) != 4 || write(out, (char *) blk, *n) != *n)
		return -1;
	*n = 0;
	return 0;
}

static void raw_varint(unsigned char *blk, int *n, unsigned long v)
{
	while (v >= 0x80) {
		blk[(*n)++] = (unsigned char) (v | 0x80);
		v >>= 7;
	}
	blk[(*n)++] = (unsigned char) v;
}

static int raw_read(char *path, int secs, int carrier)
{
	static const unsigned long hz[3] = { 0x47F42400UL, 0x47742400UL,
					      0x48030E00UL };	/* floats */
	unsigned long hdr[5], *ring = malloc(1024 * sizeof(unsigned long));
	unsigned char *blk = malloc(RAW_BLOCK);
	unsigned long rise = 0, fall = 0, pairs = 0, over = 0;
	struct rfid_stream st;
	int fd, out, h, i, n = 0, level, have_rise = 0, have_fall = 0, r = 0;
	time_t end = time((time_t *) 0) + secs;

	if (ring == NULL || blk == NULL) {
		fprintf(stderr, "rfid: no memory\n");
		return 1;
	}
	if ((out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
		perror(path);
		return 1;
	}
	hdr[0] = 0x4C464952UL;			/* "RIFL" */
	hdr[1] = 1;
	hdr[2] = hz[carrier];
	hdr[3] = carrier == RFID_CARRIER_PSK ? 0x3E800000UL : 0x3F000000UL;
	hdr[4] = RAW_BLOCK;
	if (write(out, (char *) hdr, sizeof hdr) != sizeof hdr) {
		perror(path);
		return 1;
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
	st.ring = ring;
	st.words = 1024;
	if (ioctl(fd, RFIDIOCSTREAM, &st) != 0) {
		perror("rfid: stream");
		return 1;
	}
	level = st.level;
	signal(SIGINT, raw_sig);
	printf("recording to %s for %d s (Ctrl-C ends it)...\n", path, secs);
	fflush(stdout);
	while (!raw_stop && time((time_t *) 0) < end) {
		struct pollfd pf;

		pf.fd = fd; pf.events = POLLIN; pf.revents = 0;
		if (poll(&pf, 1, 1000) != 1) continue;
		if (read(fd, (char *) &h, 4) != 4) {
			if (errno == EOVERFLOW) {	/* whole halves skipped */
				over++;
				have_rise = have_fall = 0;
				continue;
			}
			if (errno == EINTR) continue;
			perror("rfid: read");
			r = 1;
			break;
		}
		for (i = h * 512; i < (h + 1) * 512; i++) {
			level = !level;			/* this edge's new level */
			if (level) {			/* rising */
				if (have_rise) {
					raw_varint(blk, &n, have_fall
						? fall - rise : 0);
					raw_varint(blk, &n, ring[i] - rise);
					pairs++;
					if (n > RAW_BLOCK - 10
					    && raw_flush(out, blk, &n) < 0) {
						perror(path);
						raw_stop = r = 1;
						break;
					}
				}
				rise = ring[i];
				have_rise = 1;
				have_fall = 0;
			} else if (have_rise) {
				fall = ring[i];
				have_fall = 1;
			}
		}
	}
	st.words = 0;
	(void) ioctl(fd, RFIDIOCSTREAM, &st);
	close(fd);
	if (raw_flush(out, blk, &n) < 0 || close(out) < 0) {
		perror(path);
		r = 1;
	}
	printf("%lu pairs, %lu overruns\n", pairs, over);
	return r || over != 0;
}

/* rfid read, gapless (R9.0.5): when the short captures found nothing, the
 * stream's edges go straight into the decoders for up to secs seconds, so
 * frames longer than one capture are read too (RF/32 and RF/16, FDX-B,
 * Gallagher, the FSK protocols: twoflip U->M saw "511 changes", not
 * decoded).  Both polarities, as lf_capture: after an overrun the parity
 * of the edges is lost.  Durations go into d[] (no stack).  The protocol
 * (data in data[]), or -1. */
static int stream_read(int fd, int secs, uint8_t *data)
{
	struct rfid_stream st;
	unsigned long *ring = malloc(RS_WORDS * sizeof(unsigned long));
	unsigned long prev = 0;
	time_t end;
	int h, i, n, first = 1, lv = 0, pol, p = -1;

	if (ring == NULL) return -1;
	st.ring = ring;
	st.words = RS_WORDS;
	if (ioctl(fd, RFIDIOCSTREAM, &st) != 0) {
		free(ring);
		return -1;
	}
	lv = !st.level;			/* after the first edge */
	rd_on = 0;
	(void) lf_capture(0, data, &h);	/* both readers started */
	end = time((time_t *) 0) + secs;
	while (p < 0 && time((time_t *) 0) < end) {
		struct pollfd pf;

		pf.fd = fd; pf.events = POLLIN; pf.revents = 0;
		if (poll(&pf, 1, 500) != 1) continue;
		if (read(fd, (char *) &h, 4) != 4) {
			if (errno != EOVERFLOW) break;
			first = 1;		/* edges lost: start over */
			for (pol = 0; pol < 2; pol++) lfrfid_reader_gap(&rd[pol]);
			continue;
		}
		for (n = 0, i = h * (RS_WORDS / 2); i < (h + 1) * (RS_WORDS / 2);
									i++) {
			if (!first && n < MAXD) d[n++] = ring[i] - prev;
			prev = ring[i];
			first = 0;
		}
		for (pol = 0; pol < 2 && p < 0; pol++) {
			ProtocolId q = lfrfid_reader_feed_edges(&rd[pol],
				(const uint32_t *) d, (size_t) n, lv ^ pol);

			if (q != PROTOCOL_NO) {
				memcpy(data, rd[pol].data, lf_size(q));
				p = q;
			}
		}
		lv ^= n & 1;
	}
	st.words = 0;
	(void) ioctl(fd, RFIDIOCSTREAM, &st);
	free(ring);
	return p;
}

int main(int argc, char **argv)
{
	int carrier = RFID_CARRIER_125, stream = 0;
	int fd, n, i, tries = 4, raw = 0, p;
	unsigned char id[5];
	uint8_t data[LFRFID_DATA_MAX];
	const char *out = NULL;

	if (argc >= 2 && strcmp(argv[1], "list") == 0) {
		if (argc != 2) goto use;
		return list();
	}

	if (argc >= 2 && strcmp(argv[1], "emulate") == 0) {
		int runs = argc >= 3 && strcmp(argv[2], "-runs") == 0;

		if ((p = tag_args(argc, argv, 2 + runs, data)) < 0) goto use;
		/* EM4100 at RF/64 keeps its own waveform (R6.5 part 4); -runs
		 * plays it from a run list instead (the R9.0.6 check) */
		if (p == LFRFIDProtocolEM4100)
			return runs ? emulate_em_runs(data) : emulate_em(data);
		return emulate_lf(p, data);
	}

	if (argc >= 3 && strcmp(argv[1], "raw") == 0) {	/* R9.0.8 */
		int secs = 10, c = RFID_CARRIER_125;

		for (i = 3; i < argc; i++) {
			if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
				secs = atoi(argv[++i]);
			else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
				i++;
				if (strcmp(argv[i], "psk") == 0)
					c = RFID_CARRIER_PSK;
				else if (strcmp(argv[i], "134") == 0)
					c = RFID_CARRIER_134;
				else if (strcmp(argv[i], "125") != 0) goto use;
			} else goto use;
		}
		if (secs < 1) goto use;
		return raw_read(argv[2], secs, c);
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
		int tries = 20, cp = argv[1][0] == 'c', em_only = 0;

		for (i = 2; i < argc && argv[i][0] == '-'
				&& strcmp(argv[i], "-f") != 0; i++) {
			char *e;

			if (strcmp(argv[i], "-lock") == 0) wlock = 1;
			else if (strcmp(argv[i], "-em4305") == 0) em_only = 1;
			else if (strcmp(argv[i], "-hitag") == 0) whitag = 1;
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
		if (em_only && (wlock || wpass || wsend)) goto use;
		if (cp) {
			if (i != argc || em_only) goto use;
			return copy_any(tries);
		}
		if ((p = tag_args(argc, argv, i, data)) < 0) goto use;
		/* -hitag: Hitag micro chips take an EM4100 frame only, and
		 * not with a password, a lock or -em4305 (R9.0.7) */
		if (whitag && (p != LFRFIDProtocolEM4100 || em_only || wlock
							|| wpass || wsend))
			goto use;
		/* EM4100 at RF/64: its own writer, as before (R6.5) */
		if (p == LFRFIDProtocolEM4100 && !em_only)
			return write_em(data, tries);
		return write_lf(p, data, tries, em_only);
	}
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-r") == 0) raw = 1;
		else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
			tries = atoi(argv[++i]);
		else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
			stream = atoi(argv[++i]);
		else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
			out = argv[++i];
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
use:			fprintf(stderr, "usage: rfid [-r] [-n TRIES] [-o FILE] "
				"[-c 125|psk|134] [-s SECS]\n"
				"       rfid write [-n N] [-pass HEX] [-with HEX] "
				"[-lock] [-em4305] [-hitag] PROTO HEX...|-f FILE\n"
				"       rfid copy [-n N] [-pass HEX] "
				"[-with HEX] [-lock]\n"
				"       rfid clear [-with HEX] [-n N]\n"
				"       rfid dump [-with HEX] [BLOCK]\n"
				"       rfid emulate [-runs] PROTO HEX...|-f FILE\n"
				"       rfid raw FILE [-c 125|psk|134] [-t SECS]\n"
				"       rfid list\n");
			return 2;
		}
	}
	if (stream > 0) return stream_test(stream, carrier);
	if ((fd = open("/dev/rfid", O_RDONLY)) < 0) {
		perror("rfid: /dev/rfid");
		return 1;
	}
	if (carrier != RFID_CARRIER_125
			&& ioctl(fd, RFIDIOCCARRIER, &carrier) != 0) {
		perror("rfid: carrier");
		return 1;
	}
	if ((p = read_any(fd, tries, raw, data, &n)) >= 0) {
		lf_print(p, data);
		if (out != NULL && save_rfid(out, p, data) < 0) return 1;
		return 0;
	}
	if (p == -2) return 1;
	{
		unsigned long lo = ~0UL, hi = 0, sum = 0;

		for (i = 0; i < n; i++) {
			if (d[i] < lo) lo = d[i];
			if (d[i] > hi) hi = d[i];
			sum += d[i];
		}
		if (!raw && (p = stream_read(fd, 3, data)) >= 0) {
			lf_print(p, data);
			if (out != NULL && save_rfid(out, p, data) < 0)
				return 1;
			return 0;
		}
		if (n == 0) printf("no tag (no signal)\n");
		else printf("no tag rfid reads (ASK); %d changes, %lu-%lu us, "
			"mean %lu us\n", n, lo, hi, sum / n);
	}
	return 1;
}
