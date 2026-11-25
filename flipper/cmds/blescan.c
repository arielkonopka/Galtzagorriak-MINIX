/* blescan - list the BLE devices around, and find a tracker (issue #388,
 * the user's request 2026-11-25; spec Phase 15).
 *
 *	blescan [-t] [-a] [FILE]	read the controller's HCI events (H4:
 *					0x04, code, length, parameters) from
 *					FILE (default: standard input) until
 *					it ends, then list the devices,
 *					strongest first
 *	blescan -F ADDR [FILE]		finder: a line with bars for every
 *					report of the device ADDR
 *					(AA:BB:CC:DD:EE:FF), the louder the
 *					closer; "hot and cold"
 *
 *	-t	only the ones that look like trackers (Find My, Tile,
 *		SmartTag, Find Hub); without -t they are listed first
 *	-a	every device (the default; undoes -t)
 *
 * The source is a stream of HCI events: a file or a pipe today (the
 * tests; an external controller on the UART later), /dev/ble when CPU2's
 * scan is wired up (T30b, lane:local).  Nothing talks to a radio here.
 * The kinds come from the advertisement alone: a name for what the packet
 * claims, not a proof.
 *
 * RAM: the table, 16 x 40 B, on the stack; no malloc, nothing resident.
 * Exit status: 0, 1 an error or no report at all, 2 usage.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "blescan.h"

static int rd(int fd, uint8_t *b, int n)
{
	int got = 0, r;

	while (got < n) {
		r = read(fd, b + got, n - got);
		if (r < 0) return -1;
		if (r == 0) return got;
		got += r;
	}
	return got;
}

static int hexv(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
		: c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* "AA:BB:CC:DD:EE:FF" into the on-air order (last byte first). */
static int parse_addr(const char *t, uint8_t a[6])
{
	int i, h, l;

	if (strlen(t) != 17) return -1;
	for (i = 0; i < 6; i++) {
		h = hexv(t[3 * i]);
		l = hexv(t[3 * i + 1]);
		if (h < 0 || l < 0 || (i < 5 && t[3 * i + 2] != ':')) return -1;
		a[5 - i] = (uint8_t) (h << 4 | l);
	}
	return 0;
}

static void put_addr(const uint8_t a[6])
{
	printf("%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

static void put_bars(int rssi)
{
	int b = bs_bars(rssi), i;

	putchar('[');
	for (i = 0; i < 8; i++) putchar(i < b ? '#' : ' ');
	putchar(']');
}

int main(int argc, char **argv)
{
	static struct bs s;
	uint8_t pkt[3 + 255], want[6], idx[BS_MAX];
	int fd = 0, i, n, only = 0, find = 0, reports = 0, nd = 0, before, found = 0;
	const char *file = NULL;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-t") == 0) only = 1;
		else if (strcmp(argv[i], "-a") == 0) only = 0;
		else if (strcmp(argv[i], "-F") == 0 && i + 1 < argc) {
			if (parse_addr(argv[++i], want) < 0) {
				fprintf(stderr, "blescan: bad address\n");
				return 2;
			}
			find = 1;
		} else if (argv[i][0] != '-' && file == NULL) file = argv[i];
		else {
			fprintf(stderr, "usage: blescan [-t] [-a] [-F ADDR] [FILE]\n");
			return 2;
		}
	}
	if (file != NULL && (fd = open(file, O_RDONLY)) < 0) {
		perror(file);
		return 1;
	}
	bs_init(&s);
	for (;;) {
		if ((n = rd(fd, pkt, 3)) < 3) break;	/* the end (or cut) */
		if (pkt[0] != 0x04) {			/* lost: look for 0x04 */
			memmove(pkt, pkt + 1, 2);
			if (rd(fd, pkt + 2, 1) < 1) break;
			continue;
		}
		if (pkt[2] > 0 && rd(fd, pkt + 3, pkt[2]) < pkt[2]) break;
		before = 0;
		if (find)
			for (i = 0; i < s.n; i++)
				if (memcmp(s.d[i].addr, want, 6) == 0)
					before = s.d[i].seen + 1;	/* 0: new */
		n = bs_hci(&s, pkt, 3 + pkt[2]);
		if (n <= 0) continue;
		reports += n;
		if (find)	/* a report of the wanted one in this packet */
			for (i = 0; i < s.n; i++)
				if (memcmp(s.d[i].addr, want, 6) == 0
				    && (before == 0 || s.d[i].seen + 1 != before
					|| s.d[i].seen == 255)) {
					put_bars(s.d[i].last);
					printf(" %d dBm (avg %d)\n", s.d[i].last,
					       s.d[i].rssi);
					fflush(stdout);
					found = 1;
				}
	}
	if (fd != 0) close(fd);
	if (find) return found ? 0 : 1;
	nd = bs_order(&s, idx, 1);
	printf("ADDRESS            RSSI BARS       KIND       NAME\n");
	for (i = 0; i < nd; i++) {
		const struct bs_dev *d = &s.d[idx[i]];

		if (only && (d->kind == BS_K_NONE || d->kind == BS_K_EDDY
			     || d->kind == BS_K_IBEACON
			     || d->kind == BS_K_APPLE)) continue;
		put_addr(d->addr);
		printf(" %4d ", d->rssi);
		put_bars(d->rssi);
		printf(" %-10s %s\n", bs_kind_name(d->kind), d->name);
	}
	return reports > 0 ? 0 : 1;
}
