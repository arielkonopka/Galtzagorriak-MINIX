/* hid.h - the USB HID device files (spec R10.2, R11.3.1;
 * kernel/arm/hiddev.c).  Major 6 (the Flipper device task), group usb,
 * mode 0660.  Every backend implements exactly this: the QEMU one
 * (hal/qemu, a console line per report) and the Flipper's USB one (T28b).
 *
 *	minor 17  /dev/hidkbd	write: 8-byte boot keyboard reports
 *				(modifiers, 0, six usages); read: the host's
 *				LED byte (HID_LED_*) once it changed since
 *				the last read (blocks; O_NONBLOCK: EAGAIN)
 *	minor 18  /dev/hidmouse	write: 4-byte reports (buttons, dx, dy,
 *				wheel), each of dx, dy, wheel -127..127
 *	minor 19  /dev/hidcons	write: 2-byte consumer usages (HID_CC_*,
 *				little endian; 0 releases)
 *
 * A write may carry several whole reports (at most HID_MAXWRITE bytes);
 * they go out one by one, in order.  It fails with EAGAIN while the host
 * has not configured the device or has suspended it (poll() for POLLOUT
 * waits for that), and with EBUSY while a non-HID persona owns the bus.
 *
 * poll(): POLLOUT when a write would go out now; POLLIN when the state
 * (HIDIOCSTATE) changed since it was last asked, and on /dev/hidkbd also
 * when the LED byte changed since the last read.  So a program sleeps in
 * poll() and wakes only on a change, never polls in a loop (R4b.6).
 *
 * One USB persona at a time (R11.3.1, R11.3.7): while a non-HID persona
 * (HIDP_*: the 11.2 canary, an 11.1 reader mode, mass storage) holds the
 * bus, open() of these files fails with EBUSY and so do writes on files
 * opened before.  HIDIOCSPERSONA takes the bus for a persona (0 gives it
 * back); the holder's close gives it back too.  The canary always wins:
 * taking the bus never waits for the HID apps.
 */
#ifndef _MINIX_HID_H
#define _MINIX_HID_H

#include <sys/ioctl.h>

#define HID_KBD_MINOR	17
#define HID_MOUSE_MINOR	18
#define HID_CONS_MINOR	19

#define HID_KBD_LEN	8		/* report sizes, bytes */
#define HID_MOUSE_LEN	4
#define HID_CONS_LEN	2
#define HID_MAXWRITE	64		/* bytes per write() */

/* HIDIOCSTATE: bits 0-7 the bus, 8-15 the persona holding it (HIDP_*),
 * 16-23 the host's LED byte. */
#define HID_ATTACHED	0x01		/* a cable to a host (VBUS) */
#define HID_CONFIGURED	0x02		/* the host configured us */
#define HID_SUSPENDED	0x04		/* the host suspended the bus */
#define HID_PERSONA(s)	((int) ((s) >> 8 & 0xFF))
#define HID_LEDS(s)	((int) ((s) >> 16 & 0xFF))

#define HID_LED_NUM	0x01		/* the LED byte (HID 1.11, boot) */
#define HID_LED_CAPS	0x02
#define HID_LED_SCROLL	0x04

/* Personas that are not HID (HIDIOCSPERSONA). */
#define HIDP_NONE	0
#define HIDP_CANARY	1		/* spec 11.2 */
#define HIDP_READER	2		/* an 11.1 reader mode */
#define HIDP_STORAGE	3		/* mass storage, R10.3 */
#define HIDP_TEST	0x7F		/* a test's fake persona (M2) */

#define HIDIOCSTATE	_IOR('H', 1, int)	/* the state; clears POLLIN */
#define HIDIOCSPERSONA	_IOW('H', 2, int)	/* take (HIDP_*) / give back (0) */
/* Test hooks, QEMU only (ENOTTY on the Flipper): HIDIOCFAKE sets the bus
 * bits and the LED byte (bits 16-23) as a host would; HIDIOCLINE gives
 * the FNV-1a hash (32 bits) of the last "hid< ..." console line, without
 * its newline. */
#define HIDIOCFAKE	_IOW('H', 3, int)
#define HIDIOCLINE	_IOR('H', 4, long)

/* Boot keyboard modifier byte (the first byte of a keyboard report, and
 * the high byte of a stock .kl layout entry). */
#define HID_MOD_LCTRL	0x01
#define HID_MOD_LSHIFT	0x02
#define HID_MOD_LALT	0x04
#define HID_MOD_LGUI	0x08
#define HID_MOD_RCTRL	0x10
#define HID_MOD_RSHIFT	0x20
#define HID_MOD_RALT	0x40		/* AltGr */
#define HID_MOD_RGUI	0x80

/* Mouse buttons (the first byte of a mouse report). */
#define HID_BTN_LEFT	0x01
#define HID_BTN_RIGHT	0x02
#define HID_BTN_MIDDLE	0x04

/* Consumer control usages (HID usage page 0x0C) for /dev/hidcons. */
#define HID_CC_BRIGHT_UP	0x006F
#define HID_CC_BRIGHT_DOWN	0x0070
#define HID_CC_NEXT		0x00B5
#define HID_CC_PREV		0x00B6
#define HID_CC_STOP		0x00B7
#define HID_CC_PLAY_PAUSE	0x00CD
#define HID_CC_MUTE		0x00E2
#define HID_CC_VOL_UP		0x00E9
#define HID_CC_VOL_DOWN		0x00EA

#endif /* _MINIX_HID_H */
