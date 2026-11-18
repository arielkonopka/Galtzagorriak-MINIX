/* SPDX-License-Identifier: GPL-3.0-or-later
 * ducky - the Bad USB script engine (spec R11.3.3); see ducky.h.
 *
 * The language follows the stock Flipper firmware's Bad USB engine
 * (applications/main/bad_usb/helpers/ducky_script*.c, GPLv3): the same
 * commands, key names, media names and rules, here without a thread, a
 * FuriString or a whole-file buffer.  Every table is const with its names
 * in fixed arrays, not pointers: under the window apps' and commands'
 * -mno-pic-data-is-text-relative a pointer table would be relocated into
 * RAM; this way all of it stays in flash.
 */
#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include <minix/hid.h>
#include "libhid.h"
#include "ducky.h"

/* ---- tables ---- */

/* Keys by name: modifiers (mod), keys (usage), mouse buttons (btn). */
struct dkey {
	char n[13];
	unsigned char mod, usage, btn;
};

#define LC	HID_MOD_LCTRL
#define LS	HID_MOD_LSHIFT
#define LA	HID_MOD_LALT
#define LG	HID_MOD_LGUI

static const struct dkey keys[] = {
	{ "CTRL-ALT", LC | LA, 0, 0 },
	{ "CTRL-SHIFT", LC | LS, 0, 0 },
	{ "ALT-SHIFT", LA | LS, 0, 0 },
	{ "ALT-GUI", LA | LG, 0, 0 },
	{ "GUI-SHIFT", LG | LS, 0, 0 },
	{ "GUI-CTRL", LG | LC, 0, 0 },
	{ "CTRL", LC, 0, 0 },
	{ "CONTROL", LC, 0, 0 },
	{ "SHIFT", LS, 0, 0 },
	{ "ALT", LA, 0, 0 },
	{ "GUI", LG, 0, 0 },
	{ "WINDOWS", LG, 0, 0 },
	{ "DOWNARROW", 0, 0x51, 0 },
	{ "DOWN", 0, 0x51, 0 },
	{ "LEFTARROW", 0, 0x50, 0 },
	{ "LEFT", 0, 0x50, 0 },
	{ "RIGHTARROW", 0, 0x4F, 0 },
	{ "RIGHT", 0, 0x4F, 0 },
	{ "UPARROW", 0, 0x52, 0 },
	{ "UP", 0, 0x52, 0 },
	{ "ENTER", 0, 0x28, 0 },
	{ "BREAK", 0, 0x48, 0 },
	{ "PAUSE", 0, 0x48, 0 },
	{ "CAPSLOCK", 0, 0x39, 0 },
	{ "DELETE", 0, 0x4C, 0 },
	{ "BACKSPACE", 0, 0x2A, 0 },
	{ "END", 0, 0x4D, 0 },
	{ "ESC", 0, 0x29, 0 },
	{ "ESCAPE", 0, 0x29, 0 },
	{ "HOME", 0, 0x4A, 0 },
	{ "INSERT", 0, 0x49, 0 },
	{ "NUMLOCK", 0, 0x53, 0 },
	{ "PAGEUP", 0, 0x4B, 0 },
	{ "PAGEDOWN", 0, 0x4E, 0 },
	{ "PRINTSCREEN", 0, 0x46, 0 },
	{ "SCROLLLOCK", 0, 0x47, 0 },
	{ "SPACE", 0, 0x2C, 0 },
	{ "TAB", 0, 0x2B, 0 },
	{ "MENU", 0, 0x65, 0 },
	{ "APP", 0, 0x65, 0 },
	{ "F1", 0, 0x3A, 0 }, { "F2", 0, 0x3B, 0 }, { "F3", 0, 0x3C, 0 },
	{ "F4", 0, 0x3D, 0 }, { "F5", 0, 0x3E, 0 }, { "F6", 0, 0x3F, 0 },
	{ "F7", 0, 0x40, 0 }, { "F8", 0, 0x41, 0 }, { "F9", 0, 0x42, 0 },
	{ "F10", 0, 0x43, 0 }, { "F11", 0, 0x44, 0 }, { "F12", 0, 0x45, 0 },
	{ "F13", 0, 0x68, 0 }, { "F14", 0, 0x69, 0 }, { "F15", 0, 0x6A, 0 },
	{ "F16", 0, 0x6B, 0 }, { "F17", 0, 0x6C, 0 }, { "F18", 0, 0x6D, 0 },
	{ "F19", 0, 0x6E, 0 }, { "F20", 0, 0x6F, 0 }, { "F21", 0, 0x70, 0 },
	{ "F22", 0, 0x71, 0 }, { "F23", 0, 0x72, 0 }, { "F24", 0, 0x73, 0 },
	{ "LEFTCLICK", 0, 0, HID_BTN_LEFT },
	{ "LEFT_CLICK", 0, 0, HID_BTN_LEFT },
	{ "RIGHTCLICK", 0, 0, HID_BTN_RIGHT },
	{ "RIGHT_CLICK", 0, 0, HID_BTN_RIGHT },
	{ "MIDDLECLICK", 0, 0, HID_BTN_MIDDLE },
	{ "MIDDLE_CLICK", 0, 0, HID_BTN_MIDDLE },
	{ "WHEELCLICK", 0, 0, HID_BTN_MIDDLE },
	{ "WHEEL_CLICK", 0, 0, HID_BTN_MIDDLE },
};
#define NKEYS	(int) (sizeof keys / sizeof *keys)

