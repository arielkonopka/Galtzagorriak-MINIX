/* nfcmfc - device test of NFC_XF_PARITY on /dev/nfc (#390): a MIFARE Classic
 * authentication and one block READ by raw NFCIOCXFER.  The AUTH command and
 * the card's nonce go as plain frames; everything after is Crypto-1 with
 * encrypted parity bits, 9 bits a byte.  Read only: it sends AUTH and READ,
 * nothing else.  Not built by the Makefile: for the device, copy it over
 * apps/hello.c and send the hello package.
 * With -w DATA32HEX it WRITEs the block first (never block 0 or a sector
 * trailer), then reads it back.
 * Usage: nfcmfc [-q] [-w DATA32HEX] [BLOCK [KEY12HEX [a|b]]]
 *	(-q: no UID, no block data) */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <minix/nfc.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../src/lib/libnfc/crypto1.c"

static int fd;
static crypto1_t c;

static int xf(unsigned char *tx, unsigned txb, unsigned char *rx,
	unsigned room, unsigned flags, unsigned *rxb)
{
	struct nfc_xfer_io io;

	io.tx = tx; io.rx = rx; io.tx_bits = txb; io.rx_bits = room;
	io.fwt = 0; io.flags = flags;
	if (ioctl(fd, NFCIOCXFER, &io) < 0) return -errno;
	*rxb = io.rx_bits;
	return 0;
}

static unsigned odd(unsigned b)
{
	b ^= b >> 4; b ^= b >> 2; b ^= b >> 1;
	return ~b & 1;
}

static unsigned crc_a(const unsigned char *p, int n)
{
	unsigned v = 0x6363, b;

	while (n-- > 0) {
		b = (*p++ ^ v) & 0xFF;
		b = (b ^ b << 4) & 0xFF;
		v = ((v >> 8) ^ (b << 8) ^ (b << 3) ^ (b >> 4)) & 0xFFFF;
	}
	return v;
}

/* Put byte v and parity bit p at bit position *pos of the 9-bit stream. */
static void put9(unsigned char *s, unsigned *pos, unsigned v, unsigned p)
{
	unsigned i, w = v | p << 8;

	for (i = 0; i < 9; i++, (*pos)++)
		if (w >> i & 1) s[*pos / 8] |= 1 << (*pos % 8);
}

/* A 4-bit ACK/NAK, deciphered (0xA: ACK). */
static int nibble(const unsigned char *rx, unsigned rxb)
{
	unsigned i, v = 0;

	if (rxb != 4) return -1;
	for (i = 0; i < 4; i++)
		v |= (unsigned) ((rx[0] >> i & 1) ^ crypto1_bit(&c, 0, 0)) << i;
	return (int) v;
}

static unsigned get9(const unsigned char *s, unsigned k)
{
	unsigned i, w = 0, pos = k * 9;

	for (i = 0; i < 9; i++, pos++)
		w |= (unsigned) (s[pos / 8] >> (pos % 8) & 1) << i;
	return w;
}

/* Plain bytes out, ciphered with their parity. */
static void seal(unsigned char *s, unsigned *pos, const unsigned char *p, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		unsigned ks = crypto1_byte(&c, 0, 0);

		put9(s, pos, p[i] ^ ks, odd(p[i]) ^ crypto1_filter(c.odd));
	}
}

