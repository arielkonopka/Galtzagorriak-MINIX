/* ikey - iButton keys (Galtzagorriak MINIX, spec R6.7), as the stock CLI's
 * "ikey" command
 *
 * Usage:	ikey read [-n TRIES] [-o FILE]
 *					wait for a key on the contacts and
 *					print it as stock does: "Dallas"
 *					and the 8 ROM bytes in hex, then
 *					its family (DS1992, ...).  -o FILE
 *					also saves it as a stock .ibtn file,
 *					with the memory of a DS1992, DS1996
 *					or DS1971 (issue #490)
 *		ikey write [-b BLANK] Dallas KEY | FILE
 *					write KEY (16 hex digits, the ROM
 *					with its CRC) to a blank held on the
 *					contacts, then read it back.  BLANK:
 *					rw1990 (both kinds, then tm2004: the
 *					default), tm2004, tm01x, all.  A FILE
 *					of a Cyfral or Metakom key goes to a
 *					TM01x blank, which is finalized: it
 *					cannot be written again
 *		ikey copy FILE			the memory of a DS1992, DS1996 or
 *					DS1971 file onto such a key on the
 *					contacts, then read back
 *		ikey emulate [-t SECS] Dallas|DSxxxx|Cyfral|Metakom KEY | FILE
 *					be the key for a reader on the
 *					contacts until Ctrl-C (or SECS),
 *					saying each time a reader got it
 *
 * KEY is hex as `ikey read` and the stock CLI print it: Dallas 16 digits,
 * Cyfral 4 (e.g. A1B2), Metakom 8 (every byte of even parity).  A Dallas
 * key may be called by its family (DS1990, DS1992, DS1996, DS1971, DS1420,
 * DSGeneric); the ROM is what is sent.  The memory of a DS1992, DS1996 or
 * DS1971 is read and written (read -o, copy) but not emulated: a reader
 * that only asks for the ROM, as door readers do, is served.
 *
 * Reading is /dev/ibutton (the kernel does the 1-Wire READ ROM and the
 * CRC).  Writing an RW1990 is Unleashed's lib/ibutton/protocols/blanks/
 * rw1990.c: the two kinds in turn (RW1990.1: unlock 0xD1 + a 0 bit, the
 * key inverted, lock 0xD1 + a 1; RW1990.2: unlock 0x1D + a 1, the key as
 * it is, lock 0x1D + a 0; bits LSB first in both), each bit followed by a
 * programming pause, each byte by
 * a longer one, then READ ROM to compare.  The kernel does the 1-Wire
 * slots (IBIOCRUN); the pauses are ours (poll(), whole clock ticks: longer
 * than stock's 5 and 30 ms, which the EEPROM does not mind).
 * Emulating is the kernel's too (IBIOCEMU: the 1-Wire slave answers READ
 * ROM and SEARCH ROM); ikey only asks now and then how many readers got
 * the key.
 * Cyfral and Metakom keys (R6.7 part 3b) are not 1-Wire: they send their
 * code round and round as long and short levels.  Reading captures the
 * line's levels as durations (IBIOCCAPT) and decodes them here; emulating
 * builds one frame as a pulse list and the kernel plays it in a loop
 * (IBIOCPLAY).  The line code is Unleashed's (src/lib/ibkey).
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <minix/ibutton.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ibkey.h"

#define CAPT	384			/* durations: 3-4 frames of either (the
					 * Flipper takes 192 timestamps) */
static unsigned short capt[CAPT];

static void pause_ms(int ms)
{
	(void) poll((struct pollfd *) NULL, 0, ms);
}

static void usage(void)
{
	fprintf(stderr, "usage: ikey read [-n TRIES] [-o FILE]\n"
		"       ikey write [-b rw1990|tm2004|tm01x|all] Dallas KEY | FILE\n"
		"       ikey copy FILE\n"
		"       ikey emulate [-t SECS] Dallas|DSxxxx|Cyfral|Metakom KEY | FILE\n");
	exit(2);
}

static int crc_ok(const unsigned char *rom)
{
	return ibd_crc8(rom, 7) == rom[7];
}

static void print_key(const char *name, const unsigned char *k, int n)
{
	int i;

	printf("%s ", name);
	for (i = 0; i < n; i++) printf("%02X", k[i]);
	printf("\n");
}

