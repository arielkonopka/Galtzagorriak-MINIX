/* cfcore - the hidden camera finder's core (Galtzagorriak MINIX, spec F.5,
 * 9.1, task T13): pure C, no I/O, no malloc; the caller owns all state.
 * Host-tested (make test); the app, its drivers and its UI come later.
 */
#ifndef CFCORE_H
#define CFCORE_H

#include <stddef.h>

/* ---- The devboard's line protocol (R9.1.1, R9.1.8) ----
 *	AP <bssid> <rssi> <ch> <ssid>		an access point
 *	STA <mac> <rssi> <ch> <bssid>		a station and its AP
 *	RATE <mac> <bytes/s> <frames/s>		every 250 ms while tracking
 *	CH <n>					the channel is locked
 *	TRACK <mac>				tracking that station
 * MACs as aa:bb:cc:dd:ee:ff, RSSI in dBm (-127..0), channel 1..196.  Lines
 * may come cut or with garbage (a UART started mid-line): anything that
 * does not parse whole is CF_NONE. */
enum { CF_NONE, CF_AP, CF_STA, CF_RATE, CF_CH, CF_TRACK };
#define CF_SSID_MAX	32
struct cf_line {
	int type;
	unsigned char mac[6];	/* AP: the BSSID; STA, RATE, TRACK: the station */
	unsigned char bssid[6];	/* STA: its access point */
	int rssi, ch;
	unsigned long bytes, frames;	/* RATE, per second */
	char ssid[CF_SSID_MAX + 1];	/* AP; may be empty (hidden) */
};
int cf_parse(const char *s, size_t n, struct cf_line *l);

/* ---- Scoring (R9.1.2) ---- */
enum { CF_OTHER, CF_IOT, CF_CAMERA, CF_UNKNOWN };	/* levels, kinds */
struct cf_vendor {
	const char *name;
	int kind;		/* CF_CAMERA or CF_IOT */
	int first, count;	/* its OUIs in cf_oui[], 3 bytes each, sorted */
};
extern const unsigned char cf_oui[];
extern const struct cf_vendor cf_vendors[];
extern const int cf_nvendors;

int cf_vendor_of(const unsigned char mac[6]);	/* index or -1 */
struct cf_score {
	int level;		/* CF_CAMERA, CF_IOT, CF_OTHER; CF_UNKNOWN for a
				 * locally administered (random) MAC */
	int vendor;		/* index into cf_vendors, or -1 */
	int points;
};
void cf_score(const unsigned char mac[6], const char *ssid,
						struct cf_score *out);

/* ---- Motion correlation (R9.1.8) ----
 * Per 250 ms rate samples of one station: a "still" window and a "move"
 * window per round.  Rates are kept in 16-byte units, saturating (a ring
 * of 40 entries is 80 bytes). */
#define CF_MWIN		40	/* samples per 10 s window */
unsigned short cf_rate_unit(unsigned long bytes_per_s);
struct cf_round {
	int ratio;		/* median(move) / median(still), x100 */
	int corr;		/* correlation of the rate with the window
				 * pattern (still 0, move 1), x100 */
	int pass;		/* ratio >= 2 */
};
void cf_motion_round(const unsigned short *still, int ns,
		const unsigned short *move, int nm, struct cf_round *r);
int cf_motion_watches(const struct cf_round *r1, const struct cf_round *r2);

/* ---- Locate, "hot/cold" (R9.1.5) ---- */
struct cf_locate {
	int ema;		/* smoothed RSSI, dBm x16 */
	int ref;		/* OK pressed here (dBm x16), or below -127 */
	int fed;
};
void cf_locate_init(struct cf_locate *l);
void cf_locate_feed(struct cf_locate *l, int rssi);
void cf_locate_mark(struct cf_locate *l);	/* OK: this is the reference */
void cf_locate_out(const struct cf_locate *l, int *pitch_hz, int *period_ms,
								int *bar);

/* ---- IR modulation, Goertzel on ADC blocks (R9.1.7) ---- */
struct cf_goertzel {
	long coeff;		/* 2 cos(2 pi f / fs), Q14 */
	long s1, s2;		/* state, carried across DMA blocks */
	int n;
};
void cf_gz_init(struct cf_goertzel *g, int f_hz, int fs_hz);
void cf_gz_feed(struct cf_goertzel *g, const unsigned short *x, int n,
								int mean);
long long cf_gz_power(const struct cf_goertzel *g);	/* |X|^2 / n */
enum { CF_IR_NONE, CF_IR_STEADY, CF_IR_50, CF_IR_60, CF_IR_100, CF_IR_120,
       CF_IR_PWM };
int cf_ir_modulation(const unsigned short *x, int n, int fs_hz);

/* ---- Sub-GHz (R9.1.3) ---- */
struct cf_band { long start_khz, end_khz, step_khz; };
extern const struct cf_band cf_bands[];		/* the CC1101's three bands */
extern const int cf_nbands;
extern const struct cf_band cf_cell_bands[];	/* LTE B20, B8/GSM-900 UL */
extern const int cf_ncell_bands;
int cf_steps(const struct cf_band *b, int nb);
long cf_step_khz(const struct cf_band *b, int nb, int i);	/* or -1 */

struct cf_burst {		/* one step: noise floor, peak, bursts */
	int floor;		/* dBm x16, follows the quiet level */
	signed char peak;	/* peak hold, dBm */
	unsigned char above;	/* in a burst now */
	unsigned short bursts;
	unsigned short n;
};
#define CF_BURST_DB	10	/* above the floor: a burst */
void cf_burst_init(struct cf_burst *b);
void cf_burst_feed(struct cf_burst *b, int rssi);

#endif
