/* SPDX-License-Identifier: GPL-3.0-or-later
 * badusb - run a Bad USB script (spec R11.3.3).
 *
 * Usage:	badusb [-n] [-p] [-l LAYOUT] [-d MS] FILE
 *
 *	-n	only check the script: the first error with its line
 *	-p	progress records on standard output (the Bad USB app):
 *		  L n total	line n starts
 *		  H		waiting for the host (cable, configured)
 *		  W		WAIT_FOR_BUTTON_PRESS: a line on stdin goes on
 *		  R		running again (after H or W)
 *		  D skipped	done (characters with no key in the layout)
 *		  E n text	an error at line n
 *		  X		stopped (a signal)
 *	-l	the keyboard layout (libhid: en-US built in, else
 *		/usr/share/hid/layouts/NAME.kl, ...); default en-US
 *	-d	the default delay after every line, ms (DEFAULT_DELAY)
 *
 * The script is read a line at a time (ducky.c), never whole.  Between
 * keys it sleeps in poll(); SIGTERM, SIGINT, SIGHUP and SIGPIPE are
 * blocked and looked at (sigpending) at least every STEP_MS, and after
 * every write, so a stop takes effect within 100 ms (AC11.R) and lets
 * every key and button go: the last report on each device it used is all
 * zeros.  No handler, no stdio, no malloc: MM needs no gap for it.
 *
 * Exit status: 0 done, 1 an error in the script or a device, 2 usage,
 * 3 stopped.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <minix/hid.h>
#include "libhid.h"
#include "ducky.h"

#define STEP_MS		50		/* the longest sleep between stop checks */

enum { D_KBD, D_MOUSE, D_CONS, ND };

static const char devname[ND][14] = { "/dev/hidkbd", "/dev/hidmouse",
				      "/dev/hidcons" };
static const unsigned char replen[ND] = { HID_KBD_LEN, HID_MOUSE_LEN,
					  HID_CONS_LEN };

static struct {
	int fd[ND];			/* -1: not open yet */
	unsigned char kb[HID_MAXWRITE];	/* keyboard reports not yet sent */
	int kn;
	char progress;			/* -p */
	char hostwait;			/* "H" said, not yet running again */
	char stopped;
	sigset_t stops;
	const char *file;
	const char *why;		/* a device error */
} b;

/* ---- output ---- */

/* Up to five strings (NULL: none) in one write: a record never arrives
 * in pieces. */
static void out5(int fd, const char *s1, const char *s2, const char *s3,
		 const char *s4, const char *s5)
{
	char m[160];
	const char *s[5];
	int i, n = 0, k;

	s[0] = s1; s[1] = s2; s[2] = s3; s[3] = s4; s[4] = s5;
	for (i = 0; i < 5; i++) {
		if (s[i] == NULL) continue;
		k = (int) strlen(s[i]);
		if (k > (int) sizeof m - n) k = (int) sizeof m - n;
		memcpy(m + n, s[i], (size_t) k);
		n += k;
	}
	if (write(fd, m, (unsigned) n) != n && fd == 1 && b.progress)
		b.stopped = 1;		/* the app is gone */
}

static void out(int fd, const char *s1, const char *s2, const char *s3,
		const char *s4)
{
	out5(fd, s1, s2, s3, s4, NULL);
}

static char *num(char *t, long v)		/* t[12] */
{
	char *p = t + 11;
	unsigned long u = (unsigned long) (v < 0 ? -v : v);

	*p = 0;
	do *--p = (char) ('0' + u % 10); while ((u /= 10) > 0);
	if (v < 0) *--p = '-';
	return p;
}

/* A progress record; the app gone (EPIPE): stop. */
static void prog(const char *rec)
{
	int n = (int) strlen(rec);

	if (b.progress && write(1, rec, (unsigned) n) != n) b.stopped = 1;
}

/* ---- stopping ---- */

static int stop_now(void)
{
	sigset_t p;
	int s;

	if (b.stopped) return 1;
	if (sigpending(&p) == 0)
		for (s = 1; s <= _NSIG; s++)
			if (sigismember(&b.stops, s) && sigismember(&p, s)) {
				b.stopped = 1;
				return 1;
			}
	return 0;
}

/* Sleep ms in steps of at most STEP_MS; -1 on a stop. */
static int nap(long ms)
{
	long t;

	for (;;) {
		if (stop_now()) return -1;
		if (ms <= 0) return 0;
		t = ms > STEP_MS ? STEP_MS : ms;
		(void) poll((struct pollfd *) NULL, 0, (int) t);
		ms -= t;
	}
}

