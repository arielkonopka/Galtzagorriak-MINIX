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

int gui_input(struct gui_in *in, int clicks, int ms)
{
	unsigned char ev[2];
	struct pollfd p;
	long end = ms < 0 ? 0 : ticks() + ((long) ms * CLK_TCK + 999) / 1000;
	long left;
	int k, b;

	if (btn_fd < 0) return 0;
	for (;;) {
		if (ms >= 0) {		/* a timed wait: poll first */
			left = end - ticks();
			if (left <= 0) return 0;
			p.fd = btn_fd; p.events = POLLIN; p.revents = 0;
			if (poll(&p, 1, (int) (left * 1000 / CLK_TCK)) <= 0)
				continue;	/* the time is checked above */
		}
		if (read(btn_fd, ev, 2) != 2) return 0;
		for (b = 0; b < 6 && !(ev[0] & (1 << b)); b++) ;
		if (b == 6) continue;
		k = 1 << b;
		if (ev[1]) {				/* down */
			if (in->down & ~k) in->spoilt |= in->down | k;
			in->down |= k;
			in->t[b] = ticks();
			if (!(clicks & k)) return k;
		} else {				/* up */
			int alone = !(in->spoilt & k);

			in->down &= ~k;
			in->spoilt &= ~k;
			if ((clicks & k) && alone && (in->down & k) == 0
			    && ticks() - in->t[b] < CLK_TCK)
				return k;
		}
	}
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
