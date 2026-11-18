/* jiggle - keep the computer at the other end of the USB cable awake
 * (spec R11.3.6; T28e).
 *
 * Usage:	jiggle [-i SECONDS] [-p tiny|zero]	start (in the background
 *							with &, or from Teklatua)
 *		jiggle -k				stop the running one
 *
 * Every SECONDS (default 30, 1-3600) it writes mouse reports to
 * /dev/hidmouse: "tiny" a 1-pixel square (right, down, left, up: the
 * pointer ends where it was), "zero" one report that does not move it.
 *
 * It sleeps in poll() on /dev/hidmouse with the interval as the timeout,
 * so it wakes once per interval while the host listens, and not at all
 * while the cable is out, the host has not configured us, suspended the
 * bus or another USB persona holds it: then it waits without a timeout,
 * and the state change (POLLIN, R11.3.1) wakes it.
 *
 * While it runs, the desktop shows "J" on the status line (K.6): it runs
 * "guimod status jiggle J" as its child, so the record names jiggle's pid
 * and the desktop drops it when jiggle ends, however it ends.
 *
 * Stopping: jiggle reads the FIFO /tmp/jiggle (its own, mode 0600; /tmp
 * has the sticky bit), and "jiggle -k" writes one byte into it.  So -k
 * never signals a process by a remembered pid, which a stale file could
 * name after a reboot; a FIFO nobody reads is ENXIO, "not running".  On
 * -k jiggle removes the FIFO and runs "guimod close jiggle".  Killed
 * by a signal it just ends (no handler, so no MM gap is needed): no
 * button is ever held, every report has buttons 0.
 *
 * A small C program: no stdio, no malloc, no signal handler, no MM gap
 * (CHMEM_jiggle in minix.mk; RAM <= 1 KB).  Exit status: 0 stopped by
 * -k, 1 an error, 2 usage.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <minix/hid.h>

#define FIFO	"/tmp/jiggle"
#define MOUSE	"/dev/hidmouse"
#define GUIMOD	"/bin/guimod"

/* The patterns: whole 4-byte reports (buttons, dx, dy, wheel), const. */
static const unsigned char tiny[16] = {
	0, 1, 0, 0,	0, 0, 1, 0,	0, 0xFF, 0, 0,	0, 0, 0xFF, 0
};
static const unsigned char zero[4] = { 0, 0, 0, 0 };

static void say(const char *a, const char *b)
{
	(void) write(2, "jiggle: ", 8);
	(void) write(2, a, strlen(a));
	if (b != (const char *) 0) (void) write(2, b, strlen(b));
	(void) write(2, "\n", 1);
}

static int usage(void)
{
	say("usage: jiggle [-i SECONDS] [-p tiny|zero] | jiggle -k",
	    (const char *) 0);
	return 2;
}

/* Whether a report would go out now (HIDIOCSTATE also clears POLLIN). */
static int ready(int fd)
{
	int st = 0;

	if (ioctl(fd, HIDIOCSTATE, &st) < 0) return 0;
	return HID_PERSONA(st) == HIDP_NONE
		&& (st & (HID_CONFIGURED | HID_SUSPENDED)) == HID_CONFIGURED;
}

/* "jiggle -k": one byte into the running one's FIFO. */
static int stop(void)
{
	int fd = open(FIFO, O_WRONLY | O_NONBLOCK);

	if (fd < 0) {
		say(errno == ENXIO || errno == ENOENT ? "not running"
		    : "cannot open " FIFO, (const char *) 0);
		return 1;
	}
	if (write(fd, "k", 1) != 1) {
		say("cannot write to " FIFO, (const char *) 0);
		return 1;
	}
	return 0;
}

/* The FIFO -k writes into: its read end, and a write end kept open so
 * that poll() sees no hang-up while no -k has it open (two descriptors:
 * one opened O_RDWR would share its file position with the writers, as
 * MINIX FIFOs share the filp of the same direction, and never be
 * readable).  It must be ours. */
