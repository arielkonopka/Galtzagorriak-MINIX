/* SPDX-License-Identifier: GPL-3.0-or-later
 * usbremote - Teklatua ("keyboard" in Basque), the USB remote
 * (Galtzagorriak MINIX, spec R11.3.5, K.8): a window app.
 *
 * Usage:	usbremote	the app (started by the launcher: "=" in
 *				/etc/apps, its pipes on fd 3 and 4)
 *
 * The Flipper is a keyboard, a mouse and media keys for the computer at
 * its USB port; the app writes reports to the device files of R11.3.1
 * (/dev/hidkbd, /dev/hidmouse, /dev/hidcons) with libhid, and never
 * touches USB itself.  One LIVE window, "main", whose first row is the
 * page (Left/Right):
 *
 *	Keyboard  the desktop's keyboard on an input row: in a LIVE
 *		  window its text comes after every key, and the app types
 *		  what changed (a key report and its key-up per character,
 *		  Backspace for one deleted); after KMAX characters the row
 *		  starts empty again.  Layout (en-US built in, others from
 *		  the .kl files), Enter, Backspace, Tab, Esc rows.
 *	Numpad	  the same keyboard; digits and + - * / . are keypad keys
 *	Media	  volume up/down, mute, play/pause, next, previous
 *	Mouse	  "Mouse pad" opens a pad window (K.8): the arrows move
 *		  (1, 2, 4, 8 px per report while held, a report per
 *		  repeat of the desktop), OK clicks left, OK held 0.5 s
 *		  right, Back held with Up/Down turns the wheel; a lone
 *		  Back leaves the pad
 *	Clicker	  rate (1-50 clicks/s) and button; Start/Stop.  The app
 *		  clicks from its own poll() timeout while the page is open
 *	Jiggler	  interval and pattern; Start runs the jiggle command (spec
 *		  R11.3.6) in the background, so it costs no window RAM and
 *		  goes on after Back; Stop runs jiggle -k
 *
 * The last line of every page is the USB state (HIDIOCSTATE): ready, no
 * host, busy (another USB persona, R11.3.7), no access.
 *
 * No stdio, no malloc, no signal handler (libgm ignores SIGPIPE), so no
 * MM gap; the layout is a 256 B buffer on main's stack (R11.3.2).  Idle
 * it sleeps in read(): 0 wakeups; it wakes on its own only while the
 * clicker runs.  Nothing is saved: 0 flash writes, the settings are the
 * defaults again at the next start.
 *
 * The launcher starts it as the user usbremote (group usb, the only one
 * besides root that may open the HID files; /etc/apps
 * "Teklatua:usbremote:@usb:=/usr/bin/usbremote").
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <minix/hid.h>
#include "gm.h"
#include "libhid.h"

#define P_KBD		0		/* pages, in the page row's order */
#define P_NUM		1
#define P_MEDIA		2
#define P_MOUSE		3
#define P_CLICK		4
#define P_JIG		5
static const char pages[] = "Keyboard|Numpad|Media|Mouse|Clicker|Jiggler";

#define F_KBD		0		/* the device files */
#define F_MOUSE		1
#define F_CONS		2
static const char devs[3][14] = { "/dev/hidkbd", "/dev/hidmouse",
				  "/dev/hidcons" };

/* HID keyboard usages that are no character of a layout */
#define U_ENTER		0x28
#define U_ESC		0x29
#define U_BKSP		0x2A
#define U_TAB		0x2B

#define LONG_OK		500		/* ms: OK held in the pad, right click */
#define KMAX		48		/* the input row's text, then empty */

static const char layouts[] = "en-US|de-DE";	/* the layout row */

static struct {
	signed char fd[3];		/* open device files, -1 */
	char page;			/* P_* */
	char in_pad;			/* the pad window is up */
	char layout;			/* index in layouts[] */
	char kl_ok;			/* that layout is loaded */
	char clicking;			/* the clicker runs */
	char btn;			/* clicker button: 0 left, 1 right, 2 middle */
	char pat;			/* jiggler pattern: 0 tiny, 1 zero */
	unsigned char rate;		/* clicks per second */
	unsigned short every;		/* jiggler interval, s */
	/* the pad */
	unsigned char held;		/* direction keys down, KEY bits */
	unsigned char reps;		/* repeats of the moving key */
	char back_down, ok_down, ok_right;
	long ok_t;			/* ticks at OK's press */
} s;

