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
void gui_led(int r, int g, int b);
void gui_backlight(int level);

#endif /* GUI_GUI_H */
