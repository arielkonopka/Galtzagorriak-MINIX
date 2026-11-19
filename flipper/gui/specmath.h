/* specmath - the arithmetic of the Spectrum app (issue #157): which
 * frequency each step listens on, the dBm scale, zoom and pan, the grey
 * levels of the waterfall, and the /dev/subghz line.  Pure C, no I/O, so
 * the host test (tests/host/spectest.c) runs it unchanged. */
#ifndef GUI_SPECMATH_H
#define GUI_SPECMATH_H

#define SP_N		32		/* steps across the 128 px width */
#define SP_DBM_MIN	(-110)		/* the bottom of the scale */
#define SP_DBM_MAX	(-30)		/* the top */
#define SP_NODATA	(-128)		/* a step not listened to yet */
#define SP_SPAN_MIN	64000UL		/* Hz: 2 kHz per step */
#define SP_SPAN_MAX	128000000UL
#define SP_F_LO		300000000UL	/* the lowest band's start */
#define SP_F_HI		928000000UL	/* the highest band's end */
#define SP_HOLD_DECAY	2		/* dB a peak-hold dot falls per sweep */

struct sp_view {
	unsigned long f0;		/* Hz, the left edge */
	unsigned long span;		/* Hz, the whole width */
};

unsigned long sp_step(const struct sp_view *v);		/* Hz per step */
unsigned long sp_freq(const struct sp_view *v, int i);	/* step i's middle */
int sp_x(int i);			/* the first pixel column of step i */
int sp_in_band(unsigned long hz);	/* 1 if /dev/subghz takes it */
int sp_height(int dbm, int h);		/* bar height, 0..h pixels */
int sp_level(int dbm);			/* waterfall grey, 0 (white)..3 */
int sp_dither(int level, int x, int y);	/* 1: pixel x, y is black */
int sp_hold(int hold, int peak);	/* the hold dot after a sweep */
/* Centre the view on fc with the given span, inside 300..928 MHz. */
void sp_center(struct sp_view *v, unsigned long fc, unsigned long span);
/* Zoom in (in = 1, half the span) or out, keeping step cur at the same
 * frequency where it can; 1 if the view changed. */
int sp_zoom(struct sp_view *v, int cur, int in);
/* Move the view by a quarter of its width, dir -1 or 1; 1 if it moved. */
int sp_pan(struct sp_view *v, int dir);
/* A /dev/subghz line: 1 and its peak in *peak, or 0 ("off", junk). */
int sp_parse(const char *line, int n, int *peak);
/* hz as MHz with 3 decimals, "433.920" (at most 8 characters + NUL). */
char *sp_mhz(char *buf, unsigned long hz);
/* A span for the title bar: "1.74M", "600k", "64k" (at most 6 + NUL). */
char *sp_spanstr(char *buf, unsigned long span);

#endif /* GUI_SPECMATH_H */