static const char *note;		/* the last line, when not the state */
static char last[KMAX + 1];		/* the input row's text, as typed */

static long ticks(void)
{
	struct tms t;

	return (long) times(&t);
}

/*===========================================================================*
 *				Reports					     *
 *===========================================================================*/
static int dev(int i)
{
	if (s.fd[i] < 0) s.fd[i] = (signed char) open(devs[i], O_WRONLY);
	return s.fd[i];
}

/* A write of whole reports; on an error the reason on the last line. */
static int put(int i, const unsigned char *r, int n)
{
	int fd = dev(i), w;

	if (fd < 0) {
		note = errno == EBUSY ? "USB busy: another persona"
		     : errno == EACCES ? "No access (group usb)"
		     : "No HID device";
		return -1;
	}
	while ((w = write(fd, r, (unsigned) n)) < 0 && errno == EINTR) ;
	if (w == n) {
		note = NULL;
		return 0;
	}
	note = errno == EAGAIN ? "USB: no host (cable?)"
	     : errno == EBUSY ? "USB busy: another persona" : "USB write failed";
	return -1;
}

/* A key with its modifiers, then all keys up: one write. */
static void key(unsigned mod, unsigned usage)
{
	unsigned char r[2 * HID_KBD_LEN];

	hid_kbd(r, mod, usage);
	hid_kbd(r + HID_KBD_LEN, 0, 0);
	(void) put(F_KBD, r, sizeof r);
}

static void mouse(unsigned btn, int dx, int dy, int wheel)
{
	unsigned char r[HID_MOUSE_LEN];

	hid_mouse(r, btn, dx, dy, wheel);
	(void) put(F_MOUSE, r, sizeof r);
}

/* A click: the button down, then up, in one write. */
static void click(unsigned btn)
{
	unsigned char r[2 * HID_MOUSE_LEN];

	hid_mouse(r, btn, 0, 0, 0);
	hid_mouse(r + HID_MOUSE_LEN, 0, 0, 0, 0);
	(void) put(F_MOUSE, r, sizeof r);
}

static void media(unsigned usage)
{
	unsigned char r[2 * HID_CONS_LEN];

	hid_cons(r, usage);
	hid_cons(r + HID_CONS_LEN, 0);
	(void) put(F_CONS, r, sizeof r);
}

/* One character: through the layout; on the Numpad page the digits and
 * + - * / . are keypad keys. */
static void type1(const unsigned char *kl, int c)
{
	static const char kp[] = "/*-+\n1234567890.";	/* 0x54 ... 0x63 */
	const char *p;
	unsigned k;

	if (s.page == P_NUM && c != 0 && (p = strchr(kp, c)) != NULL) {
		key(0, 0x54 + (unsigned) (p - kp));
		return;
	}
	if ((k = hid_key(kl, c)) == 0) {
		note = "No such key in the layout";
		return;
	}
	key(k >> 8, k & 0xFF);
}

/* The input row's new text (a LIVE submit): Backspace for each character
 * gone since the last one, then the new ones.  1: full, show it empty. */
static int typed(const unsigned char *kl, const char *v)
{
	int p, n;

	for (p = 0; last[p] != 0 && last[p] == v[p]; p++) ;
	for (n = (int) strlen(last); n > p; n--) key(0, U_BKSP);
	for (n = p; v[n] != 0 && n < KMAX; n++) {
		type1(kl, (unsigned char) v[n]);
		last[n] = v[n];
	}
	last[n] = 0;
	return n >= KMAX;
}

/*===========================================================================*
 *				Windows					     *
 *===========================================================================*/
static char *num(char *t, unsigned v)
{
	char *p = t + 7;

	*p = 0;
	do *--p = (char) ('0' + v % 10); while ((v /= 10) > 0 && p > t);
	return p;
}