/* MEDIA names: consumer usages (HID usage page 0x0C). */
static const struct dmedia {
	char n[12];
	unsigned short usage;
} media[] = {
	{ "POWER", 0x0030 }, { "REBOOT", 0x0031 }, { "SLEEP", 0x0032 },
	{ "LOGOFF", 0x019C }, { "EXIT", 0x0204 }, { "HOME", 0x0223 },
	{ "BACK", 0x0224 }, { "FORWARD", 0x0225 }, { "REFRESH", 0x0227 },
	{ "SNAPSHOT", 0x0065 }, { "PLAY", 0x00B0 }, { "PAUSE", 0x00B1 },
	{ "PLAY_PAUSE", HID_CC_PLAY_PAUSE }, { "NEXT_TRACK", HID_CC_NEXT },
	{ "PREV_TRACK", HID_CC_PREV }, { "STOP", HID_CC_STOP },
	{ "EJECT", 0x00B8 }, { "MUTE", HID_CC_MUTE },
	{ "VOLUME_UP", HID_CC_VOL_UP }, { "VOLUME_DOWN", HID_CC_VOL_DOWN },
	{ "FN", 0x029D }, { "BRIGHT_UP", HID_CC_BRIGHT_UP },
	{ "BRIGHT_DOWN", HID_CC_BRIGHT_DOWN },
};
#define NMEDIA	(int) (sizeof media / sizeof *media)
#define CC_GLOBE	0x029D		/* the Apple Fn/Globe key */

enum {
	C_REM, C_ID, C_DELAY, C_STRING, C_STRINGLN, C_DEFDELAY, C_STRDELAY,
	C_DEFSTRDELAY, C_REPEAT, C_SYSRQ, C_ALTCHAR, C_ALTSTRING, C_HOLD,
	C_RELEASE, C_WAITBTN, C_MEDIA, C_GLOBE, C_MOVE, C_SCROLL
};

static const struct dcmd {
	char n[22];
	unsigned char c;
} cmds[] = {
	{ "REM", C_REM }, { "ID", C_ID }, { "DELAY", C_DELAY },
	{ "STRING", C_STRING }, { "STRINGLN", C_STRINGLN },
	{ "DEFAULT_DELAY", C_DEFDELAY }, { "DEFAULTDELAY", C_DEFDELAY },
	{ "STRING_DELAY", C_STRDELAY }, { "STRINGDELAY", C_STRDELAY },
	{ "DEFAULT_STRING_DELAY", C_DEFSTRDELAY },
	{ "DEFAULTSTRINGDELAY", C_DEFSTRDELAY },
	{ "REPEAT", C_REPEAT }, { "SYSRQ", C_SYSRQ },
	{ "ALTCHAR", C_ALTCHAR }, { "ALTSTRING", C_ALTSTRING },
	{ "ALTCODE", C_ALTSTRING }, { "HOLD", C_HOLD },
	{ "RELEASE", C_RELEASE }, { "WAIT_FOR_BUTTON_PRESS", C_WAITBTN },
	{ "MEDIA", C_MEDIA }, { "GLOBE", C_GLOBE },
	{ "MOUSEMOVE", C_MOVE }, { "MOUSE_MOVE", C_MOVE },
	{ "MOUSESCROLL", C_SCROLL }, { "MOUSE_SCROLL", C_SCROLL },
};
#define NCMDS	(int) (sizeof cmds / sizeof *cmds)