static int own_fifo(void)
{
	struct stat sb;
	int fd, w;

	fd = open(FIFO, O_WRONLY | O_NONBLOCK);
	if (fd >= 0) {			/* somebody reads it */
		close(fd);
		say("already running (jiggle -k stops it)", (const char *) 0);
		return -1;
	}
	if (mkfifo(FIFO, 0600) < 0 && errno != EEXIST) {
		say("cannot make ", FIFO);
		return -1;
	}
	fd = open(FIFO, O_RDONLY | O_NONBLOCK);
	if (fd < 0 || fstat(fd, &sb) < 0 || !S_ISFIFO(sb.st_mode)
	    || sb.st_uid != getuid() || (sb.st_mode & 077) != 0) {
		say(FIFO " is not jiggle's FIFO (another user's?)",
		    (const char *) 0);
		return -1;
	}
	(void) fcntl(fd, F_SETFD, FD_CLOEXEC);
	if ((w = open(FIFO, O_WRONLY | O_NONBLOCK)) < 0) {
		say("cannot open ", FIFO);
		return -1;
	}
	(void) fcntl(w, F_SETFD, FD_CLOEXEC);
	return fd;
}

/* "guimod status jiggle J" (on) or "guimod close jiggle", jiggle's child:
 * the record names jiggle.  Its complaints (no desktop) go nowhere:
 * jiggle works without a desktop.  execv, not execl: libc's execl keeps
 * a 1 KB argument array on the stack. */
static void desk(int on)
{
	char *av[5];
	pid_t pid;
	int st, fd;

	if ((pid = fork()) == 0) {
		if ((fd = open("/dev/null", O_WRONLY)) >= 0) (void) dup2(fd, 2);
		av[0] = "guimod";
		av[1] = on ? "status" : "close";
		av[2] = "jiggle";
		av[3] = on ? "J" : (char *) 0;
		av[4] = (char *) 0;
		(void) execv(GUIMOD, av);
		_exit(1);
	}
	if (pid > 0) (void) waitpid(pid, &st, 0);
}

int main(int argc, char **argv)
{
	struct pollfd p[2];
	const unsigned char *rep = tiny;
	long secs = 30;
	int i, n, on, r = 1, len = (int) sizeof tiny;
	char c, *s;

	for (i = 1; i < argc; i++) {
		s = argv[i];
		if (strcmp(s, "-k") == 0 && argc == 2) return stop();
		if (i + 1 >= argc) return usage();
		if (strcmp(s, "-i") == 0) {
			s = argv[++i];
			for (secs = 0; *s >= '0' && *s <= '9' && secs <= 3600; s++)
				secs = secs * 10 + (*s - '0');
			if (*s != 0 || secs < 1 || secs > 3600) {
				say("SECONDS: 1 to 3600", (const char *) 0);
				return 2;
			}
		} else if (strcmp(s, "-p") == 0) {
			s = argv[++i];
			if (strcmp(s, "tiny") == 0) {
				rep = tiny;
				len = (int) sizeof tiny;
			} else if (strcmp(s, "zero") == 0) {
				rep = zero;
				len = (int) sizeof zero;
			} else
				return usage();
		} else
			return usage();
	}

	if ((p[0].fd = open(MOUSE, O_WRONLY)) < 0) {
		say(errno == EBUSY ? "the USB port is busy (another persona): "
		    : "cannot open ", MOUSE);
		return 1;
	}
	(void) fcntl(p[0].fd, F_SETFD, FD_CLOEXEC);
	if ((p[1].fd = own_fifo()) < 0) return 1;
	desk(1);

	p[0].events = POLLIN;		/* a state change */
	p[1].events = POLLIN;		/* jiggle -k */
	on = ready(p[0].fd);
	for (;;) {
		p[0].revents = p[1].revents = 0;
		n = poll(p, 2, on ? (int) (secs * 1000) : -1);
		if (n < 0) {
			if (errno == EINTR) continue;
			say("poll failed", (const char *) 0);
			break;
		}
		if (p[1].revents & POLLIN) {
			(void) read(p[1].fd, &c, 1);
			r = 0;
			break;
		}
		if (p[0].revents != 0) {	/* plugged, unplugged, ... */
			on = ready(p[0].fd);
			continue;
		}
		if (n == 0 && on && write(p[0].fd, (const char *) rep,
					  (size_t) len) < 0)
			on = ready(p[0].fd);	/* EAGAIN, EBUSY: wait */
	}
	(void) unlink(FIFO);
	desk(0);
	return r;
}
