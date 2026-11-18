/* libhid - text and pointer actions as USB HID reports (spec R11.3.2).
 * See libhid.h.  The layout format is the stock Flipper firmware's (Bad
 * USB, GPLv3), so its .kl files work unchanged.
 */
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "libhid.h"

#define __	0x00, 0x00
#define K(u)	(u), 0x00		/* the key alone */
#define S(u)	(u), 0x02		/* with left Shift */

/* US (en-US.kl of the stock firmware, byte for byte; tests/hid checks). */
const unsigned char hid_us[HID_KL_SIZE] = {
	__, __, __, __, __, __, __, __,	/* . . . . . . . . */
	K(0x2a), K(0x2b), K(0x28), __, __, __, __, __,	/* . tab nl . . . . . */
	__, __, __, __, __, __, __, __,	/* . . . . . . . . */
	__, __, __, __, __, __, __, __,	/* . . . . . . . . */
	K(0x2c), S(0x1e), S(0x34), S(0x20), S(0x21), S(0x22), S(0x24), K(0x34),	/* sp ! " # $ % & ' */
	S(0x26), S(0x27), S(0x25), S(0x2e), K(0x36), K(0x2d), K(0x37), K(0x38),	/* ( ) * + , - . / */
	K(0x27), K(0x1e), K(0x1f), K(0x20), K(0x21), K(0x22), K(0x23), K(0x24),	/* 0 1 2 3 4 5 6 7 */
	K(0x25), K(0x26), S(0x33), K(0x33), S(0x36), K(0x2e), S(0x37), S(0x38),	/* 8 9 : ; < = > ? */
	S(0x1f), S(0x04), S(0x05), S(0x06), S(0x07), S(0x08), S(0x09), S(0x0a),	/* @ A B C D E F G */
	S(0x0b), S(0x0c), S(0x0d), S(0x0e), S(0x0f), S(0x10), S(0x11), S(0x12),	/* H I J K L M N O */
	S(0x13), S(0x14), S(0x15), S(0x16), S(0x17), S(0x18), S(0x19), S(0x1a),	/* P Q R S T U V W */
	S(0x1b), S(0x1c), S(0x1d), K(0x2f), K(0x31), K(0x30), S(0x23), S(0x2d),	/* X Y Z [ \ ] ^ _ */
	K(0x35), K(0x04), K(0x05), K(0x06), K(0x07), K(0x08), K(0x09), K(0x0a),	/* ` a b c d e f g */
	K(0x0b), K(0x0c), K(0x0d), K(0x0e), K(0x0f), K(0x10), K(0x11), K(0x12),	/* h i j k l m n o */
	K(0x13), K(0x14), K(0x15), K(0x16), K(0x17), K(0x18), K(0x19), K(0x1a),	/* p q r s t u v w */
	K(0x1b), K(0x1c), K(0x1d), S(0x2f), S(0x31), S(0x30), S(0x35), __,	/* x y z { | } ~ . */
};

int hid_kl_read(unsigned char *kl, const char *path)
{
	unsigned char extra;
	int fd, n, m;

	if ((fd = open(path, O_RDONLY)) < 0) return -1;
	n = (int) read(fd, (char *) kl, HID_KL_SIZE);
	m = n == HID_KL_SIZE ? (int) read(fd, (char *) &extra, 1) : 0;
	close(fd);
	if (n != HID_KL_SIZE || m != 0) {	/* short, long, or unreadable */
		memcpy(kl, hid_us, HID_KL_SIZE);	/* never half a layout */
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int hid_kl_load(unsigned char *kl, const char *name)
{
	char path[sizeof(HID_KL_DIR2) + HID_KL_NAME + 3];
	size_t n;

	if (name == NULL || *name == '\0' || strcmp(name, HID_KL_US) == 0) {
		memcpy(kl, hid_us, HID_KL_SIZE);
		return 0;
	}
	if (strchr(name, '/') != NULL) return hid_kl_read(kl, name);
	if ((n = strlen(name)) > HID_KL_NAME) {
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(path, HID_KL_DIR1);
	memcpy(path + sizeof(HID_KL_DIR1) - 1, name, n);
	strcpy(path + sizeof(HID_KL_DIR1) - 1 + n, ".kl");
	if (hid_kl_read(kl, path) == 0) return 0;
	if (errno != ENOENT) return -1;
	strcpy(path, HID_KL_DIR2);
	memcpy(path + sizeof(HID_KL_DIR2) - 1, name, n);
	strcpy(path + sizeof(HID_KL_DIR2) - 1 + n, ".kl");
	return hid_kl_read(kl, path);
}

unsigned hid_key(const unsigned char *kl, long c)
{
	if (c < 0 || c >= HID_KL_CHARS) return 0;
	return (unsigned) kl[2 * c + 1] << 8 | kl[2 * c];
}

long hid_utf8(const char **sp)
{
	const unsigned char *s = (const unsigned char *) *sp;
	long c;
	int n, i;

	if (*s == 0) return 0;
	if (*s < 0x80) {
		*sp += 1;
		return *s;
	}
	if (*s >= 0xC2 && *s <= 0xDF) n = 1, c = *s & 0x1F;
	else if (*s >= 0xE0 && *s <= 0xEF) n = 2, c = *s & 0x0F;
	else if (*s >= 0xF0 && *s <= 0xF4) n = 3, c = *s & 0x07;
	else {
		*sp += 1;
		return -1;
	}
	for (i = 1; i <= n; i++) {
		if ((s[i] & 0xC0) != 0x80) {	/* (also the end: 0) */
			*sp += 1;
			return -1;
		}
		c = c << 6 | (s[i] & 0x3F);
	}
	if ((n == 2 && c < 0x800) || (n == 3 && (c < 0x10000 || c > 0x10FFFF))
				|| (c >= 0xD800 && c <= 0xDFFF)) {
		*sp += 1;			/* overlong, surrogate, too big */
		return -1;
	}
	*sp += n + 1;
	return c;
}

void hid_kbd(unsigned char r[8], unsigned mod, unsigned usage)
{
	memset(r, 0, 8);
	r[0] = (unsigned char) mod;
	r[2] = (unsigned char) usage;
}

static unsigned char clamp(int v)
{
	if (v < -127) v = -127;
	if (v > 127) v = 127;
	return (unsigned char) (v & 0xFF);
}

void hid_mouse(unsigned char r[4], unsigned buttons, int dx, int dy,
								int wheel)
{
	r[0] = (unsigned char) buttons;
	r[1] = clamp(dx);
	r[2] = clamp(dy);
	r[3] = clamp(wheel);
}

void hid_cons(unsigned char r[2], unsigned usage)
{
	r[0] = (unsigned char) (usage & 0xFF);
	r[1] = (unsigned char) (usage >> 8 & 0xFF);
}

int hid_type(int fd, const unsigned char *kl, const char *s, int *skipped)
{
	unsigned char r[16];
	unsigned k;
	long c;
	int n = 0;

	if (skipped != NULL) *skipped = 0;
	while ((c = hid_utf8(&s)) != 0) {
		if ((k = hid_key(kl, c)) == 0) {
			if (skipped != NULL) (*skipped)++;
			continue;
		}
		hid_kbd(r, k >> 8, k & 0xFF);
		memset(r + 8, 0, 8);			/* all up */
		if (write(fd, (char *) r, sizeof r) != (int) sizeof r)
			return -1;
		n++;
	}
	return n;
}