#define KP_0		0x62		/* keypad 0; 1-9 are 0x59-0x61 */
#define K_ENTER		0x28
#define K_PRTSC		0x46

/* ---- small helpers ---- */

static int err(struct dk *d, const char *what, const char *arg, int n)
{
	int i;

	d->err = what;
	for (i = 0; arg != NULL && i < n && i < DK_ERRARG - 1 && arg[i] != 0; i++)
		d->errarg[i] = arg[i];
	d->errarg[i] = 0;
	return DK_ERROR;
}

/* The length of the word at s (up to a space, a tab or the end). */
static int wlen(const char *s)
{
	int n = 0;

	while (s[n] != 0 && s[n] != ' ' && s[n] != '\t') n++;
	return n;
}

static const char *skipsp(const char *s)
{
	while (*s == ' ' || *s == '\t') s++;
	return s;
}

static int eq(const char *name, const char *w, int n)
{
	return strncmp(name, w, (size_t) n) == 0 && name[n] == 0;
}

/* A decimal number, the whole word (with lo < 0 a sign is allowed);
 * 0 and *v, or -1. */
static int number(const char *w, int n, long lo, long hi, long *v)
{
	long x = 0;
	int i = 0, neg = 0;

	if (n > 0 && (w[0] == '-' || w[0] == '+') && lo < 0) {
		neg = w[0] == '-';
		i = 1;
	}
	if (i >= n) return -1;
	for (; i < n; i++) {
		if (w[i] < '0' || w[i] > '9') return -1;
		if (x > 99999999L) return -1;	/* (no overflow) */
		x = x * 10 + (w[i] - '0');
	}
	if (neg) x = -x;
	if (x < lo || x > hi) return -1;
	*v = x;
	return 0;
}

/* The one number argument of a command. */
static int arg_num(struct dk *d, const char *a, long lo, long hi, long *v)
{
	int n;

	a = skipsp(a);
	n = wlen(a);
	if (n == 0) return err(d, "number missing", NULL, 0);
	if (number(a, n, lo, hi, v) < 0 || *skipsp(a + n) != 0)
		return err(d, "bad number", a, n);
	return 0;
}

static const struct dkey *key_name(const char *w, int n)
{
	int i;

	for (i = 0; i < NKEYS; i++)
		if (eq(keys[i].n, w, n)) return &keys[i];
	return NULL;
}

/* ---- reports ---- */

static int kbd(struct dk *d, unsigned mod, const unsigned char *k, int nk)
{
	unsigned char r[8];
	int i, j = 2;

	if (d->io == NULL) return 0;
	memset(r, 0, sizeof r);
	r[0] = (unsigned char) (d->mod | mod);
	for (i = 0; i < 6; i++)
		if (d->key[i] != 0) r[j++] = d->key[i];
	for (i = 0; i < nk && j < 8; i++)
		if (k[i] != 0) r[j++] = k[i];
	return d->io->kbd(d->io->c, r) < 0 ? DK_STOP : 0;
}

/* Press mod + keys, then let them go (what HOLD holds stays). */
static int tap(struct dk *d, unsigned mod, const unsigned char *k, int nk)
{
	if (kbd(d, mod, k, nk) < 0 || kbd(d, 0, NULL, 0) < 0) return DK_STOP;
	return 0;
}