/* ---- devices ---- */

static int dev(int i)
{
	if (b.fd[i] < 0 && (b.fd[i] = open(devname[i], O_WRONLY)) < 0) {
		b.why = errno == EBUSY ? "USB is busy (another USB mode holds it)"
		      : errno == EACCES ? "no permission (group usb)"
		      : "cannot open the device";
		return -1;
	}
	return b.fd[i];
}

/* Write n bytes of whole reports; while the host does not listen (no
 * cable, not configured, suspended: EAGAIN) wait in poll() for it.
 * 0, or -1 (a stop, or b.why). */
static int put(int i, const unsigned char *r, int n)
{
	struct pollfd p;
	int fd = dev(i), w;

	if (fd < 0) return -1;
	for (;;) {
		if (stop_now()) return -1;
		w = (int) write(fd, (const char *) r, (unsigned) n);
		if (w == n) {
			if (b.hostwait) {
				b.hostwait = 0;
				prog("R\n");
			}
			return 0;
		}
		if (w >= 0 || (errno != EAGAIN && errno != EINTR)) {
			b.why = w < 0 && errno == EBUSY
			      ? "USB is busy (another USB mode took it)"
			      : "write to the device failed";
			return -1;
		}
		if (errno == EAGAIN) {
			if (!b.hostwait) {
				b.hostwait = 1;
				prog("H\n");
			}
			p.fd = fd;
			p.events = POLLOUT;
			p.revents = 0;
			(void) poll(&p, 1, STEP_MS);
		}
	}
}

static int flush(void)
{
	int n = b.kn;

	if (n == 0) return stop_now() ? -1 : 0;
	b.kn = 0;
	return put(D_KBD, b.kb, n);
}

/* Every key and button up on the devices used, whatever was buffered
 * (sent at once, no waiting for the host: if it does not listen, it
 * holds no key of ours either). */
static void release_all(void)
{
	static const unsigned char zero[HID_KBD_LEN];
	int i;

	b.kn = 0;
	for (i = 0; i < ND; i++)
		if (b.fd[i] >= 0)
			(void) write(b.fd[i], (const char *) zero, replen[i]);
}

/* ---- dk_io ---- */

static int io_kbd(void *c, const unsigned char r[8])
{
	(void) c;
	if (b.kn + HID_KBD_LEN > (int) sizeof b.kb && flush() < 0) return -1;
	memcpy(b.kb + b.kn, r, HID_KBD_LEN);
	b.kn += HID_KBD_LEN;
	return 0;
}

static int io_mouse(void *c, const unsigned char r[4])
{
	(void) c;
	if (flush() < 0) return -1;
	return put(D_MOUSE, r, HID_MOUSE_LEN);
}

static int io_cons(void *c, const unsigned char r[2])
{
	(void) c;
	if (flush() < 0) return -1;
	return put(D_CONS, r, HID_CONS_LEN);
}

static int io_wait(void *c, long ms)
{
	(void) c;
	if (flush() < 0) return -1;
	return nap(ms);
}

/* WAIT_FOR_BUTTON_PRESS: a line on standard input (the app sends one on
 * OK; at a terminal, Enter). */
static int io_button(void *c)
{
	struct pollfd p;
	char ch;
	int n;

	(void) c;
	if (flush() < 0) return -1;
	if (b.progress) prog("W\n");
	else out(2, "badusb: press Enter to go on\n", NULL, NULL, NULL);
	for (;;) {
		if (stop_now()) return -1;
		p.fd = 0;
		p.events = POLLIN;
		p.revents = 0;
		if (poll(&p, 1, STEP_MS) <= 0) continue;
		n = (int) read(0, &ch, 1);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) {
			b.why = "WAIT_FOR_BUTTON_PRESS: no one to press it";
			return -1;
		}
		if (ch == '\n') break;
	}
	if (b.progress) prog("R\n");
	return 0;
}

static int line_total;

static void io_line(void *c, int n)
{
	char t[12], u[12];

	(void) c;
	if (!b.progress) return;
	out5(1, "L ", num(t, n), " ", num(u, line_total), "\n");
}

static const struct dk_io io = { io_kbd, io_mouse, io_cons, io_wait,
				 io_button, io_line, NULL };

/* ---- main ---- */

static void usage(void)
{
	out(2, "usage: badusb [-n] [-p] [-l LAYOUT] [-d MS] FILE\n", NULL, NULL,
	    NULL);
	exit(2);
}

