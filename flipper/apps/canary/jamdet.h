/* jamdet - the `subghz` canary's jamming alarm (Galtzagorriak MINIX, spec
 * M.2, task T35): a band whose RSSI stays high for too long, with no gaps
 * a real transmitter has, is being jammed.
 *
 * Pure C, no I/O, no malloc, no writable statics.  Receive only (M.0).
 * Times are ms from a free-running counter (unsigned differences).  The
 * caller samples the CC1101's RSSI (dBm) every 10..100 ms.  Host-tested.
 */
#ifndef JAMDET_H
#define JAMDET_H

struct jd_rule {
	signed char hi;		/* dBm: at or above is "carrier" (-60) */
	unsigned short hold;	/* ms of carrier with no gap: alarm (3000) */
	unsigned short gap;	/* ms below hi that clears the run (200) */
	unsigned short clear;	/* ms below hi before the alarm clears (2000) */
};
#define JD_DEFAULT { -60, 3000, 200, 2000 }

struct jd_state {
	unsigned long up;	/* when the carrier run started */
	unsigned long down;	/* when the level last fell below hi */
	unsigned long last;	/* time of the last sample */
	unsigned char started, high, alarm;
};

#define JD_NONE		0
#define JD_ALARM	1	/* jamming began */
#define JD_CLEAR	2	/* back to quiet */

void jd_reset(struct jd_state *s);
/* One sample.  A sample gap longer than the hold is not trusted: the run
 * restarts (a sleeping caller must not raise an alarm). */
int jd_sample(const struct jd_rule *r, struct jd_state *s,
					unsigned long now, int rssi);

#endif
