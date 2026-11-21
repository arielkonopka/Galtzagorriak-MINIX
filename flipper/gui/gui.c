/* gui - see gui.h */
#include <sys/types.h>
#include <sys/times.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "gui.h"
#include "font4x6.c"
/* The stock fonts (u8g2, BSD-2): libfuri's copy, one source for both.
 * Only what a program calls is linked (--gc-sections). */
#ifndef GUI_HOST_U8G2		/* the host test links libfuri's own */
#include "../lib/furi/u8g2/u8g2_fonts.c"
#else
extern const unsigned char u8g2_font_helvB08_tr[], u8g2_font_haxrcorp4089_tr[];
#endif

static unsigned char fb[GUI_W * GUI_H / 8];
static int lcd_fd = -1, btn_fd = -1, btn_nb_fd = -1;

int gui_open(void)
{
	if ((lcd_fd = open("/dev/lcd", O_RDWR)) < 0) return -1;
	if ((btn_fd = open("/dev/buttons", O_RDONLY)) < 0) return -1;
	btn_nb_fd = open("/dev/buttons", O_RDONLY | O_NONBLOCK);
	gui_clear();
	return 0;
}

int gui_open_lcd(void)
{
	if ((lcd_fd = open("/dev/lcd", O_RDWR)) < 0) return -1;
	gui_clear();
	return 0;
}

void gui_image(const unsigned char *img)
{
	memcpy(fb, img, sizeof fb);
}

void gui_close(void)
{
	if (lcd_fd >= 0) close(lcd_fd);
	if (btn_fd >= 0) close(btn_fd);
	if (btn_nb_fd >= 0) close(btn_nb_fd);
	lcd_fd = btn_fd = btn_nb_fd = -1;
}

void gui_clear(void)
{
	memset(fb, 0, sizeof fb);
}

void gui_pixel(int x, int y, int on)
{
	unsigned char *p;

	if ((unsigned) x >= GUI_W || (unsigned) y >= GUI_H) return;
	p = &fb[(y >> 3) * GUI_W + x];
	if (on) *p |= 1 << (y & 7);
	else *p &= ~(1 << (y & 7));
}

void gui_hline(int x0, int x1, int y, int on)
{
	for (; x0 <= x1; x0++) gui_pixel(x0, y, on);
}

void gui_vline(int x, int y0, int y1, int on)
{
	for (; y0 <= y1; y0++) gui_pixel(x, y0, on);
}

void gui_fill(int x0, int y0, int x1, int y1, int on)
{
	for (; y0 <= y1; y0++) gui_hline(x0, x1, y0, on);
}

void gui_frame(int x0, int y0, int x1, int y1)
{
	gui_hline(x0, x1, y0, 1);
	gui_hline(x0, x1, y1, 1);
	gui_vline(x0, y0, y1, 1);
	gui_vline(x1, y0, y1, 1);
}

void gui_bits8(int x, int y, const unsigned char *rows, int n, int inverse)
{
	int gx, gy, bits;

	for (gy = 0; gy < n; gy++) {
		bits = rows[gy];
		if (inverse) bits = ~bits;
		for (gx = 0; gx < 8; gx++)
			gui_pixel(x + gx, y + gy, bits & (0x80 >> gx));
	}
}

int gui_glyph_row(int c, int row)
{
	if (c < 32 || c > 126) c = '?';
	return font_4x6[c - 32][row];
}

/*===========================================================================*
 *			The stock fonts (R5.6b.4)			     *
 *===========================================================================*/
/* The u8g2 font format: a 23-byte header, then per glyph its code, the
 * jump to the next glyph and a bit stream (LSB first): width, height, x
 * and y offset, the advance, then runs of background and foreground
 * pixels.  As u8g2_font_decode_glyph() (u8g2_font.c), without rotation. */
struct fdec {
	const unsigned char *p;
	int bit, w, x, y, lx, ly, on;
};

static unsigned fbits(struct fdec *d, int n)
{
	unsigned v = *d->p >> d->bit;
	int b = d->bit + n;

	if (b >= 8) {
		d->p++;
		v |= (unsigned) *d->p << (8 - d->bit);
		b -= 8;
	}
	d->bit = b;
	return v & ((1U << n) - 1);
}

static int fsbits(struct fdec *d, int n)
{
	return (int) fbits(d, n) - (1 << (n - 1));
}

