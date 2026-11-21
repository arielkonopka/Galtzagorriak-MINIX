/* nfcuid - bring-up test of /dev/nfc (#390): read an NFC-A card's UID by raw
 * NFCIOCXFER (REQA, anticollision, SELECT), the field cycled between rounds.
 * Not built by the Makefile yet: for the device, copy it over apps/hello.c
 * and send the hello package.  Usage: nfcuid [ROUNDS] */
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

static int fd;

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

int main(int argc, char **argv)
{
	unsigned char tx[9], rx[16], uid[10];
	unsigned rxb, n = 0;
	int v, r, cl, i, rounds = argc > 1 ? atoi(argv[1]) : 1;

	if ((fd = open("/dev/nfc", O_RDWR)) < 0) { printf("open: errno %d\n", errno); return 1; }
	v = 0;
	r = ioctl(fd, NFCIOCFEAT, &v);
	printf("feat r=%d 0x%x\n", r, v);
	v = NFC_TECH_A;
	if (ioctl(fd, NFCIOCFIELD, &v) < 0) { printf("field: errno %d\n", errno); return 1; }
	while (rounds-- > 0) {
		v = 0; ioctl(fd, NFCIOCFIELD, &v);	/* the field off and on: the card starts again */
		poll((struct pollfd *) 0, 0, 30);
		v = NFC_TECH_A;
		if (ioctl(fd, NFCIOCFIELD, &v) < 0) { printf("field: errno %d\n", errno); return 1; }
		n = 0;
		tx[0] = 0x26;
		r = xf(tx, 7, rx, 16, NFC_XF_SHORT, &rxb);
		if (r < 0) { printf("reqa: errno %d\n", -r); continue; }
		printf("atqa %02x %02x (%u bits)", rx[0], rx[1], rxb);
		for (cl = 0x93; cl <= 0x97; cl += 2) {
			tx[0] = cl; tx[1] = 0x20;
			if ((r = xf(tx, 16, rx, 40, 0, &rxb)) < 0) { printf(" anticol %02x: errno %d", cl, -r); break; }
			if (rxb != 40 || (rx[0] ^ rx[1] ^ rx[2] ^ rx[3]) != rx[4]) { printf(" anticol %02x: %u bits, bcc bad", cl, rxb); break; }
			tx[1] = 0x70;
			memcpy(tx + 2, rx, 5);
			memcpy(uid + n, rx, 4);
			if ((r = xf(tx, 56, rx, 8, NFC_XF_CRC_TX | NFC_XF_CRC_RX, &rxb)) < 0) { printf(" select %02x: errno %d", cl, -r); break; }
			if (rx[0] & 0x04) {		/* cascade: uid[0] was 0x88 */
				memmove(uid + n, uid + n + 1, 3);
				n += 3;
				continue;
			}
			n += 4;
			printf(" sak %02x uid", rx[0]);
			for (i = 0; i < (int) n; i++) printf(" %02x", uid[i]);
			break;
		}
		printf("\n");
	}
	v = 0;
	ioctl(fd, NFCIOCFIELD, &v);
	close(fd);
	return 0;
}
