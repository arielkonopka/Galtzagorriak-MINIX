/* Text console on the 128x64 framebuffer using the 4x6 font: 32x10 chars. */
#ifndef FBCON_H
#define FBCON_H

#include <stdint.h>
#include "hal.h"

#define FBCON_COLS (LCD_W / 4)
#define FBCON_ROWS (LCD_H / 6)

extern uint8_t fb[LCD_W * LCD_H / 8];

void fb_clear(void);
void fb_pixel(int x, int y, int on);
void fb_char(int col, int row, int ch, int inverse);
void fb_text(int col, int row, const char *s, int inverse);

void fbcon_init(void);
void fbcon_putc(int c);		/* handles \n \r \b \t, scrolls */
void fbcon_puts(const char *s);
void fbcon_flush(void);		/* push framebuffer to the LCD if dirty */

#endif /* FBCON_H */