static void frun(struct fdec *d, int cnt, int fg)
{
	int rem, cur;

	for (;;) {
		rem = d->w - d->lx;
		cur = cnt < rem ? cnt : rem;
		if (fg && cur > 0 && d->on >= 0)
			gui_hline(d->x + d->lx, d->x + d->lx + cur - 1,
				  d->y + d->ly, d->on);
		if (cnt < rem) break;
		cnt -= rem;
		d->lx = 0;
		d->ly++;
	}
	d->lx += cnt;
}

static const unsigned char *font_of(int font)
{
	return font == GUI_FONT_PRIMARY ? u8g2_font_helvB08_tr
					: u8g2_font_haxrcorp4089_tr;
}

/* One glyph at x, baseline y (on < 0: only measure).  Its advance; *wx
 * (if not NULL) its x offset + width, for the string's last glyph. */
static int fglyph(const unsigned char *f, int c, int x, int y, int on,
		  int *wx)
{
	const unsigned char *g = f + 23;
	struct fdec d;
	int h, gx, gy, dx, a, b;

	if (c >= 'a') g += f[19] << 8 | f[20];
	else if (c >= 'A') g += f[17] << 8 | f[18];
	for (; g[1] != 0 && g[0] != c; g += g[1]) ;
	if (g[1] == 0) return 0;
	d.p = g + 2;
	d.bit = 0;
	d.w = (int) fbits(&d, f[4]);
	h = (int) fbits(&d, f[5]);
	gx = fsbits(&d, f[6]);
	gy = fsbits(&d, f[7]);
	dx = fsbits(&d, f[8]);
	if (wx != NULL) *wx = gx + d.w;
	if (d.w > 0 && on >= 0) {
		d.x = x + gx;
		d.y = y - (h + gy);
		d.lx = d.ly = 0;
		d.on = on;
		for (;;) {
			a = (int) fbits(&d, f[2]);
			b = (int) fbits(&d, f[3]);
			do {
				frun(&d, a, 0);
				frun(&d, b, 1);
			} while (fbits(&d, 1) != 0);
			if (d.ly >= h) break;
		}
	}
	return dx;
}

int gui_text_font(int x, int y, int font, const char *s, int on)
{
	const unsigned char *f = font_of(font);
	int x0 = x;

	for (; *s != 0 && x < GUI_W; s++)
		x += fglyph(f, (unsigned char) *s, x, y, on, (int *) 0);
	return x - x0;
}

/* u8g2_GetStrWidth: the advances, but the last glyph's own width. */
int gui_font_width(int font, const char *s)
{
	const unsigned char *f = font_of(font);
	int w = 0, dx = 0, wx = 0;

	for (; *s != 0; s++) {
		dx = fglyph(f, (unsigned char) *s, 0, 0, -1, &wx);
		w += dx;
	}
	return w > 0 ? w - dx + wx : 0;
}

int gui_text(int x, int y, const char *s, int inverse)
{
	int x0 = x, gx, gy, bits, c;

	for (; *s != 0 && x < GUI_W; s++, x += FONT_W) {
		c = (unsigned char) *s;
		if (c < 32 || c > 126) c = '?';
		for (gy = 0; gy < FONT_H; gy++) {
			bits = font_4x6[c - 32][gy];
			if (inverse) bits = ~bits;
			for (gx = 0; gx < FONT_W; gx++)
				gui_pixel(x + gx, y + gy, bits & (0x80 >> gx));
		}
	}
	return x - x0;
}

int gui_text_big(int x, int y, const char *s, int k)
{
	int x0 = x, gx, gy, bits, c;

	for (; *s != 0 && x < GUI_W; s++, x += FONT_W * k) {
		c = (unsigned char) *s;
		if (c < 32 || c > 126) c = '?';
		for (gy = 0; gy < FONT_H * k; gy++) {
			bits = font_4x6[c - 32][gy / k];
			for (gx = 0; gx < FONT_W * k; gx++)
				gui_pixel(x + gx, y + gy, bits & (0x80 >> gx / k));
		}
	}
	return x - x0;
}

void gui_center(int y, const char *s, int inverse)
{
	gui_text((GUI_W - (int) strlen(s) * FONT_W) / 2, y, s, inverse);
}

void gui_title(const char *s)
{
	gui_fill(0, 0, GUI_W - 1, FONT_H, 1);
	gui_center(1, s, 1);
}

void gui_flush(void)
{
	if (lcd_fd < 0) return;
	lseek(lcd_fd, 0L, SEEK_SET);
	write(lcd_fd, fb, sizeof fb);
}