/* A Cyfral or Metakom key on the contacts: 1 found (printed), 0 none,
 * -1 this board cannot capture (yet). */
static int read_misc(int fd, unsigned char *key, int *type)
{
	struct ib_capt q;
	int t;

	q.dur = capt;
	q.n = CAPT;
	q.ms = 60;			/* a window of 60 ms: 5 or 6 frames */
	if (ioctl(fd, IBIOCCAPT, &q) < 0) return errno == ENXIO ? -1 : 0;
	/* The first level is the comparator's, which the stock firmware
	 * inverts; if the hardware has it the other way, the decoders find
	 * the key with the opposite one. */
	t = ibk_decode(capt, (int) q.got, q.level, key);
	if (t == 0) t = ibk_decode(capt, (int) q.got, !q.level, key);
	if (t == 0) return 0;
	*type = t;
	print_key(ibk_name(t), key, ibk_size(t));
	return 1;
}

/* ---- 1-Wire master steps (IBIOCRUN) ---- */

/* IBIOCRUN with what is in q; -1 on an error. */
static int run(int fd, struct ib_run *q)
{
	if (ioctl(fd, IBIOCRUN, q) != 0) {
		perror("ikey: 1-Wire");
		return -1;
	}
	return 0;
}

static void op(struct ib_run *q, int o)
{
	q->op[q->nop++] = (unsigned char) o;
}

static void wbyte(struct ib_run *q, int b)
{
	op(q, IB_WBYTE);
	op(q, b);
}

/* Reset, then SKIP ROM (0xCC): 1 a part answered, 0 none, -1 an error. */
static int select_all(int fd)
{
	struct ib_run q;

	q.nop = 0;
	op(&q, IB_RESET);
	wbyte(&q, 0xCC);
	if (run(fd, &q) < 0) return -1;
	return q.out[0];
}

/* Write n bytes, at most 15 a call (IB_MAXOP). */
static int wbytes(int fd, const unsigned char *b, int n)
{
	struct ib_run q;
	int i, j, k;

	for (i = 0; i < n; i += k) {
		k = n - i < 15 ? n - i : 15;
		q.nop = 0;
		for (j = 0; j < k; j++) wbyte(&q, b[i + j]);
		if (run(fd, &q) < 0) return -1;
	}
	return 0;
}

/* Read n bytes, at most 16 a call (IB_MAXOUT). */
static int rbytes(int fd, unsigned char *b, int n)
{
	struct ib_run q;
	int i, j, k;

	for (i = 0; i < n; i += k) {
		k = n - i < 16 ? n - i : 16;
		q.nop = 0;
		for (j = 0; j < k; j++) op(&q, IB_RBYTE);
		if (run(fd, &q) < 0) return -1;
		for (j = 0; j < k; j++) b[i + j] = q.out[j];
	}
	return 0;
}

/* ---- .ibtn files (stock: Filetype/Version/Protocol/Rom Data/...) ---- */

struct kfile {
	char proto[24];			/* "DS1990", "Cyfral", ... */
	unsigned char rom[8];		/* a Dallas key */
	int nrom;
	unsigned char data[IBK_MAXKEY];	/* Cyfral, Metakom */
	int ndata;
	long memoff;			/* the memory's hex digits, or -1 */
};

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

/* The next byte as two hex digits (spaces skipped) from f; -1 at the
 * end of the line or an error. */
static int next_byte(FILE *f)
{
	int c, h, l;

	while ((c = getc(f)) == ' ') ;
	if ((h = hexval(c)) < 0) return -1;
	if ((l = hexval(getc(f))) < 0) return -1;
	return h << 4 | l;
}

static void skip_line(FILE *f)
{
	int c;

	while ((c = getc(f)) != '\n' && c != EOF) ;
}

