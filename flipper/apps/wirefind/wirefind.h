/* wirefind - the cores of the `wirefind` app (Galtzagorriak MINIX, user
 * request 2026-11-25): three indicators, none needs extra hardware.
 *
 *  metal  125 kHz RFID antenna detuning: the carrier amplitude moves
 *         when metal or a wire is near the coil (wf_detune).
 *  mains  50/60 Hz picked up by a floating ADC pin: a Goertzel filter
 *         on the samples, strength -> beeper pitch (wf_mains).
 *  noise  CC1101 RSSI at a few frequencies against a baseline taken away
 *         from the wall: clock harmonics of a data cable (wf_noise).
 *
 * Pure C, integer only, no malloc, no writable statics; the caller owns
 * the states.  Host-tested.  The sensors (coil ADC, pin ADC, CC1101) are
 * the device lane's; thresholds are tuned on the device (#109).
 */
#ifndef WIREFIND_H
#define WIREFIND_H

/* ---- mains: Goertzel at one frequency ---------------------------------- */
#define WF_BLOCK 400	/* samples per block: 400 at 2 kHz = 0.2 s = 10 periods
			 * of 50 Hz (60 Hz: 12), so the bin is exact */

struct wf_mains {
	long s1, s2;		/* Goertzel state (Q14 coefficient) */
	long coef;		/* 2 cos(w) in Q14 */
	unsigned short n;	/* samples in this block */
	unsigned long level;	/* amplitude of the last block, ADC counts */
	long dc;		/* running mean (removed before the filter) */
};

/* hz: 50 or 60; rate: samples per second, such that WF_BLOCK * hz / rate
 * is a whole number.  Only 2 kHz is built in the table (hz 50, 60). */
int  wf_mains_init(struct wf_mains *m, int hz, int rate);
/* One ADC sample.  Returns 1 when a block ended (m->level is new). */
int  wf_mains_put(struct wf_mains *m, int sample);
/* Beeper pitch for a level, 0 if below the noise floor: 200..4000 Hz,
 * logarithmic steps (floor, 4000 Hz at 'full'). */
int  wf_pitch(unsigned long level, unsigned long floor, unsigned long full);

/* Sensitivity 0..7 (coarse far search at 7, narrow down at 0): the floor
 * and the full-scale level for wf_pitch, in ADC counts.  Each step halves
 * both, so the pitch rises for weaker fields; 7 reaches a live wire 10 to
 * 20 cm away with a plain-wire antenna (to be confirmed on the device). */
void wf_sens(int sens, unsigned long *floor, unsigned long *full);

/* ---- metal: amplitude against a baseline ------------------------------- */
struct wf_detune {
	long base;		/* the baseline amplitude (x16) */
	long cur;		/* smoothed amplitude (x16) */
	unsigned char have;
};
void wf_detune_init(struct wf_detune *d);
/* A carrier amplitude (ADC counts) per read.  The baseline is taken by
 * wf_detune_zero (the user holds the Flipper away from metal and pushes
 * OK).  Returns the change in per mille of the baseline, signed (+: more
 * amplitude), 0 until a baseline exists. */
void wf_detune_put(struct wf_detune *d, int amp);
void wf_detune_zero(struct wf_detune *d);
int  wf_detune_delta(const struct wf_detune *d);

/* ---- noise: RSSI sweep against a baseline ------------------------------ */
#define WF_NF 6		/* frequencies in a sweep */
struct wf_noise {
	signed char base[WF_NF];	/* dBm at the baseline */
	signed char cur[WF_NF];		/* dBm now */
	unsigned char have;
};
void wf_noise_init(struct wf_noise *n);
void wf_noise_put(struct wf_noise *n, int i, int rssi);
void wf_noise_zero(struct wf_noise *n);
/* Number of frequencies that rose by at least 'rise' dB over the
 * baseline, and the mean rise (dB, 0 if none) in *mean. */
int  wf_noise_count(const struct wf_noise *n, int rise, int *mean);

/* ---- the verdict ------------------------------------------------------- */
#define WF_METAL 1
#define WF_MAINS 2
#define WF_DATA  4
/* Bits from the three indicators: metal when |detune| >= metal_pm per
 * mille; mains when level >= mains_min; data when at least noise_n
 * frequencies rose. */
int wf_verdict(int detune_pm, unsigned long mains_level, int noise_cnt,
	       int metal_pm, unsigned long mains_min, int noise_n);

#endif