static int mouse(struct dk *d, int dx, int dy, int wheel)
{
	unsigned char r[4];

	if (d->io == NULL) return 0;
	hid_mouse(r, d->btn, dx, dy, wheel);
	return d->io->mouse(d->io->c, r) < 0 ? DK_STOP : 0;
}

static int cons(struct dk *d, unsigned usage)
{
	unsigned char r[2];

	if (d->io == NULL) return 0;
	hid_cons(r, usage);
	return d->io->cons(d->io->c, r) < 0 ? DK_STOP : 0;
}

static int wait_ms(struct dk *d, long ms)
{
	if (d->io == NULL) return 0;
	return d->io->wait(d->io->c, ms) < 0 ? DK_STOP : 0;
}

int dk_release(struct dk *d)
{
	int r = 0;

	if (d->io == NULL) return 0;
	if (d->mod != 0 || d->key[0] || d->key[1] || d->key[2] || d->key[3]
	    || d->key[4] || d->key[5]) {
		d->mod = 0;
		memset(d->key, 0, sizeof d->key);
		r = kbd(d, 0, NULL, 0);
	}
	if (d->btn != 0) {
		d->btn = 0;
		if (mouse(d, 0, 0, 0) < 0) r = DK_STOP;
	}
	return r;
}

/* ---- commands ---- */

/* Type a UTF-8 string; delay ms between the characters. */
static int type(struct dk *d, const char *s, long delay)
{
	unsigned char u;
	unsigned k;
	long c;
	int first = 1;

	while ((c = hid_utf8(&s)) != 0) {
		if ((k = hid_key(d->kl, c)) == 0) {
			d->skipped++;
			continue;
		}
		if (!first && delay > 0 && wait_ms(d, delay) < 0) return DK_STOP;
		first = 0;
		u = (unsigned char) (k & 0xFF);
		if (tap(d, k >> 8, &u, 1) < 0) return DK_STOP;
	}
	return 0;
}

/* Alt + the decimal digits of v on the keypad. */
static int altchar(struct dk *d, long v, int lead0)
{
	unsigned char k, dig[8];
	int n = 0;

	do dig[n++] = (unsigned char) (v % 10); while ((v /= 10) > 0 && n < 7);
	if (lead0) dig[n++] = 0;
	if (kbd(d, LA, NULL, 0) < 0) return DK_STOP;
	while (n > 0) {
		k = dig[--n] == 0 ? KP_0 : (unsigned char) (0x58 + dig[n]);
		if (kbd(d, LA, &k, 1) < 0 || kbd(d, LA, NULL, 0) < 0)
			return DK_STOP;
	}
	return kbd(d, 0, NULL, 0);
}

/* ALTSTRING: every character as an Alt code.  ASCII as the stock engine
 * (its number); others with a leading 0 up to 255 (Windows: the ANSI
 * code page), else their Unicode number. */
static int altstring(struct dk *d, const char *s)
{
	long c;

	while ((c = hid_utf8(&s)) != 0) {
		if (c < 0) {
			d->skipped++;
			continue;
		}
		if (altchar(d, c, c >= 128 && c <= 255) < 0) return DK_STOP;
	}
	return 0;
}

/* One key word for HOLD, RELEASE, SYSRQ, GLOBE: a name or a character
 * (its usage only, as the stock engine).  0, or DK_ERROR. */
static int one_key(struct dk *d, const char *a, struct dkey *k)
{
	const struct dkey *kn;
	int n;

	a = skipsp(a);
	n = wlen(a);
	if (n == 0) return err(d, "key missing", NULL, 0);
	if (*skipsp(a + n) != 0) return err(d, "one key only", a, n);
	if ((kn = key_name(a, n)) != NULL) {
		*k = *kn;
		return 0;
	}
	memset(k, 0, sizeof *k);
	if (n == 1 && (k->usage = (unsigned char) (hid_key(d->kl,
			(unsigned char) a[0]) & 0xFF)) != 0)
		return 0;
	return err(d, "unknown key", a, n);
}