/* Parse a stock .ibtn (version 1 or 2).  0, or -1 (a message printed). */
static int kf_load(const char *path, struct kfile *k, FILE **keep)
{
	FILE *f;
	char key[24], *v;
	int c, n, b;

	memset(k, 0, sizeof *k);
	k->memoff = -1;
	if ((f = fopen(path, "r")) == NULL) {
		perror(path);
		return -1;
	}
	for (;;) {
		n = 0;
		while ((c = getc(f)) != EOF && c != ':' && c != '\n'
						&& n < (int) sizeof key - 1)
			key[n++] = (char) c;
		key[n] = 0;
		if (c == EOF) break;
		if (c != ':') {
			if (c != '\n') skip_line(f);
			continue;
		}
		(void) getc(f);			/* the space after ':' */
		if (!strcmp(key, "Protocol") || !strcmp(key, "Key type")) {
			for (v = k->proto, n = 0; n < (int) sizeof k->proto - 1
				&& (c = getc(f)) != '\n' && c != EOF; n++)
				*v++ = (char) c;
			*v = 0;
			if (c != '\n' && c != EOF) skip_line(f);
		} else if (!strcmp(key, "Rom Data") || !strcmp(key, "Data")) {
			unsigned char *d = k->data;
			int *cnt = &k->ndata, max = IBK_MAXKEY;

			/* "Data" is the ROM in version 1 (a Dallas key) */
			if (!strcmp(key, "Rom Data")) {
				d = k->rom;
				cnt = &k->nrom;
				max = 8;
			} else if (!strncmp(k->proto, "DS", 2)
					|| !strcmp(k->proto, "Dallas")) {
				d = k->rom;
				cnt = &k->nrom;
				max = 8;
			}
			while (*cnt < max && (b = next_byte(f)) >= 0)
				d[(*cnt)++] = (unsigned char) b;
			skip_line(f);
		} else if (!strcmp(key, "Sram Data")
					|| !strcmp(key, "Eeprom Data")) {
			k->memoff = ftell(f);
			skip_line(f);
		} else {
			skip_line(f);
		}
	}
	if (keep != NULL) *keep = f;
	else fclose(f);
	if (k->proto[0] == 0) {
		fprintf(stderr, "ikey: %s: not an iButton key file\n", path);
		if (keep != NULL) fclose(f);
		return -1;
	}
	return 0;
}

/* ---- reading ---- */

/* The memory of family f, from the key selected by SKIP ROM, to out as a
 * stock hex line ("Sram Data: AA BB ..."), 16 bytes at a time; or, with
 * out NULL, into buf (compare).  0, or -1. */
static int read_mem(int fd, const struct ibd_fam *f, FILE *out,
	unsigned char *buf)
{
	unsigned char cmd[3], b[16];
	int i, j, n;

	if (select_all(fd) <= 0) return -1;
	cmd[0] = 0xF0;
	cmd[1] = cmd[2] = 0;		/* from address 0 */
	if (wbytes(fd, cmd, 1 + f->abytes) < 0) return -1;
	if (out != NULL) fprintf(out, "%s:", ibd_memkey(f));
	for (i = 0; i < f->size; i += n) {
		n = f->size - i < 16 ? f->size - i : 16;
		if (rbytes(fd, b, n) < 0) return -1;
		if (out != NULL) {
			for (j = 0; j < n; j++) fprintf(out, " %02X", b[j]);
		} else {
			memcpy(buf + i, b, n);
		}
	}
	if (out != NULL) fputc('\n', out);
	return 0;
}

static int save_file(const char *path, int type, const unsigned char *key,
	const struct ibd_fam *f, int fd)
{
	FILE *out;
	int i, n, r = 0;

	if ((out = fopen(path, "w")) == NULL) {
		perror(path);
		return -1;
	}
	fprintf(out, "Filetype: Flipper iButton key\nVersion: 2\nProtocol: %s\n",
		type ? ibk_name(type) : f->name);
	fprintf(out, type ? "Data:" : "Rom Data:");
	n = type ? ibk_size(type) : 8;
	for (i = 0; i < n; i++) fprintf(out, " %02X", key[i]);
	fputc('\n', out);
	if (!type && f->mem != IBD_NOMEM) r = read_mem(fd, f, out, NULL);
	if (fclose(out) != 0 || r < 0) {
		fprintf(stderr, "ikey: %s: could not write all of it\n", path);
		return -1;
	}
	return 0;
}

