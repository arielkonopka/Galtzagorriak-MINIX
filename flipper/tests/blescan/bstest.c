/* Host test of libblescan: advertisement parsing, the table, HCI frames. */
#include <stdio.h>
#include <string.h>
#include "blescan.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); \
	fails++; } else printf("  ok   %s\n", what); } while (0)

/* One legacy advertising report in an H4 event, in the spec's layout (one
 * report: the fields are in turn anyway). */
static int frame(uint8_t *o, const uint8_t addr[6], int atype,
		 const uint8_t *ad, int al, int rssi)
{
	int n = 0;

	o[n++] = 0x04; o[n++] = 0x3E; o[n++] = 0;	/* length below */
	o[n++] = 0x02; o[n++] = 1;
	o[n++] = 0;					/* ADV_IND */
	o[n++] = (uint8_t) atype;
	memcpy(o + n, addr, 6); n += 6;
	o[n++] = (uint8_t) al;
	memcpy(o + n, ad, al); n += al;
	o[n++] = (uint8_t) rssi;
	o[2] = (uint8_t) (n - 3);
	return n;
}

int main(void)
{
	static struct bs s;
	uint8_t a1[6] = { 1, 2, 3, 4, 5, 6 }, a2[6] = { 9, 8, 7, 6, 5, 4 };
	uint8_t a3[6] = { 0xAA, 0, 0, 0, 0, 1 };
	/* AirTag-like: flags, manufacturer data 4C 00 12 19 ... */
	uint8_t findmy[] = { 2, 0x01, 0x06, 6, 0xFF, 0x4C, 0x00, 0x12, 0x19, 0x00 };
	/* Tile: complete 16-bit UUIDs FEED */
	uint8_t tile[] = { 3, 0x03, 0xED, 0xFE, 5, 0x09, 'T', 'i', 'l', 'e' };
	/* Samsung SmartTag: service data FD5A */
	uint8_t smart[] = { 5, 0x16, 0x5A, 0xFD, 0x01, 0x02 };
	/* Find Hub: Eddystone service data, frame 0x40 */
	uint8_t fmdn[] = { 5, 0x16, 0xAA, 0xFE, 0x40, 0x00 };
	uint8_t ibeacon[] = { 8, 0xFF, 0x4C, 0x00, 0x02, 0x15, 0, 0, 0 };
	uint8_t bad[] = { 2, 0x01, 0x06, 9, 0x09, 'x' };	/* runs past the end */
	uint8_t pkt[300], idx[BS_MAX];
	uint8_t scan_rsp[] = { 6, 0x09, 'M', 'y', 'T', 'a', 'g' };
	int i;

	bs_init(&s);
	CHECK(bs_report(&s, a1, 1, findmy, sizeof findmy, -70) == 0
		&& s.n == 1 && s.d[0].kind == BS_K_FINDMY
		&& s.d[0].company == 0x004C && s.d[0].flags == 6
		&& s.d[0].rssi == -70, "Apple 0x12: Find My");
	CHECK(bs_report(&s, a2, 0, tile, sizeof tile, -50) == 0
		&& s.d[1].kind == BS_K_TILE && strcmp(s.d[1].name, "Tile") == 0,
		"service FEED: Tile, with its name");
	bs_init(&s);
	bs_report(&s, a1, 0, smart, sizeof smart, -60);
	CHECK(s.d[0].kind == BS_K_SMARTTAG, "service data FD5A: SmartTag");
	bs_init(&s);
	bs_report(&s, a1, 0, fmdn, sizeof fmdn, -60);
	CHECK(s.d[0].kind == BS_K_FMDN, "Eddystone frame 0x40: Find Hub");
	bs_init(&s);
	bs_report(&s, a1, 0, ibeacon, sizeof ibeacon, -60);
	CHECK(s.d[0].kind == BS_K_IBEACON, "Apple 0x02 0x15: iBeacon");
	bs_init(&s);
	CHECK(bs_report(&s, a1, 0, bad, sizeof bad, -60) == -1 && s.n == 1
		&& s.d[0].flags == 6, "a structure past the end: -1, the part before is kept");
	CHECK(bs_report(&s, a1, 0, bad, 0, 127) == -1, "RSSI 127 (not available) is refused");

	/* smoothing, and a scan response adds the name */
	bs_init(&s);
	bs_report(&s, a1, 1, findmy, sizeof findmy, -80);
	bs_report(&s, a1, 1, scan_rsp, sizeof scan_rsp, -40);
	CHECK(s.n == 1 && s.d[0].seen == 2 && s.d[0].last == -40
		&& s.d[0].rssi == -70, "RSSI averaged over 4: -80, -40 gives -70");
	CHECK(s.d[0].kind == BS_K_FINDMY && strcmp(s.d[0].name, "MyTag") == 0,
		"a scan response keeps the kind and adds the name");
	bs_report(&s, a1, 0, scan_rsp, sizeof scan_rsp, -40);
	CHECK(s.n == 2, "the same address with the other type is another device");

	/* a full table drops the weakest for a stronger one */
	bs_init(&s);
	for (i = 0; i < BS_MAX; i++) {
		a3[0] = (uint8_t) (0x10 + i);
		bs_report(&s, a3, 0, NULL, 0, -90 + i);
	}
	a3[0] = 0xEE;
	CHECK(s.n == BS_MAX, "the table holds 16");
	bs_report(&s, a3, 0, NULL, 0, -95);
	CHECK(s.n == BS_MAX && memcmp(s.d[0].addr, a3, 6) != 0,
		"a weaker newcomer is not taken");
	bs_report(&s, a3, 0, NULL, 0, -50);
	CHECK(memcmp(s.d[0].addr, a3, 6) == 0 && s.d[0].rssi == -50,
		"a stronger one replaces the weakest (-90)");

	/* order: trackers first, then strongest */
	bs_init(&s);
	bs_report(&s, a1, 0, NULL, 0, -30);
	bs_report(&s, a2, 0, findmy, sizeof findmy, -80);
	bs_report(&s, a3, 0, tile, sizeof tile, -60);
	bs_order(&s, idx, 1);
	CHECK(idx[0] == 2 && idx[1] == 1 && idx[2] == 0,
		"order: trackers first (strongest first), then the rest");
	bs_order(&s, idx, 0);
	CHECK(idx[0] == 0 && idx[1] == 2 && idx[2] == 1, "order: strongest first");

	CHECK(bs_bars(-100) == 0 && bs_bars(-40) == 8 && bs_bars(-30) == 8
		&& bs_bars(-120) == 0 && bs_bars(-70) == 4, "bars from the RSSI");
	CHECK(strcmp(bs_kind_name(BS_K_TILE), "Tile") == 0
		&& bs_kind_name(99)[0] == 0, "kind names");

	/* HCI frames */
	bs_init(&s);
	i = frame(pkt, a1, 1, findmy, sizeof findmy, (uint8_t) -65);
	CHECK(bs_hci(&s, pkt, i) == 1 && s.n == 1 && s.d[0].rssi == -65
		&& memcmp(s.d[0].addr, a1, 6) == 0 && s.d[0].atype == 1
		&& s.d[0].kind == BS_K_FINDMY, "an LE advertising report");
	CHECK(bs_hci(&s, pkt, i - 1) == -1, "a cut-short packet: -1");
	pkt[2]++;
	CHECK(bs_hci(&s, pkt, i) == -1, "a length that does not fit: -1");
	pkt[2]--;
	pkt[1] = 0x05;				/* disconnection complete */
	CHECK(bs_hci(&s, pkt, i) == 0, "another event: 0, the stream goes on");
	pkt[1] = 0x3E; pkt[3] = 0x01;		/* connection complete */
	CHECK(bs_hci(&s, pkt, i) == 0, "another LE subevent: 0");
	pkt[3] = 0x02;
	pkt[0] = 0x02;
	CHECK(bs_hci(&s, pkt, i) == -1, "not an H4 event: -1");
	pkt[0] = 0x04;
	pkt[4] = 0;
	CHECK(bs_hci(&s, pkt, i) == -1, "no reports: -1");
	pkt[4] = 1;
	pkt[5 + 8] = (uint8_t) (pkt[5 + 8] + 1);	/* data length too big */
	CHECK(bs_hci(&s, pkt, i) == -1, "a data length that does not fit: -1");

	/* two reports in the spec's grouped layout */
	{
		uint8_t m[40];
		int k = 0;

		m[k++] = 0x04; m[k++] = 0x3E; m[k++] = 0; m[k++] = 0x02; m[k++] = 2;
		m[k++] = 0; m[k++] = 0;			/* event types */
		m[k++] = 0; m[k++] = 1;			/* address types */
		memcpy(m + k, a1, 6); k += 6;
		memcpy(m + k, a2, 6); k += 6;
		m[k++] = 3; m[k++] = 0;			/* data lengths */
		m[k++] = 2; m[k++] = 0x01; m[k++] = 0x06;	/* report 1's data */
		m[k++] = (uint8_t) -55; m[k++] = (uint8_t) -75;	/* RSSIs */
		m[2] = (uint8_t) (k - 3);
		bs_init(&s);
		CHECK(bs_hci(&s, m, k) == 2 && s.n == 2 && s.d[0].rssi == -55
			&& s.d[1].rssi == -75 && s.d[1].atype == 1
			&& s.d[0].flags == 6, "two reports, fields in turn (the spec's layout)");
	}
	printf("blescan: %s\n", fails ? "FAIL" : "PASS");
	return fails != 0;
}