/* The USB state for the last line. */
static const char *state(void)
{
	int i, st;

	if (note != NULL) return note;
	for (i = 0; i < 3 && s.fd[i] < 0; i++) ;
	if (i == 3) {			/* none open yet: the keyboard's */
		if (dev(F_KBD) < 0)
			return errno == EBUSY ? "USB busy: another persona"
			     : errno == EACCES ? "No access (group usb)"
			     : "No HID device";
		i = F_KBD;
	}
	if (ioctl(s.fd[i], HIDIOCSTATE, &st) < 0) return "USB: ?";
	if (HID_PERSONA(st) != HIDP_NONE) return "USB busy: another persona";
	if (!(st & HID_ATTACHED)) return "USB: no cable";
	if (st & HID_SUSPENDED) return "USB: host asleep";
	if (!(st & HID_CONFIGURED)) return "USB: waiting for the host";
	return "USB: ready";
}

static void w_main(void)
{
	char a[8];

	gm_win_live("main", "list", "Teklatua", 1);
	gm_enum("page", "Page", s.page, pages);
	switch (s.page) {
	case P_KBD:
		gm_enum("layout", "Layout", s.layout, layouts);
		/* FALLTHROUGH */
	case P_NUM:
		gm_input("kbd", "Type", "", KMAX);
		last[0] = 0;
		gm_action("enter", "Enter");
		gm_action("bksp", "Backspace");
		if (s.page == P_KBD) {
			gm_action("tab", "Tab");
			gm_action("esc", "Esc");
		}
		break;
	case P_MEDIA:
		gm_action("volup", "Volume +");
		gm_action("voldn", "Volume -");
		gm_action("mute", "Mute");
		gm_action("play", "Play/Pause");
		gm_action("next", "Next track");
		gm_action("prev", "Previous track");
		break;
	case P_MOUSE:
		gm_action("pad", "Mouse pad");
		gm_line("Arrows move, OK click,");
		gm_line("hold OK: right click,");
		gm_line("Back+Up/Down: wheel");
		break;
	case P_CLICK:
		gm_int("rate", "Rate", 1, 50, 1, s.rate, "/s");
		gm_enum("btn", "Button", s.btn, "Left|Right|Middle");
		gm_action("run", s.clicking ? "Stop" : "Start");
		if (s.clicking) {
			char l[24];

			strcpy(l, "Clicking ");
			strcat(l, num(a, s.rate));
			strcat(l, "/s");
			gm_line(l);
		}
		break;
	case P_JIG:
		gm_int("every", "Interval", 5, 3600, 5, s.every, "s");
		gm_enum("pat", "Pattern", s.pat, "tiny|zero");
		gm_action("jstart", "Start jiggle");
		gm_action("jstop", "Stop jiggle");
		gm_line("Runs without this window");
		break;
	}
	gm_line(state());
	if (gm_show() < 0) _exit(0);	/* the desktop is gone */
}

static void w_pad(void)
{
	gm_win_pad("mouse", "Mouse", 10);
	gm_line("Arrows: move");
	gm_line("OK: click  Hold OK: right");
	gm_line("Back + Up/Down: wheel");
	gm_line("Back alone: leave");
	gm_line(state());
	if (gm_show() < 0) _exit(0);
}

static void redraw(void)
{
	if (s.in_pad) w_pad();
	else w_main();
}

/*===========================================================================*
 *				The jiggler				     *
 *===========================================================================*/
/* jiggle (R11.3.6, its own package) in the background: the app may end,
 * jiggle goes on (a grandchild, so INIT reaps it), without the pipes. */
static int run_jiggle(char **argv)
{
	static const char p1[] = "/usr/bin/jiggle", p2[] = "/bin/jiggle";
	const char *path = access(p1, X_OK) == 0 ? p1
			 : access(p2, X_OK) == 0 ? p2 : NULL;
	pid_t pid;
	int st, fd;

	if (path == NULL) {
		note = "jiggle is not installed";
		return -1;
	}
	if ((pid = fork()) < 0) {
		note = "Cannot start jiggle";
		return -1;
	}
	if (pid == 0) {
		if (fork() != 0) _exit(0);
		for (fd = 3; fd < 20; fd++) close(fd);
		execv(path, argv);
		_exit(127);
	}
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
	note = NULL;
	return 0;
}