static int read_key(int tries, const char *outfile)
{
	unsigned char rom[8], key[IBK_MAXKEY];
	const struct ibd_fam *f;
	int fd, n, t = 0, misc = 1;

	printf("Reading iButton...\nPress Ctrl+C to abort\n");
	fflush(stdout);
	while (tries-- > 0) {
		if ((fd = open("/dev/ibutton", O_RDWR)) < 0) {
			perror("ikey: /dev/ibutton");
			return 1;
		}
		n = read(fd, (char *) rom, 8);
		if (n == 8) {
			f = ibd_bycode(rom[0]);
			print_key("Dallas", rom, 8);
			if (f->mem != IBD_NOMEM)
				printf("%s: %s %d B\n", f->name,
					f->mem == IBD_SRAM ? "SRAM" : "EEPROM",
					(int) f->size);
			else printf("%s\n", f->name);
			fflush(stdout);
			if (outfile != NULL && save_file(outfile, 0, rom, f, fd) < 0) {
				close(fd);
				return 1;
			}
			close(fd);
			return 0;
		}
		if (misc && (n = read_misc(fd, key, &t)) != 0) {	/* Cyfral, Metakom */
			if (n > 0) {
				if (outfile != NULL
				    && save_file(outfile, t, key, NULL, fd) < 0) {
					close(fd);
					return 1;
				}
				close(fd);
				return 0;
			}
			close(fd);
			printf("ikey: Cyfral and Metakom keys: not implemented "
				"on this board yet, Dallas only\n");
			fflush(stdout);
			misc = 0;
		} else {
			close(fd);
		}
		pause_ms(250);			/* ENODEV: no key yet */
	}
	printf("no key\n");
	return 1;
}

/* ---- writing a ROM onto a blank ---- */

static int read_back(int fd, const unsigned char *key)
{
	struct ib_run q;
	int i;

	q.nop = 0;
	op(&q, IB_RESET);
	wbyte(&q, 0x33);			/* READ ROM */
	for (i = 0; i < 8; i++) op(&q, IB_RBYTE);
	if (run(fd, &q) < 0) return -1;
	return q.nout == 9 && q.out[0] && memcmp(q.out + 1, key, 8) == 0;
}

/* One step: reset, a command byte, one bit, a pause (unlock or lock). */
static int flag(int fd, int reset, int cmd, int bit, int ms)
{
	struct ib_run q;

	q.nop = 0;
	if (reset) op(&q, IB_RESET);
	wbyte(&q, cmd);
	op(&q, bit ? IB_W1 : IB_W0);
	if (run(fd, &q) < 0) return -1;
	pause_ms(ms);
	return 0;
}

/* RW1990.1 (v = 1) or .2 (v = 2); 1 written and read back, 0 not, -1 an
 * error. */
static int rw1990(int fd, const unsigned char *key, int v)
{
	struct ib_run q;
	int i, b, byte;

	if (flag(fd, 1, v == 1 ? 0xD1 : 0x1D, v == 2, 5) < 0) return -1;
	q.nop = 0;				/* the data */
	op(&q, IB_RESET);
	wbyte(&q, 0xD5);			/* WRITE ROM */
	if (run(fd, &q) < 0) return -1;
	for (i = 0; i < 8; i++) {
		byte = v == 1 ? ~key[i] & 0xFF : key[i];
		for (b = 0; b < 8; b++) {	/* LSB first, a pause after each */
			q.nop = 0;
			op(&q, (byte >> b & 1) ? IB_W1 : IB_W0);
			if (run(fd, &q) < 0) return -1;
			pause_ms(5);
		}
		pause_ms(30);
	}
	if (flag(fd, 0, v == 1 ? 0xD1 : 0x1D, v == 1, 10) < 0) return -1;
	return read_back(fd, key);
}

/* TM2004 (Unleashed blanks/tm2004.c): WRITE ROM (0x3C) from address 0; for
 * each byte: send it, read the blank's answer, a programming pulse (a 1
 * bit), wait, read the byte back. */
static int tm2004(int fd, const unsigned char *key)
{
	struct ib_run q;
	int i;

	q.nop = 0;
	op(&q, IB_RESET);
	wbyte(&q, 0x3C);
	wbyte(&q, 0);
	wbyte(&q, 0);
	if (run(fd, &q) < 0) return -1;
	if (!q.out[0]) return 0;		/* nobody on the contacts */
	for (i = 0; i < 8; i++) {
		q.nop = 0;
		wbyte(&q, key[i]);
		op(&q, IB_RBYTE);		/* the blank's answer (CRC) */
		if (run(fd, &q) < 0) return -1;
		q.nop = 0;
		op(&q, IB_W1);			/* the pulse that programs it */
		if (run(fd, &q) < 0) return -1;
		pause_ms(50);
		q.nop = 0;
		op(&q, IB_RBYTE);
		if (run(fd, &q) < 0) return -1;
		if (q.out[0] != key[i]) return 0;
	}
	return read_back(fd, key);
}

