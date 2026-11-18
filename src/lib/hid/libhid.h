/* libhid - text and pointer actions as USB HID reports (spec R11.3.2).
 *
 * Static, no malloc, no stdio; builds the reports of <minix/hid.h>'s
 * device files.  A keyboard layout is a stock Flipper Bad USB ".kl" file:
 * HID_KL_SIZE bytes, 128 little-endian 16-bit entries, one per ASCII code,
 * each the key's usage (low byte) and the boot modifier byte (high byte,
 * HID_MOD_*: Shift 0x02, AltGr 0x40), 0 for "no key".  US is built in
 * (const, in flash); the others are read into the caller's buffer (on its
 * stack) from the layout directories when a script or a page starts.
 *
 *	unsigned char kl[HID_KL_SIZE];
 *	int fd = open("/dev/hidkbd", O_WRONLY), skipped;
 *
 *	if (hid_kl_load(kl, "de-DE") < 0) ...	(no such layout, or not
 *						 exactly HID_KL_SIZE bytes)
 *	hid_type(fd, kl, "Kaixo!\n", &skipped);	key, then key-up, per char
 *
 * Host-testable: tests/hid (make hidtest) builds it with the host's cc.
 */
#ifndef LIBHID_H
#define LIBHID_H

#define HID_KL_SIZE	256		/* a stock .kl file, exactly */
#define HID_KL_CHARS	128		/* its entries: ASCII 0-127 */
#define HID_KL_US	"en-US"		/* the built-in one */
#define HID_KL_DIR1	"/usr/share/hid/layouts/"
#define HID_KL_DIR2	"/home/flipper/badusb/assets/layouts/"	/* /ext */
#define HID_KL_NAME	32		/* longest layout name */

extern const unsigned char hid_us[HID_KL_SIZE];

/* Layouts.  hid_kl_read: the file at path into kl; 0, or -1 (errno: the
 * open's, EINVAL when it is not exactly HID_KL_SIZE bytes).  hid_kl_load:
 * by name: NULL, "" or HID_KL_US copies the built-in US one; a name with
 * a '/' is a path; else NAME.kl in HID_KL_DIR1, then HID_KL_DIR2. */
int hid_kl_read(unsigned char *kl, const char *path);
int hid_kl_load(unsigned char *kl, const char *name);

/* The key of character c (a code point): modifiers << 8 | usage, or 0
 * when the layout has none (non-ASCII, control characters other than
 * tab and newline). */
unsigned hid_key(const unsigned char *kl, long c);

/* The next code point of a UTF-8 string, *sp moved past it: 0 at the
 * end, -1 for a malformed sequence (one byte skipped). */
long hid_utf8(const char **sp);

/* Reports.  hid_mouse clamps dx, dy and wheel to -127..127. */
void hid_kbd(unsigned char r[8], unsigned mod, unsigned usage);
void hid_mouse(unsigned char r[4], unsigned buttons, int dx, int dy,
								int wheel);
void hid_cons(unsigned char r[2], unsigned usage);

/* Type a UTF-8 string on fd (/dev/hidkbd): for each character its key,
 * then all keys up, in one write.  Characters with no key in the layout
 * are left out and counted in *skipped (may be NULL).  Returns the
 * characters typed, or -1 when a write failed (errno; what was typed
 * before stays typed). */
int hid_type(int fd, const unsigned char *kl, const char *s, int *skipped);

#endif /* LIBHID_H */
