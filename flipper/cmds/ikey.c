/* ikey - iButton keys (Galtzagorriak MINIX, spec R6.7), as the stock CLI's
 * "ikey" command
 *
 * Usage:	ikey read [-n TRIES]	wait for a key on the contacts and
 *					print it as stock does: "Dallas"
 *					and the 8 ROM bytes in hex
 *		ikey write Dallas KEY	write KEY (16 hex digits, the ROM
 *					with its CRC) to an RW1990 blank
 *					held on the contacts, then read it
 *					back
 *		ikey emulate [-t SECS] Dallas|Cyfral|Metakom KEY
 *					be the key for a reader on the
 *					contacts until Ctrl-C (or SECS),
 *					saying each time a reader got it
 *
 * KEY is hex as `ikey read` and the stock CLI print it: Dallas 16 digits,
 * Cyfral 4 (e.g. A1B2), Metakom 8 (every byte of even parity).
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
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ibkey.h"

#define CAPT	256			/* durations: 2-3 frames of either */
static unsigned short capt[CAPT];

static void pause_ms(int ms)
{
	(void) poll((struct pollfd *) NULL, 0, ms);
}

static void usage(void)
{
	fprintf(stderr, "usage: ikey read [-n TRIES]\n"
			"       ikey write Dallas KEY\n"
			"       ikey emulate [-t SECS] Dallas|Cyfral|Metakom KEY\n");
	exit(2);
}

static int crc_ok(const unsigned char *rom)
{
	unsigned char crc = 0;
	int i, j;

	for (i = 0; i < 7; i++) {
		crc ^= rom[i];
		for (j = 0; j < 8; j++) crc = (crc & 1) ? (crc >> 1) ^ 0x8C : crc >> 1;
	}
	return crc == rom[7];
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
static int read_misc(int fd)
{
	struct ib_capt q;
	unsigned char key[IBK_MAXKEY];
	int t;

	q.dur = capt;
	q.n = CAPT;
	q.ms = 60;			/* 256 durations: ~32 ms of either */
	if (ioctl(fd, IBIOCCAPT, &q) < 0) return errno == ENXIO ? -1 : 0;
	if ((t = ibk_decode(capt, (int) q.got, q.level, key)) == 0) return 0;
	print_key(ibk_name(t), key, ibk_size(t));
	return 1;
}

static int read_key(int tries)
{
	unsigned char rom[8];
	int fd, n, misc = 1;

	printf("Reading iButton...\nPress Ctrl+C to abort\n");
	fflush(stdout);
	while (tries-- > 0) {
		if ((fd = open("/dev/ibutton", O_RDONLY)) < 0) {
			perror("ikey: /dev/ibutton");
			return 1;
		}
		n = read(fd, (char *) rom, 8);
		if (n == 8) {
			close(fd);
			print_key("Dallas", rom, 8);
			return 0;
		}
		if (misc && (n = read_misc(fd)) != 0) {	/* Cyfral, Metakom */
			close(fd);
			if (n > 0) return 0;
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

/* KEY: 16 hex digits, the ROM with its CRC, as `ikey read` prints it
 * without the spaces. */
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

static int write_key(const char *hex)
{
	unsigned char key[8];
	int fd, r, v;

	parse_key(hex, key, "written");
	if ((fd = open("/dev/ibutton", O_RDWR)) < 0) {
		perror("ikey: /dev/ibutton");
		return 1;
	}
	printf("Writing key...\n");
	fflush(stdout);
	for (v = 1; v <= 2; v++) {
		if ((r = rw1990(fd, key, v)) < 0) break;
		if (r == 1) {
			printf("Write success (RW1990.%d)\n", v);
			close(fd);
			return 0;
		}
	}
	close(fd);
	printf("Write failed: no RW1990 blank on the contacts, or it did not "
		"take the key\n");
	return 1;
}

/* Be the key until killed, or for secs (> 0).  The kernel answers the
 * reader; every half second we ask how many readers got the whole key. */
static int emulate_key(const char *hex, int secs)
{
	struct ib_emu q;
	unsigned char last = 0;
	int fd, i, n = 0;

	parse_key(hex, q.rom, "sent");
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
static int emulate_misc(int type, const char *hex, int secs)
{
	static unsigned short dur[IBK_MAXFRAME];
	struct ib_play q;
	unsigned char key[IBK_MAXKEY], last = 0;
	int fd, lv, nd, n = 0;

	parse_hex(hex, key, ibk_size(type));
	if (ibk_valid(type, key) != 1) {
		fprintf(stderr, "ikey: Metakom %s: every byte needs even parity "
			"(a reader would not take it)\n", hex);
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

int main(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "read") == 0) {
		int tries = 40;			/* 10 s */

		if (argc == 4 && strcmp(argv[2], "-n") == 0) tries = atoi(argv[3]);
		else if (argc != 2) usage();
		return read_key(tries);
	}
	if (argc == 4 && strcmp(argv[1], "write") == 0) {
		if (strcmp(argv[2], "Dallas") != 0) {
			fprintf(stderr, "ikey: write: only Dallas keys (RW1990 "
				"blanks)\n");
			return 2;
		}
		return write_key(argv[3]);
	}
	if (argc >= 2 && strcmp(argv[1], "emulate") == 0) {
		int a = 2, secs = 0;

		if (argc >= 4 && strcmp(argv[2], "-t") == 0) {
			secs = atoi(argv[3]);
			a = 4;
		}
		if (argc != a + 2) usage();
		if (ibk_type(argv[a]) != 0)
			return emulate_misc(ibk_type(argv[a]), argv[a + 1], secs);
		if (strcmp(argv[a], "Dallas") != 0) usage();
		return emulate_key(argv[a + 1], secs);
	}
	usage();
	return 2;
}