/* TM01x, a Dallas ROM (blanks/tm01x.c tm01x_write_dallas): flag 0xC1 (a 1
 * bit), WRITE ROM 0xC5, 8 bytes LSB first with 5 ms after each bit, then
 * READ ROM.  Unleashed's own timings for these blanks differ a little
 * (a 740 us reset, a presence sample at 140 us); here the standard ones
 * are used and the read back says whether the blank took the key. */
static int tm01x_begin(int fd)
{
	struct ib_run q;

	q.nop = 0;
	op(&q, IB_RESET);
	wbyte(&q, 0xC1);
	op(&q, IB_W1);
	if (run(fd, &q) < 0) return -1;
	if (!q.out[0]) return 0;
	pause_ms(5);
	q.nop = 0;
	op(&q, IB_RESET);
	wbyte(&q, 0xC5);
	if (run(fd, &q) < 0) return -1;
	return 1;
}

static int tm01x_bit(int fd, int bit)
{
	struct ib_run q;

	q.nop = 0;
	op(&q, bit ? IB_W1 : IB_W0);
	if (run(fd, &q) < 0) return -1;
	pause_ms(5);
	return 0;
}

static int tm01x_dallas(int fd, const unsigned char *key)
{
	int i, b, r;

	if ((r = tm01x_begin(fd)) <= 0) return r;
	for (i = 0; i < 8; i++)
		for (b = 0; b < 8; b++)
			if (tm01x_bit(fd, key[i] >> b & 1) < 0) return -1;
	return read_back(fd, key);
}

/* TM01x, a Cyfral or Metakom frame: 36 bits, most significant first, each
 * inverted; read back (READ ROM gives it inverted and bit-reversed),
 * compared, and only then finalized (0xCA Cyfral, 0xCB Metakom), which a
 * blank cannot undo. */
static int tm01x_frame(int fd, int type, const unsigned char *key)
{
	unsigned char img[8], got[8];
	struct ib_run q;
	int i, r;

	if (ibk_tm01x_image(type, key, img) < 0) return -1;
	if ((r = tm01x_begin(fd)) <= 0) return r;
	for (i = 0; i < IBK_TM01X_BITS; i++)
		if (tm01x_bit(fd, !(img[i >> 3] >> (7 - (i & 7)) & 1)) < 0)
			return -1;
	q.nop = 0;
	op(&q, IB_RESET);
	wbyte(&q, 0x33);
	for (i = 0; i < 8; i++) op(&q, IB_RBYTE);
	if (run(fd, &q) < 0) return -1;
	if (q.out[0] == 0) return 0;
	for (i = 0; i < 8; i++) got[i] = q.out[1 + i];
	ibk_tm01x_norm(got);
	if (memcmp(got, img, 8) != 0) return 0;
	q.nop = 0;
	op(&q, IB_RESET);
	wbyte(&q, type == IBK_CYFRAL ? 0xCA : 0xCB);
	op(&q, IB_W1);
	if (run(fd, &q) < 0) return -1;
	pause_ms(10);
	return 1;
}

/* KEY: n hex digit pairs, as `ikey read` prints them without spaces. */
static void parse_hex(const char *hex, unsigned char *key, int n)
{
	char two[3];
	int i;

	if ((int) strlen(hex) != 2 * n) usage();
	for (i = 0; i < n; i++) {
		char *e;

		two[0] = hex[2 * i]; two[1] = hex[2 * i + 1]; two[2] = 0;
		key[i] = (unsigned char) strtoul(two, &e, 16);
		if (*e != 0 || two[0] == '-' || two[0] == '+' || two[0] == ' ')
			usage();
	}
}

static void parse_key(const char *hex, unsigned char *key, const char *how)
{
	parse_hex(hex, key, 8);
	if (!crc_ok(key))
		fprintf(stderr, "ikey: note: the last byte is not the CRC of the "
			"first seven (%s as given)\n", how);
}

#define B_RW1990	1
#define B_TM2004	2
#define B_TM01X		4