/* The host configured, else "H" and wait (in poll(), for a state change). */
static int host(void)
{
	struct pollfd p;
	int st, fd = dev(D_KBD);

	if (fd < 0) return -1;
	for (;;) {
		if (stop_now()) return -1;
		if (ioctl(fd, HIDIOCSTATE, &st) < 0) return 0;	/* write tells */
		if ((st & (HID_CONFIGURED | HID_SUSPENDED)) == HID_CONFIGURED) {
			if (b.hostwait) {
				b.hostwait = 0;
				prog("R\n");
			}
			return 0;
		}
		if (!b.hostwait) {
			b.hostwait = 1;
			prog("H\n");
		}
		p.fd = fd;
		p.events = POLLIN | POLLOUT;
		p.revents = 0;
		(void) poll(&p, 1, STEP_MS);
	}
}

int main(int argc, char **argv)
{
	unsigned char kl[HID_KL_SIZE];
	struct dk d;
	const char *layout = NULL;
	char t[12];
	long defdelay = 0;
	int i, fd, r, check = 0;

	for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != 0; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		if (strcmp(argv[i], "-n") == 0) check = 1;
		else if (strcmp(argv[i], "-p") == 0) b.progress = 1;
		else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc)
			layout = argv[++i];
		else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
			char *e;

			defdelay = strtol(argv[++i], &e, 10);
			if (*e != 0 || defdelay < 0 || defdelay > DK_MAXDELAY)
				usage();
		} else
			usage();
	}
	if (i != argc - 1) usage();
	b.file = argv[i];
	for (i = 0; i < ND; i++) b.fd[i] = -1;

	/* Block the stop signals: looked at between keys, never caught.
	 * SIGTERM and SIGPIPE get their default action first: ignored by
	 * the parent (libgm ignores SIGPIPE, exec keeps that) they would
	 * never be pending.  SIGINT and SIGHUP stay ignored if they are
	 * (a background job, nohup). */
	signal(SIGTERM, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);
	sigemptyset(&b.stops);
	sigaddset(&b.stops, SIGTERM);
	sigaddset(&b.stops, SIGINT);
	sigaddset(&b.stops, SIGHUP);
	sigaddset(&b.stops, SIGPIPE);
	(void) sigprocmask(SIG_BLOCK, &b.stops, (sigset_t *) NULL);

	if (hid_kl_load(kl, layout) < 0) {
		out(2, "badusb: no layout ", layout, " (or not 256 bytes)\n", NULL);
		if (b.progress) prog("E 0 no such layout\n");
		return 1;
	}
	if ((fd = open(b.file, O_RDONLY)) < 0) {
		out(2, "badusb: cannot open ", b.file, "\n", NULL);
		if (b.progress) prog("E 0 cannot open the script\n");
		return 1;
	}
	memset(&d, 0, sizeof d);
	d.kl = kl;
	d.defdelay = defdelay;
	if (check) {
		d.io = NULL;
		r = dk_run(&d, fd);
	} else {
		if (b.progress) {
			line_total = dk_count(fd);
			(void) lseek(fd, (off_t) 0, SEEK_SET);
		}
		d.io = &io;
		r = host() < 0 ? DK_STOP : dk_run(&d, fd);
		if (r == DK_DONE && flush() < 0) r = DK_STOP;
		if (r != DK_DONE) release_all();
	}
	close(fd);
	if (r == DK_DONE) {
		if (b.progress) {
			out(1, "D ", num(t, d.skipped), "\n", NULL);
		} else if (d.skipped > 0)
			out(2, "badusb: ", num(t, d.skipped),
			    " characters have no key in the layout\n", NULL);
		return 0;
	}
	if (r == DK_STOP && b.why == NULL) {
		prog("X\n");
		return 3;
	}
	if (r == DK_STOP) {			/* a device error */
		out5(2, "badusb: ", b.file, ": ", b.why, "\n");
		if (b.progress) out5(1, "E ", num(t, d.line), " ", b.why, "\n");
		return 1;
	}
	{
		char m[DK_ERRARG + 48];

		strcpy(m, d.err);
		if (d.errarg[0]) {
			strcat(m, ": ");
			strcat(m, d.errarg);
		}
		out5(2, "badusb: ", b.file, ": line ", num(t, d.line), ": ");
		out(2, m, "\n", NULL, NULL);
		if (b.progress) out5(1, "E ", num(t, d.line), " ", m, "\n");
	}
	return 1;
}
