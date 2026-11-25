/* usbcan - the `usb` canary's core (Galtzagorriak MINIX, spec 11.2, R11.2.1
 * and R11.2.2, task T35): from the events of a USB device port, the verdict
 * level and the trace summary: what the host on the other side tries to do.
 *
 * Pure C: no I/O, no malloc, no writable statics; the caller owns the
 * state.  It only observes: nothing here answers the host (M.1).  Times are
 * ms from a free-running counter, differences unsigned.  Host-tested.  The
 * USB driver that delivers the events and the personas come later (device
 * lane); the host fingerprint (R11.2.4) is a table over this summary.
 */
#ifndef USBCAN_H
#define USBCAN_H

enum uc_level {
	UC_NONE,	/* nothing attached */
	UC_POWER,	/* R11.2.1 1: VBUS, no reset or SOF within 3 s */
	UC_ALIVE,	/* 2: reset and SOF, no SETUP */
	UC_ENUM,	/* 3: the device descriptor was read, address set */
	UC_CONFIG,	/* 4: SET_CONFIGURATION */
	UC_USED		/* 5: traffic on a function endpoint */
};

#define UC_POWER_MS	3000

struct uc_sum {
	unsigned char level;		/* enum uc_level */
	unsigned char powered;		/* UC_POWER was decided (3 s passed) */
	unsigned char first_wlen;	/* wLength of the first GET_DESCRIPTOR
					 * (DEVICE), 0 = not seen (cut at 255) */
	unsigned char reenum;		/* resets after the first SETUP */
	unsigned char strings;		/* string indexes asked for, bit n of
					 * the index 0..7; higher: bit 7 */
	unsigned char flags;		/* UCF_* */
	unsigned char setups;		/* SETUP packets (stops at 255) */
	unsigned char resets;		/* bus resets (stops at 255) */
	unsigned short langid;		/* language id asked for, 0 = none */
	unsigned long t_attach;
	unsigned long t_alive, t_enum, t_config, t_used;	/* 0 = never */
};

#define UCF_MSOS	0x01	/* string 0xEE or a vendor request (MS OS) */
#define UCF_QUALIFIER	0x02	/* device qualifier asked for */
#define UCF_BOS		0x04	/* BOS descriptor asked for */
#define UCF_SUSPEND	0x08	/* the host suspended the port */
#define UCF_ADDRESS	0x10	/* SET_ADDRESS seen */
#define UCF_CLASSREQ	0x20	/* a class request (HID, CDC, MSC...) */

void uc_init(struct uc_sum *s);
void uc_attach(struct uc_sum *s, unsigned long now);	/* VBUS present */
void uc_detach(struct uc_sum *s);
void uc_reset(struct uc_sum *s, unsigned long now);	/* bus reset */
void uc_sof(struct uc_sum *s, unsigned long now);	/* first SOF counts */
void uc_suspend(struct uc_sum *s);
/* One SETUP packet (the 8 bytes as on the wire). */
void uc_setup(struct uc_sum *s, unsigned long now, const unsigned char p[8]);
/* Traffic on a non-control endpoint of the persona. */
void uc_data(struct uc_sum *s, unsigned long now);
/* Time passing: decides "power only" after UC_POWER_MS. */
void uc_tick(struct uc_sum *s, unsigned long now);
/* A short name for the level: "power only", "host alive", ... */
const char *uc_level_name(int level);

#endif
