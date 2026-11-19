/* gui - drawing on the Flipper's 128x64 LCD and reading its buttons, for
 * Galtzagorriak MINIX programs.  Uses /dev/lcd and /dev/buttons. */
#ifndef GUI_GUI_H
#define GUI_GUI_H

#define GUI_W		128
#define GUI_H		64
#define GUI_COLS	32		/* 4x6 font */
#define GUI_ROWS	10

/* Button codes (as in flipper/hal/hal.h). */
#define KEY_UP		0x01
#define KEY_DOWN	0x02
#define KEY_LEFT	0x04
#define KEY_RIGHT	0x08
#define KEY_OK		0x10
#define KEY_BACK	0x20
#define KEY_LONG	0x80		/* not reported any more (keys come on press) */

int gui_open(void);			/* 0 or -1 */
void gui_close(void);
void gui_clear(void);
void gui_pixel(int x, int y, int on);
void gui_hline(int x0, int x1, int y, int on);
void gui_vline(int x, int y0, int y1, int on);
void gui_fill(int x0, int y0, int x1, int y1, int on);
void gui_frame(int x0, int y0, int x1, int y1);
int gui_text(int x, int y, const char *s, int inverse);	/* pixels used */
void gui_center(int y, const char *s, int inverse);
int gui_text_big(int x, int y, const char *s, int k);	/* k times larger */
void gui_title(const char *s);		/* inverse bar on the top line */
void gui_flush(void);
int gui_key(void);			/* next key press (blocking) */
int gui_key_nowait(void);		/* next key press or 0 */
/* Keys as clicks (R5.6a.4): a key in `clicks` is returned on its release,
 * and only if it was pressed alone (no other key went down while it was
 * held) and held less than ~1 s; the others on their press, as gui_key().
 * So a chord such as the backlight gesture Left+Up (R6.10) never clicks.
 * The state is the caller's (stack: no .bss); zero it before the first
 * call and after giving the buttons away (an app ran).  ms < 0: wait for
 * a key; else at most ms, then 0. */
struct gui_in {
	unsigned char down;		/* keys down now */
	unsigned char spoilt;		/* down together with another key */
	long t[6];			/* clock ticks at each key's press */
	unsigned char raw;		/* gui_wait: releases too (GUI_UP) */
};
int gui_input(struct gui_in *in, int clicks, int ms);
/* gui_input, and also returns GUI_FD as soon as fd (>= 0, e.g. a window
 * app's records, spec K.8) is readable or closed.  Keys come first. */
#define GUI_FD		0x100
/* With in->raw set (a K.8 pad window), every press is returned at once,
 * clicks included, and every release as KEY | GUI_UP, with GUI_CLICK too
 * when it ends a click (a key in clicks, alone, held less than ~1 s). */
#define GUI_UP		0x200
#define GUI_CLICK	0x400
int gui_wait(struct gui_in *in, int clicks, int ms, int fd);
int gui_click(void);			/* the next click of any key */
/* An 8-pixel-wide bitmap, n rows of one byte (MSB left) at x, y. */
void gui_bits8(int x, int y, const unsigned char *rows, int n, int inverse);
int gui_glyph_row(int c, int row);	/* the font's row of a character */
/* The two stock fonts (spec R5.6b.4; u8g2 helvB08 and haxrcorp 4089, as
 * the stock Canvas's FontPrimary and FontSecondary): s with its baseline
 * at y, the font's pixels set to `on` (the others untouched, as the
 * Canvas's transparent font mode).  The pixels moved right. */
#define GUI_FONT_PRIMARY	0
#define GUI_FONT_SECONDARY	1
int gui_text_font(int x, int y, int font, const char *s, int on);
int gui_font_width(int font, const char *s);	/* as canvas_string_width */
void gui_led(int r, int g, int b);
void gui_backlight(int level);

#endif /* GUI_GUI_H */
