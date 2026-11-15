/* Framebuffer drawing and a minimal scrolling text console. */
#include <string.h>
#include "fbcon.h"
#include "font4x6.c"

uint8_t fb[LCD_W * LCD_H / 8];

/* Text console state: the character grid is the source of truth and is
 * re-rendered on scroll, which is simpler than shifting 6-pixel rows across
 * 8-pixel display pages. */
static char screen[FBCON_ROWS][FBCON_COLS];
static int cur_x, cur_y, dirty;

void fb_clear(void)
{
	memset(fb, 0, sizeof fb);
}

void fb_pixel(int x, int y, int on)
{
	uint8_t *p;

	if ((unsigned) x >= LCD_W || (unsigned) y >= LCD_H) return;
	p = &fb[(y >> 3) * LCD_W + x];
	if (on) *p |= 1 << (y & 7);
	else *p &= ~(1 << (y & 7));
}

void fb_char(int col, int row, int ch, int inverse)
{
	const unsigned char *glyph;
	int x, y, bits;

	if (ch < 32 || ch > 126) ch = ' ';
	glyph = font_4x6[ch - 32];
	for (y = 0; y < FONT_H; y++) {
		bits = glyph[y];
		if (inverse) bits = ~bits;
		for (x = 0; x < FONT_W; x++)
			fb_pixel(col * FONT_W + x, row * FONT_H + y,
				 bits & (0x80 >> x));
	}
}

void fb_text(int col, int row, const char *s, int inverse)
{
	while (*s != 0 && col < FBCON_COLS)
		fb_char(col++, row, *s++, inverse);
}

static void redraw(void)
{
	int r, c;

	fb_clear();
	for (r = 0; r < FBCON_ROWS; r++)
		for (c = 0; c < FBCON_COLS; c++)
			fb_char(c, r, screen[r][c], 0);
}

void fbcon_init(void)
{
	memset(screen, ' ', sizeof screen);
	cur_x = cur_y = 0;
	redraw();
	dirty = 1;
}

static void newline(void)
{
	cur_x = 0;
	if (++cur_y < FBCON_ROWS) return;
	memmove(screen[0], screen[1], (FBCON_ROWS - 1) * FBCON_COLS);
	memset(screen[FBCON_ROWS - 1], ' ', FBCON_COLS);
	cur_y = FBCON_ROWS - 1;
	redraw();
}

void fbcon_putc(int c)
{
	switch (c) {
	case '\n':
		newline();
		break;
	case '\r':
		cur_x = 0;
		break;
	case '\b':
		if (cur_x > 0) cur_x--;
		break;
	case '\t':
		do fbcon_putc(' '); while (cur_x & 7);
		break;
	default:
		if (cur_x >= FBCON_COLS) newline();
		screen[cur_y][cur_x] = c;
		fb_char(cur_x, cur_y, c, 0);
		cur_x++;
	}
	dirty = 1;
}

void fbcon_puts(const char *s)
{
	while (*s != 0) fbcon_putc(*s++);
}

void fbcon_flush(void)
{
	if (!dirty) return;
	dirty = 0;
	if (hal_lcd_flush(fb) < 0) dirty = 1;	/* SPI2 busy: the next flush */
}
