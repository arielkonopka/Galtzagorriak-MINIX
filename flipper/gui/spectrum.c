/* spectrum - the Sub-GHz spectrum app (issue #157, the user's wish #9):
 * sweeps /dev/subghz across a band and shows the strongest signal of
 * each step as bars or as a waterfall.  Receive only: nothing here
 * transmits.
 *
 * One step is a tune (write the frequency in Hz) and one read ("rssi=-92
 * peak=-41 freq=433919830"; the read samples for about 30 ms), so 32 steps
 * take about a second.  The keys are read between two steps, never after
 * a whole sweep: a press waits at most one step (~33 ms) before the
 * screen answers.
 *
 *	spectrum [device]	(default /dev/subghz; m2test uses /dev/null
 *				 for the "no radio" screen)
 *
 * Screens (spec K.2 "Pine meets modern UX"):
 *  - the band list: 433.05-434.79 (EU SRD), 868.0-868.6, 315 MHz, Custom;
 *  - Custom: the centre frequency, digit by digit;
 *  - the view: bars (peak per step, -110..-30 dBm, peak-hold dots that
 *    fall 2 dB a sweep) or a waterfall (a row per sweep, newest on top,
 *    4 greys by dithering).  Left/Right move the cursor (its frequency and
 *    dBm replace the hint line for 2 s; past an edge the view moves),
 *    Up/Down zoom in/out around it, OK switches bars/waterfall, Back
 *    writes "0" (the radio sleeps) and goes back to the list.
 *
 * RAM: no malloc and no stdio; the frame buffer (gui.c, 1 KB), the
 * waterfall (46 rows x 8 B) and 64 B of bars are .bss.
 */
#include <sys/types.h>
#include <sys/times.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "gui.h"
#include "specmath.h"

#define ROW_Y(i)	(7 + (i) * 6)	/* list rows, as the launcher's */
#define HINT_Y		58		/* the hint line; its rule at 56 */
#define G_TOP		10		/* the graph's top row (8, 9: cursor) */
#define G_BOT		55		/* its bottom row */
#define G_H		(G_BOT - G_TOP + 1)	/* 46 rows, also the waterfall */
#define STEP_W		(128 / SP_N)	/* 4 px a step: a 3 px bar, a gap */
#define FLUSH_EVERY	8		/* bars: show a sweep in 4 parts */

static const struct preset {
	const char *name;
	unsigned long f0, span;		/* span 0: Custom */
} presets[] = {
	{ "433.05-434.79  EU SRD", 433050000UL, 1740000UL },
	{ "868.0-868.6    EU SRD", 868000000UL, 600000UL },
	{ "315 MHz        US", 314000000UL, 2000000UL },
	{ "Custom", 0, 0 },
};
#define NPRESET	((int) (sizeof presets / sizeof presets[0]))

static const char *dev = "/dev/subghz";
static int sg = -1;			/* the device, -1: no radio */
static struct sp_view view;
static signed char peak[SP_N];		/* the last read of each step */
static signed char hold[SP_N];		/* the peak-hold dots */
static unsigned char wf[G_H][SP_N / 4];	/* 2 bits a step, a row a sweep */
static int wf_top, wf_rows;		/* the newest row, rows filled */
static int cur = SP_N / 2;		/* the cursor's step */
static int wfall;			/* 1: the waterfall, 0: bars */
static long note_end;			/* the readout shows until then */
static unsigned long custom_khz = 433920;

static long now(void)
{
	struct tms t;

	return (long) times(&t);
}

/* ---- the radio ---- */

/* Listen at hz and read one line: 1 and the peak, 0 if refused or no
 * line.  Never called outside the bands (sp_in_band). */
static int listen_at(unsigned long hz, int *pk)
{
	char t[40];
	int i = (int) sizeof t, n;

	t[--i] = 0;
	do { t[--i] = (char) ('0' + hz % 10); hz /= 10; } while (hz != 0);
	if (sg < 0) return 0;
	if (write(sg, t + i, sizeof t - 1 - i) != (int) (sizeof t - 1 - i))
		return 0;
	if ((n = read(sg, t, sizeof t)) <= 0) return 0;
	return sp_parse(t, n, pk);
}

static void radio_off(void)
{
	if (sg >= 0) (void) write(sg, "0", 1);
}

/* ---- drawing ---- */

static void hints(const char *s)
{
	gui_hline(0, GUI_W - 1, HINT_Y - 2, 1);
	gui_text(1, HINT_Y, s, 0);
}

static void title(const char *name, const char *right)
{
	gui_title(name);
	if (right != NULL)
		gui_text(GUI_W - 1 - (int) strlen(right) * 4, 1, right, 1);
}

