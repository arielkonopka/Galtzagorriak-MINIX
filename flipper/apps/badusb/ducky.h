/* ducky.h - the Bad USB script engine (spec R11.3.3): DuckyScript 1.0
 * with the stock Flipper firmware's extensions, one line at a time.
 *
 * Pure C, no malloc, no stdio, no device: the engine reads the script
 * from a file descriptor (a 128 B buffer and one DK_LINE line on the
 * stack, never the whole file), turns each line into reports with libhid
 * and hands them to the caller's dk_io.  Without a dk_io it only checks
 * the script (badusb -n).  The same code runs on the PC (tests/badusb:
 * golden reports, the fuzz test) and in badusb.
 *
 * The language is the stock firmware's (applications/main/bad_usb,
 * GPLv3): the commands, the key names and the rules below follow its
 * ducky_script*.c; the code is written for MINIX.
 *
 *	REM ...			a comment (also ID: VID/PID are not applied,
 *				that is usbmode's job; the line is checked)
 *	DELAY ms		wait
 *	DEFAULT_DELAY ms	wait after every line (DEFAULTDELAY)
 *	STRING_DELAY ms		between the characters of the next STRING
 *				only (STRINGDELAY); DEFAULT_STRING_DELAY ms
 *				(DEFAULTSTRINGDELAY): of every STRING
 *	STRING text		type it (UTF-8; characters with no key in the
 *				layout are left out); STRINGLN: then Enter
 *	REPEAT n		the last line again n times
 *	KEY ...			a key combination: modifiers (CTRL, SHIFT, ALT,
 *				GUI, CTRL-ALT, ...) then a key name (ENTER,
 *				F5, ...) or one character (after a modifier:
 *				the key alone, its layout modifiers dropped)
 *	HOLD KEY, RELEASE KEY	press and keep, let go (also mouse buttons)
 *	ALTCHAR n		Alt + the number on the keypad
 *	ALTSTRING text		each character as ALTCHAR (ALTCODE)
 *	SYSRQ KEY		Alt + PrintScreen + KEY
 *	MEDIA NAME		a consumer key (VOLUME_UP, PLAY_PAUSE, ...)
 *	GLOBE KEY		the Fn/Globe key held with KEY
 *	MOUSEMOVE x y		move (MOUSE_MOVE); MOUSESCROLL n (MOUSE_SCROLL)
 *	LEFTCLICK ...		a mouse click (RIGHTCLICK, MIDDLECLICK,
 *				WHEELCLICK, also with an underscore)
 *	WAIT_FOR_BUTTON_PRESS	wait for the person (dk_io.button)
 *
 * Bounds (spec 11.3 Security): a line longer than DK_LINE bytes, a NUL in
 * a line, a delay above DK_MAXDELAY, REPEAT above DK_MAXREPEAT and a
 * mouse move beyond +-DK_MAXMOVE are errors, reported with the line.
 */
#ifndef DUCKY_H
#define DUCKY_H

#define DK_LINE		256		/* longest line, bytes (no newline) */
#define DK_MAXDELAY	3600000L	/* ms, one DELAY: 1 hour */
#define DK_MAXREPEAT	10000
#define DK_MAXMOVE	32767
#define DK_ERRARG	24		/* the word an error names */

/* The caller's side.  Each function returns 0, or -1 to stop the script
 * (a signal, the device gone): dk_run then ends with DK_STOP. */
struct dk_io {
	int (*kbd)(void *c, const unsigned char r[8]);
	int (*mouse)(void *c, const unsigned char r[4]);
	int (*cons)(void *c, const unsigned char r[2]);
	int (*wait)(void *c, long ms);		/* also a stop check */
	int (*button)(void *c);			/* WAIT_FOR_BUTTON_PRESS */
	void (*line)(void *c, int n);		/* line n starts (may be NULL) */
	void *c;
};

struct dk {
	const unsigned char *kl;	/* the layout (libhid, 256 B) */
	const struct dk_io *io;		/* NULL: only check (badusb -n) */
	long defdelay;			/* DEFAULT_DELAY */
	long strdelay;			/* STRING_DELAY: the next STRING */
	long defstrdelay;		/* DEFAULT_STRING_DELAY */
	unsigned char mod;		/* HOLD: modifiers */
	unsigned char key[6];		/* HOLD: keys */
	unsigned char btn;		/* HOLD: mouse buttons */
	int line;			/* the line running, from 1 */
	int skipped;			/* characters with no key */
	const char *err;		/* DK_ERROR: what */
	char errarg[DK_ERRARG];		/* DK_ERROR: the word, or "" */
};

#define DK_DONE		0
#define DK_ERROR	(-1)		/* d->err, d->errarg, at d->line */
#define DK_STOP		(-2)		/* an io function said stop */

/* Run (io set) or check (io NULL) the script on fd from where it is.
 * d->kl, d->io and d->defdelay are the caller's; the rest is set here. */
int dk_run(struct dk *d, int fd);

/* The lines of the script on fd (from where it is; the count of the
 * progress line "n / total"), or -1. */
int dk_count(int fd);

/* Release everything held (HOLD) through io: the end of a script. */
int dk_release(struct dk *d);

#endif /* DUCKY_H */