/* Events are (key, down) pairs; return a key as soon as it goes down
 * (T3: waiting for the release put the whole press, 50-150 ms, into the
 * button -> screen latency L1).  KEY_LONG is not reported any more: no
 * app used it, and long Back becomes the kernel's job (spec R4b.8). */
static int event(int fd)
{
	unsigned char ev[2];
	int k;

	for (;;) {
		if (read(fd, ev, 2) != 2) return 0;
		for (k = 0; k < 8 && !(ev[0] & (1 << k)); k++) ;
		if (k == 8 || !ev[1]) continue;		/* releases */
		return ev[0];
	}
}

int gui_key(void)
{
	return btn_fd < 0 ? 0 : event(btn_fd);
}

int gui_key_nowait(void)
{
	return btn_nb_fd < 0 ? 0 : event(btn_nb_fd);
}

static long ticks(void)
{
	struct tms t;

	return (long) times(&t);
}

int gui_wait(struct gui_in *in, int clicks, int ms, int fd)
{
	unsigned char ev[2];
	struct pollfd p[2];
	long end = ms < 0 ? 0 : ticks() + ((long) ms * CLK_TCK + 999) / 1000;
	long left;
	int k, b, r, rk;

	if (btn_fd < 0) return 0;
	for (;;) {
		/* a held repeating key: the poll wakes up for the next repeat */
		rk = in->rep & in->down & ~in->spoilt;
		if (rk != 0 && (rk & (rk - 1)) != 0) rk = 0;	/* one key only */
		if (ms >= 0 || fd >= 0 || rk != 0) {	/* a timed wait, two fds or a repeat: poll */
			left = ms < 0 ? -1 : end - ticks();
			if (ms >= 0 && left <= 0) return 0;
			if (rk != 0) {
				long w = in->next - ticks();

				if (w < 0) w = 0;
				if (left < 0 || w < left) left = w;
			}
			p[0].fd = btn_fd; p[0].events = POLLIN; p[0].revents = 0;
			p[1].fd = fd; p[1].events = POLLIN; p[1].revents = 0;
			r = poll(p, fd >= 0 ? 2 : 1,
				 left < 0 ? -1 : (int) (left * 1000 / CLK_TCK));
			if (r == 0 && rk != 0 && ticks() >= in->next) {
				in->next += ((long) (in->rep_ms ? in->rep_ms : GUI_REP_MS)
						* CLK_TCK + 999) / 1000;
				return rk;
			}
			if (r <= 0) continue;	/* the time is checked above */
			/* the buttons first: a key is never kept waiting
			 * behind the other side's records */
			if (!(p[0].revents & POLLIN)) {
				if (p[1].revents != 0) return GUI_FD;
				continue;
			}
		}
		if (read(btn_fd, ev, 2) != 2) return 0;
		for (b = 0; b < 6 && !(ev[0] & (1 << b)); b++) ;
		if (b == 6) continue;
		k = 1 << b;
		if (ev[1]) {				/* down */
			if (in->down & ~k) in->spoilt |= in->down | k;
			in->down |= k;
			in->t[b] = ticks();
			in->next = in->t[b]
				+ ((long) GUI_REP_FIRST * CLK_TCK + 999) / 1000;
			if (!(clicks & k) || in->raw) return k;
		} else {				/* up */
			int alone = !(in->spoilt & k);

			in->down &= ~k;
			in->spoilt &= ~k;
			if ((clicks & k) && alone
			    && ticks() - in->t[b] < CLK_TCK)
				return in->raw ? k | GUI_UP | GUI_CLICK : k;
			if (in->raw) return k | GUI_UP;
		}
	}
}

int gui_input(struct gui_in *in, int clicks, int ms)
{
	return gui_wait(in, clicks, ms, -1);
}

int gui_click(void)
{
	struct gui_in in;

	memset(&in, 0, sizeof in);
	return gui_input(&in, 0x3F, -1);
}

void gui_led(int r, int g, int b)
{
	unsigned char c[3];
	int fd;

	c[0] = r; c[1] = g; c[2] = b;
	if ((fd = open("/dev/led", O_WRONLY)) < 0) return;
	write(fd, c, 3);
	close(fd);
}

void gui_backlight(int level)
{
	unsigned char c = level;
	int fd;

	if ((fd = open("/dev/backlight", O_WRONLY)) < 0) return;
	write(fd, &c, 1);
	close(fd);
}