static int wf_get(int r, int j)
{
	return wf[r][j >> 2] >> ((j & 3) * 2) & 3;
}

static void draw_view(void)
{
	char s[40], *p;
	int j, x, y, h, x0;

	gui_clear();
	title("SPECTRUM", sp_spanstr(s, view.span));
	for (j = 0; j < SP_N; j++) {
		x0 = sp_x(j);
		if (!sp_in_band(sp_freq(&view, j))) {	/* no radio there */
			for (x = x0; x < x0 + STEP_W; x += 2)
				gui_pixel(x, G_BOT, 1);
			continue;
		}
		if (wfall) {
			for (y = 0; y < wf_rows; y++) {
				int lv = wf_get((wf_top + y) % G_H, j);

				for (x = x0; x < x0 + STEP_W; x++)
					if (sp_dither(lv, x, G_TOP + y))
						gui_pixel(x, G_TOP + y, 1);
			}
			continue;
		}
		if ((h = sp_height(peak[j], G_H)) > 0)
			gui_fill(x0, G_BOT - h + 1, x0 + STEP_W - 2, G_BOT, 1);
		if ((h = sp_height(hold[j], G_H)) > 0)
			gui_hline(x0, x0 + STEP_W - 2, G_BOT - h + 1, 1);
	}
	x0 = sp_x(cur);				/* the cursor: a small v */
	gui_hline(x0, x0 + STEP_W - 2, 8, 1);
	gui_pixel(x0 + 1, 9, 1);
	if (note_end != 0) {			/* the readout */
		p = sp_mhz(s, sp_freq(&view, cur));
		p += strlen(p);
		strcpy(p, " MHz  ");
		p += 6;
		if (!sp_in_band(sp_freq(&view, cur)))
			strcpy(p, "no band");
		else if (peak[cur] == SP_NODATA)
			strcpy(p, "--- dBm");
		else {
			int v = peak[cur];

			if (v < 0) { *p++ = '-'; v = -v; }
			if (v >= 100) *p++ = (char) ('0' + v / 100);
			if (v >= 10) *p++ = (char) ('0' + v / 10 % 10);
			*p++ = (char) ('0' + v % 10);
			strcpy(p, " dBm");
		}
		hints(s);
	} else hints(wfall ? "<>Cursor ^vZoom OK Bars Back"
			   : "<>Cursor ^vZoom OK Wfall Back");
	gui_flush();
}

/* The whole-screen notice for an error that ends the app (spec K.2). */
static void no_radio(void)
{
	int k;

	gui_clear();
	title("SPECTRUM", NULL);
	gui_center(22, "no radio", 0);
	gui_center(32, dev, 0);
	hints("Back Quit");
	gui_flush();
	do k = gui_key(); while (k != KEY_BACK && k != KEY_OK && k != 0);
}

/* ---- the view ---- */

static void reset(void)
{
	memset(peak, SP_NODATA, sizeof peak);
	memset(hold, SP_NODATA, sizeof hold);
	wf_rows = 0;
}

static void wf_push(void)
{
	int j;

	wf_top = (wf_top + G_H - 1) % G_H;
	memset(wf[wf_top], 0, sizeof wf[0]);
	for (j = 0; j < SP_N; j++)
		wf[wf_top][j >> 2] |= (unsigned char)
			(sp_level(peak[j]) << ((j & 3) * 2));
	if (wf_rows < G_H) wf_rows++;
}

/* A key in the view; 1 on Back.  *i restarts the sweep when the view
 * changed. */
static int key(int k, int *i)
{
	if (k == KEY_BACK || k == 0) return 1;
	if (k == KEY_LEFT || k == KEY_RIGHT) {
		int d = k == KEY_LEFT ? -1 : 1;

		if (cur + d >= 0 && cur + d < SP_N) cur += d;
		else if (sp_pan(&view, d)) { reset(); *i = 0; }
		note_end = now() + 2 * CLK_TCK;
		if (note_end == 0) note_end = 1;
	}
	if ((k == KEY_UP || k == KEY_DOWN) && sp_zoom(&view, cur, k == KEY_UP)) {
		reset();
		*i = 0;
	}
	if (k == KEY_OK) wfall = !wfall;
	draw_view();
	return 0;
}

static int any_band(void)
{
	int j;

	for (j = 0; j < SP_N; j++)
		if (sp_in_band(sp_freq(&view, j))) return 1;
	return 0;
}

/* Sweep until Back; 0, or -1 when the radio refuses an in-band step
 * before it ever answered (no radio). */
