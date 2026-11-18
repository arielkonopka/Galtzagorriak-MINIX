/* mtpd - files to a PC over MTP (spec R10.4; T29a: the core and the test
 * transport).
 *
 * Usage:	mtpd [-r ROOT] [-l LINE]
 *
 * Offers ROOT (default /home/flipper, the same tree as /ext) as the one
 * MTP storage.  The host speaks to it on LINE (a serial line: the QEMU
 * test line /dev/mtpline) or, without -l, on standard input and output
 * (a pipe: tools/mtphost.py on the PC), in the framed stream of mtpfd.c.
 * It exits when the host goes (end of file, a broken frame); a file the
 * host was sending then is removed.  Started as root it runs as the user
 * flipper (spec R10.4): never with more rights than that user.
 *
 * T29b adds the USB transport; devd starts mtpd when the host configures
 * the device and stops it on detach.
 *
 * RAM (spec: <= 8 KB while it runs, 0 when it is not running): the 1 KB
 * buffer, the 2 KB parent ring, path buffers, opendir's 1 KB; no stdio.
 * Exit status: 0 the host went, 1 an error, 2 usage.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include "mtp.h"

#define ROOT	"/home/flipper"
#define USER	"flipper"

void tr_fd(int in, int out);

static void say(const char *s)
{
	if (write(2, "mtpd: ", 6) < 0 || write(2, s, strlen(s)) < 0
	    || write(2, "\n", 1) < 0)
		return;
}

int main(int argc, char **argv)
{
	const char *root = ROOT, *line = NULL;
	struct passwd *pw;
	int fd, i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) root = argv[++i];
		else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) line = argv[++i];
		else {
			say("usage: mtpd [-r root] [-l line]");
			return 2;
		}
	}
	if (line != NULL) {
		if ((fd = open(line, O_RDWR)) < 0) {
			say("cannot open the line");
			return 1;
		}
		tr_fd(fd, fd);
	} else {
		tr_fd(0, 1);
	}
	if (geteuid() == 0) {
		if ((pw = getpwnam(USER)) == NULL
		    || setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0
		    || geteuid() == 0) {
			say("cannot become " USER);
			return 1;
		}
	}
	signal(SIGPIPE, SIG_IGN);	/* a gone host: write() fails */
	if (fs_init(root) < 0) {
		say("no storage root");
		return 1;
	}
	mtp_init();
	while (mtp_step())
		;
	return 0;
}
