/* console - "Console" app: shows the system console (the USB terminal) on
 * the LCD, live, until BACK.  The kernel draws the text (flipdev.c,
 * lcd_cons_putc); this app only switches it on and waits.  Meanwhile the
 * console's window size is the LCD's, 32x10, so full-screen programs
 * (mined) fit it; the old size comes back with BACK (T8, V4). */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include "gui.h"

#define LCD_COLS	31		/* 128 x 64 pixels, 4 x 6 font, in a frame */
#define LCD_ROWS	10

int main(void)
{
	int fd, tty;
	struct winsize old, lcd;

	if (gui_open() < 0) return 1;
	if ((tty = open("/dev/console", O_WRONLY)) >= 0
	    && ioctl(tty, TIOCGWINSZ, &old) == 0) {
		lcd = old;
		lcd.ws_row = LCD_ROWS;
		lcd.ws_col = LCD_COLS;
		(void) ioctl(tty, TIOCSWINSZ, &lcd);
	} else if (tty >= 0) {
		close(tty);
		tty = -1;
	}
	if ((fd = open("/dev/lcd", O_WRONLY)) < 0) return 1;
	write(fd, "C", 1);		/* the console from now on */
	close(fd);
	while (gui_key() != KEY_BACK) ;
	gui_clear();			/* any frame ends the console view */
	gui_flush();
	if (tty >= 0) {
		(void) ioctl(tty, TIOCSWINSZ, &old);
		close(tty);
	}
	return 0;
}
