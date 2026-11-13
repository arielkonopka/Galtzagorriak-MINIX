/* info - "About" app: system name, version, uptime. */
#include <sys/types.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "gui.h"

int main(void)
{
	struct utsname u;
	struct tms t;
	char line[40];
	long up;

	if (gui_open() < 0) return 1;
	uname(&u);
	do {
		up = times(&t) / CLK_TCK;
		gui_clear();
		gui_title("ABOUT");
		gui_center(10, "Galtzagorriak MINIX", 0);
		sprintf(line, "%s %s.%s on %s", u.sysname, u.release, u.version,
								u.machine);
		gui_center(19, line, 0);
		sprintf(line, "host %s", u.nodename);
		gui_center(28, line, 0);
		sprintf(line, "up %ld:%02ld:%02ld", up / 3600, up / 60 % 60, up % 60);
		gui_center(37, line, 0);
		gui_center(52, "BACK to return", 0);
		gui_flush();
	} while (gui_key() != KEY_BACK);
	return 0;
}
