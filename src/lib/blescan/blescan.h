/* blescan - the logic of a BLE scanner and tracker finder (issue #388,
 * the user's request 2026-11-25): advertising reports in, a table of the
 * devices around out.  Pure C: no I/O, no malloc, no controller; the
 * source of the reports (CPU2 through ACI, an external HCI controller, a
 * file in a test) is the caller's.  It runs on the PC (tests/blescan) and
 * in the `blescan` command (flipper/cmds/blescan.c).
 *
 * RAM: struct bs is 16 x 40 B = 640 B, kept by the caller (the command's
 * stack); nothing static.
 *
 * The tracker kinds are heuristics from the advertisement alone (company
 * id, service UUIDs); they name what the packet says, they prove nothing.
 */
#ifndef BLESCAN_H
#define BLESCAN_H

#include <stdint.h>

#define BS_MAX		16
#define BS_NAME		17

#define BS_K_NONE	0
#define BS_K_FINDMY	1	/* Apple Find My network (AirTag and others) */
#define BS_K_TILE	2	/* service 0xFEED / 0xFEEC */
#define BS_K_SMARTTAG	3	/* Samsung SmartTag, service 0xFD5A */
#define BS_K_FMDN	4	/* Google Find My Device network (0xFEAA, 0x40/0x41) */
#define BS_K_EDDY	5	/* other Eddystone beacons */
#define BS_K_IBEACON	6	/* Apple iBeacon */
#define BS_K_APPLE	7	/* other Apple continuity packets */
#define BS_K_N		8

struct bs_dev {
	uint8_t addr[6];	/* as on the air: addr[5] is printed first */
	uint8_t atype;		/* 0 public, 1 random */
	int8_t rssi;		/* dBm, smoothed (EMA over 4) */
	int8_t last;		/* dBm, the last report */
	uint8_t kind;		/* BS_K_* */
	uint8_t seen;		/* reports so far (stops at 255) */
	uint8_t flags;		/* AD type 0x01, 0 if none */
	uint16_t company;	/* manufacturer data company id, 0 if none */
	char name[BS_NAME];	/* complete or shortened local name */
};

struct bs {
	struct bs_dev d[BS_MAX];
	uint8_t n;
};

void bs_init(struct bs *s);

/* One advertising report (address, address type, the AD structures, the
 * RSSI in dBm).  The device is added or updated; 0 if it was taken, -1 if
 * the data are malformed (an AD structure that runs past the end: the
 * fields before it are still used).  A full table drops its weakest
 * device for a stronger one. */
int bs_report(struct bs *s, const uint8_t addr[6], int atype,
	      const uint8_t *data, int len, int rssi);

/* An HCI event packet (H4: 0x04, code, length, parameters): the LE
 * Advertising Report (subevent 0x02) is fed to bs_report().  Returns the
 * number of reports taken (0 for any other event: the stream goes on), or
 * -1 for a packet that is cut short or whose lengths do not fit. */
int bs_hci(struct bs *s, const uint8_t *pkt, int len);

/* The indexes of the devices, strongest first (kind != 0 first when
 * trackers_first).  Returns s->n. */
int bs_order(const struct bs *s, uint8_t idx[BS_MAX], int trackers_first);

const char *bs_kind_name(int kind);

/* 0..8: how close, from the RSSI (-100 dBm: 0, -40 dBm or more: 8). */
int bs_bars(int rssi);

#endif