static int hold(struct dk *d, const struct dkey *k, int on)
{
	int i, free_i = -1;

	if (k->btn != 0) {
		d->btn = (unsigned char) (on ? d->btn | k->btn : d->btn & ~k->btn);
		return mouse(d, 0, 0, 0);
	}
	if (k->usage != 0) {
		for (i = 5; i >= 0; i--) {
			if (d->key[i] == k->usage) break;
			if (d->key[i] == 0) free_i = i;
		}
		if (on && i < 0) {
			if (free_i < 0)
				return err(d, "more than 6 keys held", NULL, 0);
			d->key[free_i] = k->usage;
		} else if (!on && i >= 0)
			d->key[i] = 0;
	}
	d->mod = (unsigned char) (on ? d->mod | k->mod : d->mod & ~k->mod);
	return kbd(d, 0, NULL, 0);
}

/* A line that is no command: modifiers, then at most six keys (names, or
 * after a modifier one character each), or mouse buttons. */
static int combo(struct dk *d, const char *s)
{
	const struct dkey *k;
	unsigned char u[6], btn = 0, mod = 0;
	int n, nk = 0, r;

	for (s = skipsp(s); *s != 0; s = skipsp(s + n)) {
		n = wlen(s);
		if ((k = key_name(s, n)) != NULL) {
			mod |= k->mod;
			btn |= k->btn;
			if (k->usage != 0) {
				if (nk == 6) return err(d, "more than 6 keys", s, n);
				u[nk++] = k->usage;
			}
		} else if (n == 1 && mod != 0 && hid_key(d->kl,
				(unsigned char) s[0]) != 0) {
			if (nk == 6) return err(d, "more than 6 keys", s, n);
			u[nk++] = (unsigned char) (hid_key(d->kl,
						(unsigned char) s[0]) & 0xFF);
		} else
			return err(d, "unknown command or key", s, n);
	}
	if (btn != 0) {			/* a click, modifiers held with it */
		if (kbd(d, mod, u, nk) < 0) return DK_STOP;
		d->btn |= btn;
		r = mouse(d, 0, 0, 0);
		d->btn &= (unsigned char) ~btn;
		if (r < 0 || mouse(d, 0, 0, 0) < 0) return DK_STOP;
		return kbd(d, 0, NULL, 0);
	}
	return tap(d, mod, u, nk);
}

/* ID VID:PID [Manufacturer:Product]: checked; VID/PID and the strings
 * are not applied here (spec R11.3.3: usbmode re-enumerates). */
static int id_line(struct dk *d, const char *a)
{
	int i, n;

	a = skipsp(a);
	n = wlen(a);
	if (n != 9 || a[4] != ':') return err(d, "ID wants VID:PID", a, n);
	for (i = 0; i < 9; i++)
		if (i != 4 && !((a[i] >= '0' && a[i] <= '9')
			      || (a[i] >= 'a' && a[i] <= 'f')
			      || (a[i] >= 'A' && a[i] <= 'F')))
			return err(d, "ID wants VID:PID", a, n);
	return 0;
}

/* One line (no newline, no NUL inside).  0, DK_ERROR, DK_STOP, or > 0:
 * REPEAT that many times (d->line is the REPEAT line). */