static void jiggle(int start)
{
	char a[8], *argv[6];

	argv[0] = "jiggle";
	if (start) {
		argv[1] = "-i";
		argv[2] = num(a, s.every);
		argv[3] = "-p";
		argv[4] = s.pat ? "zero" : "tiny";
		argv[5] = NULL;
	} else {
		argv[1] = "-k";
		argv[2] = NULL;
	}
	if (run_jiggle(argv) == 0)
		note = start ? "Jiggle runs (J on the desktop)" : "Jiggle stopped";
}

/*===========================================================================*
 *				Events					     *
 *===========================================================================*/
static void stop_clicker(void)
{
	s.clicking = 0;
}

static void ok(const char *win, const char *k)
{
	static const char mk[] = "volup\0voldn\0mute\0\0play\0\0next\0\0prev\0";
	static const unsigned short mu[6] = { HID_CC_VOL_UP, HID_CC_VOL_DOWN,
		HID_CC_MUTE, HID_CC_PLAY_PAUSE, HID_CC_NEXT, HID_CC_PREV };
	int i;

	if (strcmp(win, "main") != 0) return;
	for (i = 0; i < 6; i++)
		if (strcmp(k, mk + 6 * i) == 0) {
			media(mu[i]);
			break;
		}
	if (i < 6) ;
	else if (strcmp(k, "enter") == 0)
		key(0, s.page == P_NUM ? 0x58 : U_ENTER);
	else if (strcmp(k, "bksp") == 0) key(0, U_BKSP);
	else if (strcmp(k, "tab") == 0) key(0, U_TAB);
	else if (strcmp(k, "esc") == 0) key(0, U_ESC);
	else if (strcmp(k, "run") == 0) s.clicking = (char) !s.clicking;
	else if (strcmp(k, "jstart") == 0) jiggle(1);
	else if (strcmp(k, "jstop") == 0) jiggle(0);
	else if (strcmp(k, "pad") == 0) {
		s.in_pad = 1;
		s.held = 0;
		s.back_down = s.ok_down = 0;
	}
	redraw();
}

static int load(unsigned char *kl)
{
	char name[HID_KL_NAME];
	const char *o = layouts;
	int i, n;

	for (i = 0; i < s.layout; i++) o = strchr(o, '|') + 1;
	for (n = 0; o[n] != 0 && o[n] != '|' && n < HID_KL_NAME - 1; n++)
		name[n] = o[n];
	name[n] = 0;
	if (hid_kl_load(kl, name) < 0) {
		(void) hid_kl_load(kl, HID_KL_US);
		note = "Layout not found: en-US";
		s.layout = 0;
		return -1;
	}
	return 0;
}

/* submit main KEY=VALUE ...: the LIVE rows, at once. */
static void values(char **f, unsigned char *kl)
{
	char *k, *v;
	int i, page = s.page, show = 0;

	for (i = 2; f[i] != NULL; i++) {
		k = gm_kv(f[i], &v);
		if (strcmp(k, "kbd") == 0) {	/* not shown again (that
						 * closes the keyboard)
						 * unless full */
			if (typed(kl, v)) show = 1;
		} else if (strcmp(k, "page") == 0) {
			s.page = (char) (*v - '0');
			if (s.page < 0 || s.page > P_JIG) s.page = 0;
		} else if (strcmp(k, "layout") == 0) {
			s.layout = (char) (*v == '1');
			(void) load(kl);
			show = 1;
		} else if (strcmp(k, "rate") == 0) {
			int r = 0;

			while (*v >= '0' && *v <= '9') r = r * 10 + *v++ - '0';
			s.rate = (unsigned char) (r < 1 ? 1 : r > 50 ? 50 : r);
			if (s.clicking) show = 1;
		} else if (strcmp(k, "btn") == 0) {
			s.btn = (char) (*v - '0');
			if (s.btn < 0 || s.btn > 2) s.btn = 0;
		} else if (strcmp(k, "every") == 0) {
			unsigned r = 0;

			while (*v >= '0' && *v <= '9') r = r * 10 + (unsigned) (*v++ - '0');
			s.every = (unsigned short) (r < 5 ? 5 : r > 3600 ? 3600 : r);
		} else if (strcmp(k, "pat") == 0)
			s.pat = (char) (*v == '1');
	}
	if (s.page != page) {			/* another page */
		if (s.page != P_CLICK) stop_clicker();
		note = NULL;
		show = 1;
	}
	if (show) w_main();
}

