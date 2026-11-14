/* console - "Console" app: shows the system console (the USB terminal) on
 * the LCD, live, until BACK.  The kernel draws the text (flipdev.c,
 * lcd_cons_putc); this app only switches it on and waits. */
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include "gui.h"

int main(void)
{
	int fd;

	if (gui_open() < 0) return 1;
	if ((fd = open("/dev/lcd", O_WRONLY)) < 0) return 1;
	write(fd, "C", 1);		/* the console from now on */
	close(fd);
	while (gui_key() != KEY_BACK) ;
	gui_clear();			/* any frame ends the console view */
	gui_flush();
	return 0;
}
