/* info - "About" app: system name, version, uptime. */
#include <sys/types.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <minix/gzversion.h>
#include "gui.h"

static void tick(int sig)
{
	signal(SIGALRM, tick);
}

int main(void)
{
	struct utsname u;
	struct tms t;
	char line[40];
	long up;
	int k;

	if (gui_open() < 0) return 1;
	signal(SIGALRM, tick);
	uname(&u);
	do {
		up = times(&t) / CLK_TCK;
		gui_clear();
		gui_title("ABOUT");
		/* 32 columns: one fact per line (owner, 2026-11-16) */
		sprintf(line, "%s %s", u.sysname, u.release);
		gui_center(10, line, 0);
		gui_center(18, "based on " GZ_BASE, 0);
		sprintf(line, "on %s, ABI %d", u.machine, GZ_ABI);
		gui_center(26, line, 0);
		sprintf(line, "host %.26s", u.nodename);
		gui_center(34, line, 0);
		sprintf(line, "up %ld:%02ld:%02ld", up / 3600, up / 60 % 60, up % 60);
		gui_center(42, line, 0);
		gui_center(52, "BACK to return", 0);
		gui_flush();
		/* the uptime ticks: redraw every second (the key read gives
		 * EINTR at the alarm); keys other than Back change nothing */
		alarm(1);
		while ((k = gui_key()) != 0 && k != KEY_BACK) ;
		alarm(0);
	} while (k != KEY_BACK);
	return 0;
}
