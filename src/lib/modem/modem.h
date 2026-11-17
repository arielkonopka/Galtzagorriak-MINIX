/* modem.h - libmodem: data links between two Flippers over RFID, NFC,
 * Sub-GHz and BLE (spec section P).
 *
 * One frame format for every medium (P.3); the PHY backends add only their
 * own sync.  Three services: an ARQ byte stream (selective repeat, half
 * duplex, window <= 8), a carousel for simplex media (RFID today) and
 * datagrams.  Plus the pieces of the two backends that are pure logic and
 * are tested on the PC (make modemtest): the RFID line code P4 (P.5), the
 * CC1101 register calculator, the presets, the region's band table and the
 * duty-cycle counter (P.4).
 *
 * Plain C89 (long long in cc1101.c), no malloc: the caller passes every
 * buffer, so the same code serves the `fmodem` command, the PPP link of
 * Phase 19 and Berry.  Every byte from the air is hostile (P.3): only
 * mdm_unpack() reads a frame, and it checks the length against the buffer and the MTU before the CRC.
 */
#ifndef _MODEM_H
#define _MODEM_H

/* ---- frames (P.3) ---- */
#define MDM_HDR		5		/* type|flags, sid, a, b, len */
#define MDM_CRC		2
#define MDM_OVH		(MDM_HDR + MDM_CRC)
#define MDM_MAXPAY	250		/* the largest MTU of any medium */
#define MDM_MAXFRAME	(MDM_MAXPAY + MDM_OVH)

#define MDM_T_DATA	1		/* a: seq */
#define MDM_T_ACK	2		/* a: next expected, b: bitmap */
#define MDM_T_CAR	3		/* a, b: index (low, high) */
#define MDM_T_DGRAM	4

#define MDM_F_TURN	1		/* the other side may send now */
#define MDM_F_LAST	2		/* the last frame of the stream */

#define MDM_WMAX	8		/* the ACK bitmap's bits */

struct mdm_frame {
	unsigned char type, flags, sid, a, b, len;
	const unsigned char *pay;	/* len bytes (in the packed buffer) */
};

unsigned short mdm_crc16(const unsigned char *p, int n, unsigned short crc);
/* Pack f into out[max]: the frame's length, or -1 if it does not fit. */
int mdm_pack(const struct mdm_frame *f, unsigned char *out, int max);
/* Check and split in[0..n-1] (one frame, nothing after it): 0 with f set
 * (f->pay points into in), -1 if the length, the type or the CRC is
 * wrong or len > mtu. */
int mdm_unpack(const unsigned char *in, int n, int mtu, struct mdm_frame *f);

/* ---- a medium ---- */
struct mdm_phy {
	void *ctx;
	int mtu;			/* payload bytes per frame */
	long turn_ms;			/* receive<->transmit turnaround with the
				 * scheduler's slack (>= 25 ms on MINIX) */
	/* one frame; 0 or an error (< 0) */
	int (*send)(void *ctx, const unsigned char *buf, int n);
	/* one frame into buf[max]: its length, 0 on timeout, < 0 error */
	int (*recv)(void *ctx, unsigned char *buf, int max, long ms);
	/* ms on the air for a frame of n bytes (with the PHY's own sync) */
	long (*airtime)(void *ctx, int n);
};

/* The stream's data: get fills buf[max] (bytes, 0 at the end, < 0 error);
 * put takes n bytes in order (0 ok, < 0 error). */
typedef int (*mdm_get_fn)(void *u, unsigned char *buf, int max);
typedef int (*mdm_put_fn)(void *u, const unsigned char *buf, int n);

struct mdm_stat {
	long frames;			/* data frames sent or taken */
	long resent;			/* sent again */
	long bad;			/* frames dropped: CRC, length, sid */
	long bytes;			/* payload bytes delivered or acked */
};

#define MDM_OK		0
#define MDM_ELOST	(-1)		/* 20 bursts without progress */
#define MDM_EIDLE	(-2)		/* nothing heard for idle_ms */
#define MDM_EIO		(-3)		/* the PHY, get or put failed */
#define MDM_EARG	(-4)

#define MDM_RETRIES	20

/* The ARQ stream (P.3, RP.4).  window: 1..MDM_WMAX; wbuf: window * (mtu +
 * 2) bytes.  The sender's sid is any random byte (another transfer's
 * frames are ignored).  progress (may be NULL) is called after each ACK
 * and each delivered frame. */
int mdm_send(const struct mdm_phy *p, unsigned char sid, int window,
	mdm_get_fn get, void *u, unsigned char *wbuf, struct mdm_stat *st,
	void (*progress)(void *u, const struct mdm_stat *st));
int mdm_recv(const struct mdm_phy *p, int window, long idle_ms,
	mdm_put_fn put, void *u, unsigned char *wbuf, struct mdm_stat *st,
	void (*progress)(void *u, const struct mdm_stat *st));

/* ---- carousel (simplex, RP.4) ---- */
#define MDM_CAR_MAX	4096		/* frames per transfer */
struct mdm_car {
	unsigned char *map;		/* MDM_CAR_MAX / 8 bytes, the caller's */
	int sid;			/* -1 until the first frame */
	int total;			/* -1 until the LAST frame */
	int have;
	long size;			/* bytes, known with the LAST frame */
};
void mdm_car_init(struct mdm_car *c, unsigned char *map);
/* Frame idx of a carousel of chunk-byte pieces (the last with LAST). */
int mdm_car_pack(unsigned char sid, int idx, int last,
	const unsigned char *data, int n, unsigned char *out, int max);
