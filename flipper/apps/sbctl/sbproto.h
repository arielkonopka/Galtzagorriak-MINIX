/* sbproto.h - Storz & Bickel vaporizer protocol core (spec 16.4, task T6)
 *
 * Pure C: no I/O, no malloc, tables in flash.  Encodes requests into GATT
 * writes and decodes reads/notifications into one small state record; the
 * BLE transport (bled, Phase 15-16) and the UI are elsewhere.
 *
 * Protocol from storzandbickel-ble 0.1.25 (MIT, Copyright (c) lorek123,
 * https://github.com/lorek123/snb-ble), protocol.py and the device modules;
 * not from the vendor's code.
 */
#ifndef SBPROTO_H
#define SBPROTO_H

enum sb_model { SB_UNKNOWN, SB_VOLCANO, SB_VENTY, SB_VEAZY, SB_CRAFTY };

/* Requests (sb_encode's req), with value: */
enum sb_req {
	SB_TARGET,	/* target temperature, tenths of a degree C */
	SB_HEATER,	/* 0 off, 1 on */
	SB_PUMP,	/* 0 off, 1 on (Volcano) */
	SB_MODE,	/* heater mode 0 off, 1 normal, 2 boost, 3 superboost
			 * (Venty, Veazy; the others have no such write) */
	SB_BRIGHTNESS,	/* Volcano, Crafty 0-100; Venty, Veazy 1-9 */
	SB_AUTOOFF,	/* seconds (Volcano, Crafty) */
	SB_FIND		/* "find my device" (Venty, Veazy, Crafty) */
};

#define SB_TEMP_NONE	(-32768)	/* temperature not known/reported */

/* What is known about a device; every field "unknown" until decoded. */
struct sb_state {
	unsigned char model;		/* enum sb_model */
	unsigned char heater;		/* 0 off, 1 on, 2 boost, 3 superboost */
	unsigned char pump;		/* Volcano */
	unsigned char ready;		/* at the target temperature */
	unsigned char battery;		/* %, 255 unknown */
	unsigned char charger;		/* charger connected */
	unsigned char fahrenheit;	/* the device shows degrees F */
	unsigned char brightness;
	short cur;			/* tenths C, SB_TEMP_NONE */
	short target;			/* tenths C, SB_TEMP_NONE */
	short boost;			/* Crafty: boost offset, tenths C;
					 * Venty/Veazy: boost offset, degrees */
	short superboost;		/* Venty/Veazy: superboost offset, deg. */
	unsigned short auto_off;	/* the setting, seconds */
	unsigned short countdown;	/* until auto-off, seconds */
	unsigned short reg[3];		/* Volcano status 1-3; Crafty status,
					 * project status 1, 2 */
};

/* One GATT write.  uuid: 128 bits, as written in text (big endian). */
struct sb_write {
	unsigned char uuid[16];
	unsigned char data[20];
	unsigned char len;
	unsigned char response;		/* write with response */
	unsigned char delay;		/* wait this many 10 ms before it */
};

#define SB_MAXW		3		/* writes one request can need */

/* Errors (negative returns) */
#define SB_ENOTSUP	(-1)		/* this model has no such request */
#define SB_ERANGE	(-2)		/* value out of the model's limits */
#define SB_EINVAL	(-3)		/* bad model or request */

int sb_detect(const char *name);		/* advertised name -> model */
void sb_init(struct sb_state *st, int model);
int sb_encode(const struct sb_state *st, int req, long value,
					struct sb_write *w);	/* count or < 0 */
int sb_decode(struct sb_state *st, const unsigned char *uuid,
		const unsigned char *data, int len);	/* 0, or -1 unknown */
int sb_reads(int model, unsigned char (*uuid)[16], int max); /* to poll */
int sb_temp_limits(int model, short *lo, short *hi);

#endif