static int run_view(void)
{
	int i = 0, k, pk, ok = 0;

	cur = SP_N / 2;
	note_end = 0;
	reset();
	draw_view();
	for (;;) {
		/* the keys first, between two steps */
		while ((k = gui_key_nowait()) != 0)
			if (key(k, &i)) {
				radio_off();
				return 0;
			}
		if (note_end != 0 && now() - note_end >= 0) {
			note_end = 0;
			draw_view();
		}
		/* a view wholly between the bands has nothing to sweep:
		 * sleep until a key (no busy loop) */
		if (i == 0 && !any_band()) {
			radio_off();
			if (key(gui_key(), &i)) return 0;
			continue;
		}
		/* one step */
		if (sp_in_band(sp_freq(&view, i))) {
			if (listen_at(sp_freq(&view, i), &pk)) {
				ok = 1;
				if (pk < SP_DBM_MIN - 10) pk = SP_DBM_MIN - 10;
				if (pk > 0) pk = 0;
				peak[i] = (signed char) pk;
			} else {
				if (!ok) return -1;
				peak[i] = SP_NODATA;
			}
		} else peak[i] = SP_NODATA;
		if (++i == SP_N) {		/* a sweep done */
			for (i = 0; i < SP_N; i++)
				hold[i] = (signed char) sp_hold(hold[i], peak[i]);
			wf_push();
			i = 0;
			draw_view();
		} else if (!wfall && i % FLUSH_EVERY == 0) draw_view();
	}
}

/* ---- the band list and Custom ---- */

static int choose(int sel)
{
	int i, y, k;

	for (;;) {
		gui_clear();
		title("SPECTRUM", NULL);
		for (i = 0; i < NPRESET; i++) {
			y = ROW_Y(i);
			if (i == sel) gui_fill(0, y - 1, GUI_W - 1, y + 5, 1);
			gui_text(6, y, presets[i].name, i == sel);
			if (i == sel) gui_text(1, y, ">", 1);
		}
		hints("^v Pick  OK Start  Back Quit");
		gui_flush();
		k = gui_key();
		if (k == KEY_BACK || k == 0) return -1;
		if (k == KEY_UP) sel = (sel + NPRESET - 1) % NPRESET;
		if (k == KEY_DOWN) sel = (sel + 1) % NPRESET;
		if (k == KEY_OK) return sel;
	}
}

/* The centre frequency, in kHz, digit by digit; 1 on OK, 0 on Back. */
static int custom(void)
{
	static const unsigned long pow10[6] = {
		100000, 10000, 1000, 100, 10, 1
	};
	char s[12];
	int d = 2, i, k, x;

	for (;;) {
		gui_clear();
		title("CUSTOM", NULL);
		gui_text(6, 18, "Centre", 0);
		sp_mhz(s, custom_khz * 1000UL);
		x = 6 + 7 * 4;
		for (i = 0; s[i] != 0; i++) {
			char c[2];
			int di = i < 3 ? i : i - 1;	/* the digit, past '.' */

			c[0] = s[i];
			c[1] = 0;
			gui_text(x + i * 4, 18, c, s[i] != '.' && di == d);
		}
		gui_text(x + i * 4, 18, " MHz", 0);
		gui_text(6, 28, "Span   2.00M (zoom with ^v)", 0);
		hints("<>Digit ^vChange OK Start Back");
		gui_flush();
		k = gui_key();
		if (k == KEY_BACK || k == 0) return 0;
		if (k == KEY_OK) return 1;
		if (k == KEY_LEFT) d = (d + 5) % 6;
		if (k == KEY_RIGHT) d = (d + 1) % 6;
		if (k == KEY_UP && custom_khz + pow10[d] <= SP_F_HI / 1000)
			custom_khz += pow10[d];
		if (k == KEY_DOWN && custom_khz >= SP_F_LO / 1000 + pow10[d])
			custom_khz -= pow10[d];
	}
}

int main(int argc, char **argv)
{
	int sel = 0, pk;

	if (argc > 1) dev = argv[1];
	if (gui_open() < 0) return 1;
	sg = open(dev, O_RDWR);
	/* is a radio there? one step at 433.92 MHz, then let it sleep */
	if (sg < 0 || !listen_at(433920000UL, &pk)) {
		no_radio();
		radio_off();
		return 0;
	}
	radio_off();
	for (;;) {
		if ((sel = choose(sel)) < 0) break;
		if (presets[sel].span == 0) {
			if (!custom()) continue;
			sp_center(&view, custom_khz * 1000UL, 2000000UL);
		} else {
			view.f0 = presets[sel].f0;
			view.span = presets[sel].span;
		}
		if (run_view() < 0) {
			radio_off();
			no_radio();
			break;
		}
	}
	radio_off();
	return 0;
}