static int blank_mask(const char *s)
{
	if (!strcmp(s, "rw1990")) return B_RW1990 | B_TM2004;	/* the default */
	if (!strcmp(s, "tm2004")) return B_TM2004;
	if (!strcmp(s, "tm01x")) return B_TM01X;
	if (!strcmp(s, "all")) return B_RW1990 | B_TM2004 | B_TM01X;
	usage();
	return 0;
}

static int write_dallas(const unsigned char *key, int mask)
{
	int fd, r = 0, v;

	if ((fd = open("/dev/ibutton", O_RDWR)) < 0) {
		perror("ikey: /dev/ibutton");
		return 1;
	}
	printf("Writing key...\n");
	fflush(stdout);
	for (v = 1; v <= 2 && (mask & B_RW1990) && r <= 0; v++) {
		if ((r = rw1990(fd, key, v)) < 0) break;
		if (r == 1) printf("Write success (RW1990.%d)\n", v);
	}
	if (r == 0 && (mask & B_TM2004)) {
		printf("Trying TM2004...\n");
		fflush(stdout);
		if ((r = tm2004(fd, key)) == 1) printf("Write success (TM2004)\n");
	}
	if (r == 0 && (mask & B_TM01X)) {
		printf("Trying TM01x...\n");
		fflush(stdout);
		if ((r = tm01x_dallas(fd, key)) == 1)
			printf("Write success (TM01x)\n");
	}
	close(fd);
	if (r == 1) return 0;
	printf("Write failed: no blank on the contacts, or it did not "
		"take the key\n");
	return 1;
}

static int write_misc(int type, const unsigned char *key)
{
	int fd, r;

	if (ibk_valid(type, key) != 1) {
		fprintf(stderr, "ikey: Metakom: every byte needs even parity "
			"(a reader would not take it)\n");
		return 2;
	}
	if ((fd = open("/dev/ibutton", O_RDWR)) < 0) {
		perror("ikey: /dev/ibutton");
		return 1;
	}
	printf("Writing %s to a TM01x blank (it cannot be written again)...\n",
		ibk_name(type));
	fflush(stdout);
	r = tm01x_frame(fd, type, key);
	close(fd);
	if (r == 1) {
		printf("Write success (TM01x)\n");
		return 0;
	}
	printf("Write failed: no TM01x blank on the contacts, or it did not "
		"take the key\n");
	return 1;
}

/* ---- copying the memory (DS1992, DS1996, DS1971) ---- */

/* Unleashed dallas_common_write_mem, a page at a time through the
 * scratchpad: write scratchpad (0x0F), read it back (0xAA) and compare,
 * copy it (0x55 with the address registers it gave).  The DS1971 takes one
 * address byte and its copy command a key byte (0xA5), and its EEPROM needs
 * time to program. */
static int copy_mem(int fd, const struct ibd_fam *f, FILE *src)
{
	unsigned char page[32], chk[32], regs[3], cmd[4];
	int p, i, b, n;

	for (p = 0; p < f->size; p += f->page) {
		for (i = 0; i < f->page; i++) {
			if ((b = next_byte(src)) < 0) {
				fprintf(stderr, "ikey: the file has too little "
					"memory data\n");
				return -1;
			}
			page[i] = (unsigned char) b;
		}
		if (select_all(fd) <= 0) return 0;
		cmd[0] = 0x0F;
		cmd[1] = (unsigned char) p;
		cmd[2] = (unsigned char) (p >> 8);
		if (wbytes(fd, cmd, 1 + f->abytes) < 0
		    || wbytes(fd, page, f->page) < 0) return -1;
		if (select_all(fd) <= 0) return 0;
		cmd[0] = 0xAA;
		if (f->abytes == 1) {
			cmd[1] = 0;
			n = 2;
			regs[0] = 0;
		} else {
			n = 1;
		}
		if (wbytes(fd, cmd, n) < 0) return -1;
		if (f->abytes == 2 && rbytes(fd, regs, 3) < 0) return -1;
		if (rbytes(fd, chk, f->page) < 0) return -1;
		if (memcmp(page, chk, f->page) != 0) return 0;
		if (select_all(fd) <= 0) return 0;
		cmd[0] = 0x55;
		if (f->abytes == 2) {
			cmd[1] = regs[0];
			cmd[2] = regs[1];
			cmd[3] = regs[2];
			n = 4;
		} else {
			cmd[1] = 0xA5;
			n = 2;
		}
		if (wbytes(fd, cmd, n) < 0) return -1;
		pause_ms(f->mem == IBD_EEPROM ? 12 : 1);
	}
	return 1;
}

