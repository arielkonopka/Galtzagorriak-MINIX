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
 *		ikey emulate TYPE KEY	not yet (R6.7: slave emulation)
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

static void pause_ms(int ms)
{
	(void) poll((struct pollfd *) NULL, 0, ms);
}

static void usage(void)
{
	fprintf(stderr, "usage: ikey read [-n TRIES]\n"
			"       ikey write Dallas KEY\n"
			"       ikey emulate Dallas|Cyfral|Metakom KEY\n");
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

static int read_key(int tries)
{
	unsigned char rom[8];
	int fd, n, i;

	printf("Reading iButton...\nPress Ctrl+C to abort\n");
	fflush(stdout);
	while (tries-- > 0) {
		if ((fd = open("/dev/ibutton", O_RDONLY)) < 0) {
			perror("ikey: /dev/ibutton");
			return 1;
		}
		n = read(fd, (char *) rom, 8);
		close(fd);
		if (n == 8) {
			printf("Dallas ");
			for (i = 0; i < 8; i++) printf("%02X", rom[i]);
			printf("\n");
			return 0;
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

static int write_key(const char *hex)
{
	unsigned char key[8];
	char two[3];
	int fd, i, r, v;

	if (strlen(hex) != 16) usage();
	for (i = 0; i < 8; i++) {
		char *e;

		two[0] = hex[2 * i]; two[1] = hex[2 * i + 1]; two[2] = 0;
		key[i] = (unsigned char) strtoul(two, &e, 16);
		if (*e != 0) usage();
	}
	if (!crc_ok(key))
		fprintf(stderr, "ikey: note: the last byte is not the CRC of the "
			"first seven (written as given)\n");
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
		fprintf(stderr, "ikey emulate: not supported yet (spec R6.7)\n");
		return 2;
	}
	usage();
	return 2;
}
