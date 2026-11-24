/* hangtest - a test app that never ends (Back 1 s / 4 s, FG_LONG and FG_KILL).
 *
 * Usage: hangtest busy | catch | deaf | sleep
 *   busy   a CPU loop with no signal handling at all: the polite request
 *          (SIGTERM) at Back 1 s ends it.
 *   catch  catches the first SIGTERM and ignores it; a second ends it.
 *   deaf   ignores SIGTERM and loops: only the stop at Back 4 s (SIGKILL)
 *          can end it.
 *   sleep  waits in pause(): SIGTERM ends it at once (like an app that waits
 *          for a key and never gets one).
 * It draws what it does, then hangs; the screen stays as drawn.  For the
 * device: not in the images; copy it to gui/hangtest.c, add `hangtest` to
 * GUIAPPS in minix.mk, send the base package, and add lines like
 *	Hang busy:nobody:@tools:/bin/hangtest busy
 * to /etc/apps by hand. */
#include <sys/types.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include "gui.h"

static volatile int seen;

static void on_term(int sig)
{
	if (seen++ > 0) _exit(1);
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "busy";
	volatile unsigned long n = 0;

	if (gui_open_lcd() < 0) return 1;
	gui_fill(0, 0, 127, 63, 0);
	gui_center(10, "hangtest", 1);
	gui_center(24, mode, 1);
	gui_center(40, "Back 1 s: request", 1);
	gui_center(50, "Back 4 s: stop", 1);
	gui_flush();
	if (strcmp(mode, "catch") == 0) signal(SIGTERM, on_term);
	if (strcmp(mode, "deaf") == 0) signal(SIGTERM, SIG_IGN);
	if (strcmp(mode, "sleep") == 0) for (;;) pause();
	for (;;) n++;
}