static int copy_file(const char *path)
{
	struct kfile k;
	const struct ibd_fam *f;
	unsigned char *buf;
	FILE *src;
	int fd, r, i, b, same;

	if (kf_load(path, &k, NULL) < 0) return 2;
	if ((f = ibd_byname(k.proto)) == NULL || f->mem == IBD_NOMEM
	    || k.memoff < 0) {
		fprintf(stderr, "ikey: copy: %s has no memory to copy (DS1992, "
			"DS1996 and DS1971 files have)\n", path);
		return 2;
	}
	if ((src = fopen(path, "r")) == NULL) {
		perror(path);
		return 1;
	}
	if ((fd = open("/dev/ibutton", O_RDWR)) < 0) {
		perror("ikey: /dev/ibutton");
		fclose(src);
		return 1;
	}
	printf("Writing %s memory (%d B)...\n", f->name, (int) f->size);
	fflush(stdout);
	fseek(src, k.memoff, SEEK_SET);
	if ((r = copy_mem(fd, f, src)) == 1) {
		/* read it all back and compare with the file */
		if ((buf = malloc(f->size)) == NULL) {
			r = -1;
		} else {
			fseek(src, k.memoff, SEEK_SET);
			r = read_mem(fd, f, NULL, buf) == 0;
			for (i = 0, same = 1; r && i < f->size; i++) {
				if ((b = next_byte(src)) != buf[i]) same = 0;
			}
			if (r) r = same;
			free(buf);
		}
	}
	close(fd);
	fclose(src);
	if (r == 1) {
		printf("Write success\n");
		return 0;
	}
	printf("Write failed: no %s on the contacts, or it did not take the "
		"data\n", f->name);
	return 1;
}

/* ---- emulating ---- */

/* Be the key until killed, or for secs (> 0).  The kernel answers the
 * reader; every half second we ask how many readers got the whole key. */
static int emulate_key(const unsigned char *rom, const struct ibd_fam *f,
	int secs)
{
	struct ib_emu q;
	unsigned char last = 0;
	int fd, i, n = 0;

	memcpy(q.rom, rom, 8);
	if ((fd = open("/dev/ibutton", O_RDWR)) < 0) {
		perror("ikey: /dev/ibutton");
		return 1;
	}
	q.on = 1;
	if (ioctl(fd, IBIOCEMU, &q) < 0) {
		perror(errno == EBUSY ? "ikey: another program emulates a key"
					: "ikey: emulate");
		close(fd);
		return 1;
	}
	printf("Emulating Dallas");
	for (i = 0; i < 8; i++) printf(" %02X", q.rom[i]);
	printf("\nHold the contacts to the reader; Ctrl-C stops.\n");
	if (f != NULL && f->mem != IBD_NOMEM)
		printf("(%s: the ROM only, not its memory)\n", f->name);
	fflush(stdout);
	while (secs <= 0 || n < 2 * secs) {
		pause_ms(500);
		n++;
		q.on = 1;
		if (ioctl(fd, IBIOCEMU, &q) < 0) break;
		if (q.served != last) {
			printf("Read by a reader (%u)\n",
				(unsigned) (unsigned char) (q.served - last));
			fflush(stdout);
			last = q.served;
		}
	}
	q.on = 0;
	(void) ioctl(fd, IBIOCEMU, &q);
	close(fd);
	return 0;
}

/* A Cyfral or Metakom key: one frame as a pulse list, played by the
 * kernel round and round until killed, or for secs (> 0). */