static int line(struct dk *d, char *s, int *delay_after)
{
	const char *a;
	struct dkey k;
	long v, w;
	int i, n, c;

	*delay_after = 0;
	s = (char *) skipsp(s);
	if (*s == 0) return 0;		/* blank: no default delay */
	n = wlen(s);
	for (i = 0; i < NCMDS && !eq(cmds[i].n, s, n); i++) ;
	if (i == NCMDS) {
		*delay_after = 1;
		return combo(d, s);
	}
	c = cmds[i].c;
	a = s + n;
	if (*a != 0) a++;		/* the one space after the command */
	*delay_after = c != C_REM && c != C_ID;
	switch (c) {
	case C_REM:
		return 0;
	case C_ID:
		return id_line(d, a);
	case C_DELAY:
		if (arg_num(d, a, 0, DK_MAXDELAY, &v) < 0) return DK_ERROR;
		return wait_ms(d, v);
	case C_DEFDELAY:
		if (arg_num(d, a, 0, DK_MAXDELAY, &v) < 0) return DK_ERROR;
		d->defdelay = v;
		return 0;
	case C_STRDELAY:
		if (arg_num(d, a, 0, DK_MAXDELAY, &v) < 0) return DK_ERROR;
		d->strdelay = v;
		return 0;
	case C_DEFSTRDELAY:
		if (arg_num(d, a, 0, DK_MAXDELAY, &v) < 0) return DK_ERROR;
		d->defstrdelay = v;
		return 0;
	case C_STRING:
	case C_STRINGLN:
		v = d->strdelay > 0 ? d->strdelay : d->defstrdelay;
		d->strdelay = 0;
		if (type(d, a, v) < 0) return DK_STOP;
		if (c == C_STRINGLN) {
			unsigned char e = K_ENTER;

			return tap(d, 0, &e, 1);
		}
		return 0;
	case C_REPEAT:
		if (arg_num(d, a, 1, DK_MAXREPEAT, &v) < 0) return DK_ERROR;
		return (int) v;
	case C_SYSRQ:
		if (one_key(d, a, &k) < 0) return DK_ERROR;
		{
			unsigned char u[2];

			u[0] = K_PRTSC;
			u[1] = k.usage;
			return tap(d, LA | k.mod, u, 2);
		}
	case C_ALTCHAR:
		a = skipsp(a);
		n = wlen(a);
		if (n == 0 || n > 5 || number(a, n, 0, 65535, &v) < 0
		    || *skipsp(a + n) != 0)
			return err(d, "bad number", a, n);
		return altchar(d, v, a[0] == '0' && n > 1);
	case C_ALTSTRING:
		return altstring(d, a);
	case C_HOLD:
	case C_RELEASE:
		if (one_key(d, a, &k) < 0) return DK_ERROR;
		return hold(d, &k, c == C_HOLD);
	case C_WAITBTN:
		if (*skipsp(a) != 0) return err(d, "no argument wanted", a, wlen(a));
		if (d->io == NULL) return 0;
		return d->io->button(d->io->c) < 0 ? DK_STOP : 0;
	case C_MEDIA:
		a = skipsp(a);
		n = wlen(a);
		for (i = 0; i < NMEDIA && !eq(media[i].n, a, n); i++) ;
		if (i == NMEDIA || *skipsp(a + n) != 0)
			return err(d, "unknown media key", a, n);
		if (cons(d, media[i].usage) < 0) return DK_STOP;
		return cons(d, 0);
	case C_GLOBE:
		if (one_key(d, a, &k) < 0) return DK_ERROR;
		if (cons(d, CC_GLOBE) < 0 || tap(d, k.mod, &k.usage, 1) < 0)
			return DK_STOP;
		return cons(d, 0);
	case C_MOVE:
		a = skipsp(a);
		n = wlen(a);
		if (number(a, n, -DK_MAXMOVE, DK_MAXMOVE, &v) < 0)
			return err(d, "bad number", a, n);
		a = skipsp(a + n);
		n = wlen(a);
		if (number(a, n, -DK_MAXMOVE, DK_MAXMOVE, &w) < 0
		    || *skipsp(a + n) != 0)
			return err(d, "bad number", a, n);
		while (v != 0 || w != 0) {	/* in steps of at most 127 */
			int dx = v > 127 ? 127 : v < -127 ? -127 : (int) v;
			int dy = w > 127 ? 127 : w < -127 ? -127 : (int) w;

			if (mouse(d, dx, dy, 0) < 0) return DK_STOP;
			v -= dx;
			w -= dy;
		}
		return 0;
	case C_SCROLL:
		if (arg_num(d, a, -DK_MAXMOVE, DK_MAXMOVE, &v) < 0)
			return DK_ERROR;
		while (v != 0) {
			int dw = v > 127 ? 127 : v < -127 ? -127 : (int) v;

			if (mouse(d, 0, 0, dw) < 0) return DK_STOP;
			v -= dw;
		}
		return 0;
	}
	return err(d, "unknown command", s, n);
}

/* ---- the file, a line at a time ---- */