/* A frame from the air: 1 if new (store f->len bytes at idx * chunk,
 * idx = f->a | f->b << 8), 0 if known or not ours, -1 if it breaks the
 * rules (index past MDM_CAR_MAX, a short frame that is not the last). */
int mdm_car_take(struct mdm_car *c, const struct mdm_frame *f, int chunk);
int mdm_car_done(const struct mdm_car *c);

/* ---- RFID line code P4 (P.5) ----
 * A cycle: Q carrier periods loaded, then (L-1)*Q not; L = 2..5 carries
 * two bits (most significant pair first), L = 7 is the sync.  Segments for
 * RFIDIOCEMU: dur = L*Q - 1, pulse = Q.  The decoder takes the reader's
 * edge times (us, the level of the first unknown) and calls frame() for
 * every frame whose CRC is good. */
#define MDM_P4_FAST	16		/* Q in carrier periods: 128 us */
#define MDM_P4_ROBUST	32		/* 256 us */
#define MDM_P4_SYNC	7
#define MDM_P4_PERIOD_US 8		/* 125 kHz */
#define MDM_RFID_MTU	24		/* a frame of 125 cycles (spec RP.5a) */

/* segments for n bytes: 1 + 4 n */
int mdm_p4_encode(const unsigned char *b, int n, int q,
	unsigned long *dur, unsigned long *pulse, int max);
/* how many frames were found */
int mdm_p4_decode(const unsigned long *d, int nd, int q, int mtu,
	void (*frame)(void *u, const unsigned char *buf, int n), void *u);
/* us the frame of n bytes takes on average (random data) */
long mdm_p4_us(int n, int q);

/* ---- Sub-GHz: CC1101 (P.4) ---- */
#define MDM_MOD_2FSK	0
#define MDM_MOD_GFSK	1
#define MDM_MOD_OOK	3

struct mdm_sgphy {
	unsigned long baud;		/* symbols/s */
	unsigned long dev;		/* Hz (FSK) */
	unsigned long bw;		/* receive filter, Hz */
	int mod;
};
struct mdm_preset {
	char name[8];
	struct mdm_sgphy phy;
	char use[32];
};
/* No pointers in these tables: they stay in flash in PIC programs. */
extern const struct mdm_preset mdm_presets[];	/* ends with name "" */
const struct mdm_preset *mdm_preset(const char *name);

struct mdm_cc1101 {
	unsigned char freq2, freq1, freq0;
	unsigned char mdmcfg4, mdmcfg3, mdmcfg2, deviatn;
	unsigned long freq, baud, dev, bw;	/* what the chip really does */
	int mod;
};
#define MDM_XOSC	26000000UL
/* The nearest CC1101 values: 0, or -1 if a value is outside the chip's
 * range (frequency outside 300-348, 387-464, 779-928 MHz, rate outside
 * 0.6-500 kBaud, deviation outside 1.6-380 kHz, filter outside
 * 58-812 kHz). */
int mdm_cc1101_regs(unsigned long freq, const struct mdm_sgphy *in,
	struct mdm_cc1101 *out);
/* Hz the signal occupies: FSK 2 * dev + baud, OOK 2 * baud */
unsigned long mdm_occupied(const struct mdm_cc1101 *r);

/* A band a region allows data in (spec P.4 table). */
struct mdm_band {
	char region[3];
	unsigned long lo, hi;		/* Hz */
	int dbm;			/* the highest PATABLE step allowed */
	int duty_pm;			/* duty cycle, per mille; 1000 = none */
	char note[28];
};
extern const struct mdm_band mdm_bands[];	/* ends with region "" */

#define MDM_SG_OK	0
#define MDM_SG_REGION	(-1)		/* no data bands in this region */
#define MDM_SG_BAND	(-2)		/* the signal leaves every band */
/* Whether a transmit at r (from mdm_cc1101_regs) is allowed in region;
 * *band is the band it lies in (or NULL). */
int mdm_sg_check(const char *region, const struct mdm_cc1101 *r,
	const struct mdm_band **band);
/* The output power for band b: the highest PATABLE step allowed. */
int mdm_sg_dbm(const struct mdm_band *b);
/* ms on the air for n bytes: preamble 4, sync 4, length 1, n, CRC 2 */
long mdm_sg_airtime(unsigned long baud, int n);

/* ---- duty cycle (RP.7) ----
 * Transmit time in 10 ms units in 7 slots of 10 minutes: any 60-minute
 * window lies inside the last 7, so their sum <= the limit holds for all. */
#define MDM_DUTY_SLOTS	7
#define MDM_DUTY_SLOT_S	600
struct mdm_duty {
	unsigned long slot0;		/* the newest slot's number (time/600) */
	unsigned short slot[MDM_DUTY_SLOTS];	/* [0] newest */
};
void mdm_duty_init(struct mdm_duty *d);
/* Seconds to wait before air_10ms more may go (0: now; -1: never, the
 * packet alone is over the limit). */
long mdm_duty_wait(struct mdm_duty *d, unsigned long now_s, int duty_pm,
	unsigned air_10ms);
void mdm_duty_add(struct mdm_duty *d, unsigned long now_s, unsigned air_10ms);
unsigned long mdm_duty_used(struct mdm_duty *d, unsigned long now_s);

#endif /* _MODEM_H */