static int emulate_misc(int type, const unsigned char *key, int secs)
{
	static unsigned short dur[IBK_MAXFRAME];
	struct ib_play q;
	unsigned char last = 0;
	int fd, lv, nd, n = 0;

	if (ibk_valid(type, key) != 1) {
		fprintf(stderr, "ikey: Metakom: every byte needs even parity "
			"(a reader would not take it)\n");
		return 2;
	}
	if ((nd = ibk_frame(type, key, dur, &lv, IBK_MAXFRAME)) < 0) return 2;
	if ((fd = open("/dev/ibutton", O_RDWR)) < 0) {
		perror("ikey: /dev/ibutton");
		return 1;
	}
	q.dur = dur;
	q.n = (unsigned) nd;
	q.level = lv;
	if (ioctl(fd, IBIOCPLAY, &q) < 0) {
		if (errno == ENXIO)
			printf("ikey: %s keys: not implemented on this board "
				"yet, Dallas only\n", ibk_name(type));
		else
			perror(errno == EBUSY ? "ikey: another program "
				"emulates a key" : "ikey: emulate");
		close(fd);
		return 1;
	}
	printf("Emulating ");
	print_key(ibk_name(type), key, ibk_size(type));
	printf("Hold the contacts to the reader; Ctrl-C stops.\n");
	fflush(stdout);
	while (secs <= 0 || n < 2 * secs) {
		pause_ms(500);
		n++;
		if (ioctl(fd, IBIOCPLAY, &q) < 0) break;	/* served */
		if (q.served != last) {
			printf("Read by a reader (%u)\n",
				(unsigned) (unsigned char) (q.served - last));
			fflush(stdout);
			last = q.served;
		}
	}
	q.n = 0;
	(void) ioctl(fd, IBIOCPLAY, &q);
	close(fd);
	return 0;
}

/* What a command names: a type and key by arguments (TYPE KEY), or a
 * .ibtn FILE.  Returns 0, or a status for exit(). */
static int key_arg(int argc, char **argv, int *type, const struct ibd_fam **f,
	unsigned char *key)
{
	struct kfile k;
	const char *name;

	*f = NULL;
	if (argc == 1) {
		if (kf_load(argv[0], &k, NULL) < 0) return 2;
		name = k.proto;
		if ((*type = ibk_type(name)) != 0) {
			if (k.ndata != ibk_size(*type)) {
				fprintf(stderr, "ikey: %s: no key data\n", argv[0]);
				return 2;
			}
			memcpy(key, k.data, ibk_size(*type));
			return 0;
		}
		if ((*f = ibd_byname(name)) == NULL || k.nrom != 8) {
			fprintf(stderr, "ikey: %s: %s is not a key type I know\n",
				argv[0], name);
			return 2;
		}
		memcpy(key, k.rom, 8);
		return 0;
	}
	if (argc != 2) usage();
	if ((*type = ibk_type(argv[0])) != 0) {
		parse_hex(argv[1], key, ibk_size(*type));
		return 0;
	}
	if ((*f = ibd_byname(argv[0])) == NULL) usage();
	parse_key(argv[1], key, "used");
	return 0;
}

int main(int argc, char **argv)
{
	unsigned char key[8];
	const struct ibd_fam *f;
	int type, r;

	if (argc >= 2 && strcmp(argv[1], "read") == 0) {
		int tries = 40, a = 2;		/* 10 s */
		const char *outfile = NULL;

		while (a < argc) {
			if (a + 1 < argc && strcmp(argv[a], "-n") == 0)
				tries = atoi(argv[a + 1]);
			else if (a + 1 < argc && strcmp(argv[a], "-o") == 0)
				outfile = argv[a + 1];
			else usage();
			a += 2;
		}
		return read_key(tries, outfile);
	}
	if (argc >= 3 && strcmp(argv[1], "write") == 0) {
		int a = 2, mask = B_RW1990 | B_TM2004;

		if (argc >= 5 && strcmp(argv[2], "-b") == 0) {
			mask = blank_mask(argv[3]);
			a = 4;
		}
		if ((r = key_arg(argc - a, argv + a, &type, &f, key)) != 0)
			return r;
		if (type != 0) return write_misc(type, key);
		return write_dallas(key, mask);
	}
	if (argc == 3 && strcmp(argv[1], "copy") == 0) return copy_file(argv[2]);
	if (argc >= 3 && strcmp(argv[1], "emulate") == 0) {
		int a = 2, secs = 0;

		if (argc >= 5 && strcmp(argv[2], "-t") == 0) {
			secs = atoi(argv[3]);
			a = 4;
		}
		if ((r = key_arg(argc - a, argv + a, &type, &f, key)) != 0)
			return r;
		if (type != 0) return emulate_misc(type, key, secs);
		return emulate_key(key, f, secs);
	}
	usage();
	return 2;
}