struct rd {
	int fd;
	long pos;			/* the file offset of b[0] */
	int len, at;
	char b[128];
};

/* The next line into l[DK_LINE + 1], its file offset in *start, the
 * offset after it in *end.  Its length, -1 at the end, -2 too long (the
 * rest of it skipped), -3 a NUL in it, -4 a read error. */
static int rd_line(struct rd *r, char *l, long *start, long *end)
{
	int n = 0, bad = 0, got = 0, k;

	*start = r->pos + r->at;
	for (;;) {
		if (r->at == r->len) {
			r->pos += r->len;
			r->at = 0;
			r->len = (int) read(r->fd, r->b, sizeof r->b);
			if (r->len < 0) {
				r->len = 0;
				return -4;
			}
			if (r->len == 0) break;
		}
		k = r->b[r->at++];
		got = 1;
		if (k == '\n') break;
		if (k == 0 && bad == 0) bad = -3;
		if (n < DK_LINE) l[n++] = (char) k;
		else if (bad == 0) bad = -2;
	}
	*end = r->pos + r->at;
	if (!got) return -1;
	if (n > 0 && l[n - 1] == '\r') n--;	/* CRLF scripts */
	l[n] = 0;
	return bad != 0 ? bad : n;
}

static int rd_seek(struct rd *r, long off)
{
	if (lseek(r->fd, (off_t) off, SEEK_SET) < 0) return -1;
	r->pos = off;
	r->len = r->at = 0;
	return 0;
}

static int bad_read(struct dk *d, int n)
{
	return err(d, n == -2 ? "line too long" : n == -3 ? "NUL in the line"
							 : "read error", NULL, 0);
}

int dk_run(struct dk *d, int fd)
{
	char l[DK_LINE + 1];
	struct rd r;
	long start, end, prev = -1, after;
	int n, rep, i, delay, prevline = 0;

	memset(d->key, 0, sizeof d->key);
	d->mod = d->btn = 0;
	d->strdelay = d->defstrdelay = 0;
	d->line = 0;
	d->skipped = 0;
	d->err = NULL;
	d->errarg[0] = 0;
	r.fd = fd;
	r.pos = (long) lseek(fd, (off_t) 0, SEEK_CUR);
	if (r.pos < 0) r.pos = 0;
	r.len = r.at = 0;
	for (;;) {
		n = rd_line(&r, l, &start, &end);
		if (n == -1) break;
		d->line++;
		if (n < 0) return bad_read(d, n);
		if (d->io != NULL && d->io->line != NULL)
			d->io->line(d->io->c, d->line);
		rep = line(d, l, &delay);
		if (rep < 0) return rep;
		if (rep > 0) {			/* REPEAT */
			if (prev < 0)
				return err(d, "REPEAT with no line before it",
					   NULL, 0);
			if (d->io == NULL) continue;	/* checked already */
			after = end;
			if (rd_seek(&r, prev) < 0
			    || (n = rd_line(&r, l, &start, &end)) < 0)
				return err(d, "read error", NULL, 0);
			for (i = 0; i < rep; i++) {
				int dl, x = line(d, l, &dl);

				if (x < 0) {
					d->line = prevline;
					return x;
				}
				if (wait_ms(d, d->defdelay) < 0)
					return DK_STOP;
			}
			if (rd_seek(&r, after) < 0)
				return err(d, "read error", NULL, 0);
			continue;
		}
		if (delay) {			/* a line REPEAT repeats */
			prev = start;
			prevline = d->line;
			if (wait_ms(d, d->defdelay) < 0) return DK_STOP;
		} else if (d->io != NULL && wait_ms(d, 0) < 0)
			return DK_STOP;		/* REM: a stop check */
	}
	return dk_release(d) < 0 ? DK_STOP : DK_DONE;
}

int dk_count(int fd)
{
	char b[128];
	int n, i, lines = 0, part = 0;

	while ((n = (int) read(fd, b, sizeof b)) > 0)
		for (i = 0; i < n; i++) {
			part = b[i] != '\n';
			if (!part) lines++;
		}
	if (n < 0) return -1;
	return lines + part;
}