/* Mouse step of a held direction: 1 px, then 2, 4, 8 while held. */
static int step(void)
{
	return s.reps < 8 ? 1 : s.reps < 16 ? 2 : s.reps < 24 ? 4 : 8;
}

static void move(void)
{
	int d = step(), dx = 0, dy = 0;

	if (s.held & 0x01) dy -= d;
	if (s.held & 0x02) dy += d;
	if (s.held & 0x04) dx -= d;
	if (s.held & 0x08) dx += d;
	if (dx != 0 || dy != 0) mouse(0, dx, dy, 0);
}

/* key KEY press|release|repeat in the pad. */
static void pad(const char *k, const char *what)
{
	static const char names[] = "up\0\0\0\0down\0\0left\0\0right\0";
	int press = strcmp(what, "press") == 0, rep = strcmp(what, "repeat") == 0;
	int i;

	if (strcmp(k, "back") == 0) {
		s.back_down = (char) (press || rep);
		return;
	}
	if (strcmp(k, "ok") == 0) {
		if (press) {
			s.ok_down = 1;
			s.ok_right = 0;
			s.ok_t = ticks();
		} else if (rep && s.ok_down && !s.ok_right
			   && ticks() - s.ok_t >= (LONG_OK * CLK_TCK + 999) / 1000) {
			s.ok_right = 1;
			click(HID_BTN_RIGHT);
		} else if (!press && !rep && s.ok_down) {
			if (!s.ok_right) click(HID_BTN_LEFT);
			s.ok_down = 0;
		}
		return;
	}
	for (i = 0; i < 4 && strcmp(k, names + 6 * i) != 0; i++) ;
	if (i == 4) return;
	if (s.back_down && i < 2) {		/* the wheel */
		if (press || rep) mouse(0, 0, 0, i == 0 ? 1 : -1);
		return;
	}
	if (press) {
		s.held |= (unsigned char) (1 << i);
		s.reps = 0;
		move();
	} else if (rep) {
		if (s.reps < 255) s.reps++;
		move();
	} else
		s.held &= (unsigned char) ~(1 << i);
}

int main(int argc, char **argv)
{
	char buf[128], ev[128], *f[GM_FIELDS];
	unsigned char kl[HID_KL_SIZE];	/* the layout (R11.3.2) */

	(void) argv;
	s.fd[0] = s.fd[1] = s.fd[2] = -1;
	s.rate = 10;
	s.every = 30;
	if (argc != 1 || gm_init(buf, sizeof buf) < 0) {
		(void) write(2, "usbremote: start Teklatua from the launcher\n", 44);
		return 1;
	}
	(void) hid_kl_load(kl, HID_KL_US);
	w_main();
	for (;;) {
		/* the clicker: a click when poll() waited 1/rate s for an
		 * event in vain; else read() waits: 0 wakeups */
		if (s.clicking && !s.in_pad && gm_wait(ev, 1000 / s.rate) == 0) {
			click(s.btn == 0 ? HID_BTN_LEFT : s.btn == 1
			      ? HID_BTN_RIGHT : HID_BTN_MIDDLE);
			if (note != NULL) {	/* no host: it stops */
				stop_clicker();
				w_main();
			}
			continue;
		}
		switch (gm_event(ev, sizeof ev, f, GM_FIELDS)) {
		case GM_OK:
			if (f[1] != NULL && f[2] != NULL) ok(f[1], f[2]);
			break;
		case GM_SUBMIT:
			if (f[1] != NULL && strcmp(f[1], "main") == 0)
				values(f, kl);
			break;
		case GM_KEY:
			if (s.in_pad && f[1] != NULL && f[2] != NULL)
				pad(f[1], f[2]);
			break;
		case GM_BACK:		/* out of the pad: all buttons up */
			if (s.in_pad) {
				s.in_pad = 0;
				s.held = 0;
				if (s.ok_down) mouse(0, 0, 0, 0);
				s.ok_down = 0;
			}
			break;
		case GM_PAGE:
		case GM_REDRAW:
			redraw();
			break;
		case GM_CLOSE:
			return 0;
		}
	}
}
