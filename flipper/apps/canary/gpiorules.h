/* gpiorules - the `gpio` canary's core (Galtzagorriak MINIX, spec M.0,
 * R7b.4.4, task T35): debounce and the loop / contact / pulse / volt
 * rules on a stream of samples.
 *
 * Pure C: no I/O, no malloc, no writable statics; the caller owns the
 * rule and its state.  Input only: nothing here drives a pin (M.0).
 * Times are milliseconds from any free-running counter; differences are
 * taken unsigned, so the counter may wrap.  Host-tested (make test); the
 * `sensor` task (H.3, M4) that delivers the samples comes later.
 */
#ifndef GPIORULES_H
#define GPIORULES_H

enum gr_kind {
	GR_LOOP,	/* pull-up, wire loop to ground: open (high) = alarm */
	GR_CONTACT,	/* reed switch or button to ground: alarm on a change */
	GR_PULSE,	/* edge counts: alarm when the rate leaves [lo, hi] */
	GR_VOLT		/* ADC millivolts: alarm outside [lo, hi] */
};

/* gr_rule.when for GR_CONTACT (high = open with the pull-up) */
#define GR_ON_OPEN	1
#define GR_ON_CLOSE	2
#define GR_ON_ANY	3

#define GR_DEBOUNCE	20	/* ms, the default (R7b.4.4) */

struct gr_rule {
	unsigned char kind;	/* enum gr_kind */
	unsigned char when;	/* GR_CONTACT: GR_ON_* */
	unsigned char latch;	/* GR_LOOP, GR_PULSE, GR_VOLT: an alarm stays
				 * until gr_reset, even if the input is back */
	unsigned short debounce; /* ms a level must hold (GR_LOOP, GR_CONTACT;
				 * GR_VOLT: ms outside / inside the window) */
	unsigned short window;	/* GR_PULSE: ms per counting window */
	long lo, hi;		/* GR_PULSE: edges per window; GR_VOLT: mV */
	long hyst;		/* GR_VOLT: mV back inside before clearing */
};

struct gr_state {
	unsigned long since;	/* when the raw level last changed */
	unsigned long win;	/* GR_PULSE: start of the open window */
	unsigned long count;	/* GR_PULSE: edges in the open window;
				 * GR_CONTACT: alarms so far */
	long value;		/* last sample (level, edges, mV) */
	long rate;		/* GR_PULSE: edges in the last whole window */
	unsigned char started;
	unsigned char raw;	/* the input before the debounce */
	unsigned char stable;	/* after it; GR_UNKNOWN until first settled */
	unsigned char alarm;	/* in alarm now (or latched) */
};
#define GR_UNKNOWN	0xFF

/* What gr_sample returns */
#define GR_NONE		0
#define GR_ALARM	1	/* the rule fired (GR_CONTACT: each time) */
#define GR_CLEAR	2	/* back to normal (not with latch) */
/* GR_PULSE over a gap: if the rule fired and cleared again inside one
 * sample, gr_sample says GR_ALARM and gr_state.alarm is 0. */

void gr_reset(struct gr_state *s);
/* One sample at now ms.  v: GR_LOOP, GR_CONTACT the pin level (0 low,
 * else high); GR_PULSE the edges counted since the previous sample (the
 * pulse engine's delta, counted into the window open at now); GR_VOLT
 * millivolts after the divider.  Samples come in time order; the rule
 * needs no fixed sample rate.  A pulse window closes at the first sample
 * after its end; a gap of several windows counts as empty windows. */
int gr_sample(const struct gr_rule *r, struct gr_state *s,
					unsigned long now, long v);

#endif