int main(int argc, char **argv)
{
	unsigned char tx[24], rx[24], uid[10], pl[18], key[6];
	unsigned rxb, n = 0, pos, w, parbad = 0, blk = 0, kt = 0x60;
	unsigned long nt, at, u;
	unsigned long long k = 0;
	int v, r, cl, i, quiet = 0;
	const char *wr = (char *) 0;

	if (argc > 1 && !strcmp(argv[1], "-q")) { quiet = 1; argc--; argv++; }
	if (argc > 2 && !strcmp(argv[1], "-w")) { wr = argv[2]; argc -= 2; argv += 2; }
	if (argc > 1) blk = atoi(argv[1]);
	if (wr != NULL && (strlen(wr) != 32 || blk == 0 || blk % 4 == 3)) {
		printf("write: 32 hex digits, not block 0, not a trailer\n");
		return 2;
	}
	memset(key, 0xFF, 6);
	if (argc > 2 && strlen(argv[2]) == 12)
		for (i = 0; i < 6; i++) {
			char h[3];

			h[0] = argv[2][2 * i]; h[1] = argv[2][2 * i + 1]; h[2] = 0;
			key[i] = (unsigned char) strtol(h, (char **) 0, 16);
		}
	if (argc > 3 && argv[3][0] == 'b') kt = 0x61;
	for (i = 0; i < 6; i++) k = k << 8 | key[i];
	if ((fd = open("/dev/nfc", O_RDWR)) < 0) { printf("open: errno %d\n", errno); return 1; }
	v = NFC_TECH_A;
	if (ioctl(fd, NFCIOCFIELD, &v) < 0) { printf("field: errno %d\n", errno); return 1; }
	poll((struct pollfd *) 0, 0, 20);
	tx[0] = 0x52;
	if ((r = xf(tx, 7, rx, 16, NFC_XF_SHORT, &rxb)) < 0) { printf("wupa: errno %d\n", -r); goto out; }
	for (cl = 0x93; cl <= 0x97; cl += 2) {
		tx[0] = cl; tx[1] = 0x20;
		if ((r = xf(tx, 16, rx, 40, 0, &rxb)) < 0 || rxb != 40) { printf("anticol: %d\n", r); goto out; }
		tx[1] = 0x70;
		memcpy(tx + 2, rx, 5);
		memcpy(uid + n, rx, 4);
		if ((r = xf(tx, 56, rx, 8, NFC_XF_CRC_TX | NFC_XF_CRC_RX, &rxb)) < 0) { printf("select: errno %d\n", -r); goto out; }
		if (!(rx[0] & 0x04)) { n += 4; break; }
		memmove(uid + n, uid + n + 1, 3);
		n += 3;
	}
	printf("sak %02x, uid %u bytes", rx[0], n);
	if (!quiet) for (i = 0; i < (int) n; i++) printf(" %02x", uid[i]);
	printf("\n");
	u = (unsigned long) uid[n - 4] << 24 | (unsigned long) uid[n - 3] << 16
		| (unsigned long) uid[n - 2] << 8 | uid[n - 1];

	tx[0] = kt; tx[1] = blk;			/* AUTH, plain */
	if ((r = xf(tx, 16, rx, 64, NFC_XF_CRC_TX, &rxb)) < 0) { printf("auth: errno %d\n", -r); goto out; }
	if (rxb != 32) { printf("auth: nonce of %u bits\n", rxb); goto out; }
	nt = (unsigned long) rx[0] << 24 | (unsigned long) rx[1] << 16
		| (unsigned long) rx[2] << 8 | rx[3];
	crypto1_init(&c, k);
	(void) crypto1_word(&c, u ^ nt, 0);
	memset(tx, 0, sizeof tx);
	pos = 0;
	for (i = 0; i < 4; i++) {			/* {nr}: fed into the cipher */
		unsigned nr = 0x10 + i, ks = crypto1_byte(&c, nr, 0);

		put9(tx, &pos, nr ^ ks, odd(nr) ^ crypto1_filter(c.odd));
	}
	at = crypto1_prng_successor(nt, 64);
	pl[0] = at >> 24; pl[1] = at >> 16; pl[2] = at >> 8; pl[3] = at;
	seal(tx, &pos, pl, 4);				/* {ar} */
	if ((r = xf(tx, pos, rx, sizeof rx * 8, NFC_XF_PARITY, &rxb)) < 0) {
		printf("auth 2: errno %d (%s)\n", -r, -r == ETIMEDOUT ? "a wrong key gives silence" : "?");
		goto out;
	}
	if (rxb != 36) { printf("auth 2: %u bits\n", rxb); goto out; }
	at = 0;
	for (i = 0; i < 4; i++) {
		w = get9(rx, i);
		at = at << 8 | ((w ^ crypto1_byte(&c, 0, 0)) & 0xFF);
		if ((w >> 8 ^ crypto1_filter(c.odd)) != odd(at & 0xFF)) parbad++;
	}
	if (at != crypto1_prng_successor(nt, 96)) { printf("auth: the card's answer is wrong\n"); goto out; }
	printf("auth ok (block %u, key %c), parity errors %u\n", blk, kt == 0x60 ? 'A' : 'B', parbad);

	if (wr != NULL) {				/* WRITE, two steps */
		pl[0] = 0xA0; pl[1] = blk;
		w = crc_a(pl, 2);
		pl[2] = w; pl[3] = w >> 8;
		memset(tx, 0, sizeof tx);
		pos = 0;
		seal(tx, &pos, pl, 4);
		if ((r = xf(tx, pos, rx, sizeof rx * 8, NFC_XF_PARITY, &rxb)) < 0) { printf("write: errno %d\n", -r); goto out; }
		if ((r = nibble(rx, rxb)) != 0xA) { printf("write: answer %d (%u bits)\n", r, rxb); goto out; }
		for (i = 0; i < 16; i++) {
			char h[3];

			h[0] = wr[2 * i]; h[1] = wr[2 * i + 1]; h[2] = 0;
			pl[i] = (unsigned char) strtol(h, (char **) 0, 16);
		}
		w = crc_a(pl, 16);
		pl[16] = w; pl[17] = w >> 8;
		memset(tx, 0, sizeof tx);
		pos = 0;
		seal(tx, &pos, pl, 18);
		if ((r = xf(tx, pos, rx, sizeof rx * 8, NFC_XF_PARITY, &rxb)) < 0) { printf("write data: errno %d\n", -r); goto out; }
		if ((r = nibble(rx, rxb)) != 0xA) { printf("write data: answer %d (%u bits)\n", r, rxb); goto out; }
		printf("write ok (block %u)\n", blk);
	}
	pl[0] = 0x30; pl[1] = blk;			/* READ */
	w = crc_a(pl, 2);
	pl[2] = w; pl[3] = w >> 8;
	memset(tx, 0, sizeof tx);
	pos = 0;
	seal(tx, &pos, pl, 4);
	if ((r = xf(tx, pos, rx, sizeof rx * 8, NFC_XF_PARITY, &rxb)) < 0) { printf("read: errno %d\n", -r); goto out; }
	if (rxb != 18 * 9) { printf("read: %u bits\n", rxb); goto out; }
	parbad = 0;
	for (i = 0; i < 18; i++) {
		w = get9(rx, i);
		pl[i] = w ^ crypto1_byte(&c, 0, 0);
		if ((w >> 8 ^ crypto1_filter(c.odd)) != odd(pl[i])) parbad++;
	}
	w = crc_a(pl, 16);
	printf("read ok: 16 bytes, crc %s, parity errors %u\n",
		(pl[16] | pl[17] << 8) == (int) w ? "good" : "BAD", parbad);
	if (!quiet) {
		for (i = 0; i < 16; i++) printf("%02x ", pl[i]);
		printf("\n");
	}
out:
	v = 0;
	ioctl(fd, NFCIOCFIELD, &v);
	close(fd);
	return 0;
}
