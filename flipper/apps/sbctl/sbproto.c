/* sbproto.c - Storz & Bickel vaporizer protocol core (spec 16.4, task T6)
 *
 * Pure C: no I/O, no malloc; the tables are const (flash).  See sbproto.h.
 *
 * The protocol follows storzandbickel-ble 0.1.25 (MIT License, Copyright (c)
 * lorek123, https://github.com/lorek123/snb-ble): protocol.py for the UUIDs,
 * bits and packet layouts, volcano.py, venty.py and crafty.py for how each
 * request is written and each value read.  The MIT license is compatible
 * with this GPLv3 project; its notice is kept here.
 */
#include "sbproto.h"

/* Characteristics: the first 32 bits of the UUID and its family, the other
 * 96 bits (protocol.py). */
enum { F_SB, F_CRAFTY };
static const unsigned char tail[2][12] = {
	{ 0x53, 0x54, 0x4f, 0x52, 0x5a, 0x26, 0x42, 0x49, 0x43, 0x4b, 0x45,
	  0x4c },			/* -5354-4f52-5a26-4249434b454c */
	{ 0x4c, 0x45, 0x4b, 0x43, 0x49, 0x42, 0x26, 0x5a, 0x52, 0x4f, 0x54,
	  0x53 },			/* -4c45-4b43-4942-265a524f5453 */
};

enum {	/* characteristic numbers, index into chars[] */
	V_CUR, V_TARGET, V_HEAT_ON, V_HEAT_OFF, V_PUMP_ON, V_PUMP_OFF, V_LED,
	V_AUTOOFF, V_COUNTDOWN, V_ST1, V_ST2, V_ST3,
	Q_MAIN,
	C_TARGET, C_BATTERY, C_CUR, C_STATUS, C_HEAT_ON, C_HEAT_OFF, C_BOOST,
	C_POWER, C_PRJ, C_PRJ2, C_LED, C_AUTOOFF, C_COUNTDOWN, C_CODE,
	NCHARS
};
static const struct { unsigned long first; unsigned char family; }
chars[NCHARS] = {
	{ 0x10110001UL, F_SB }, { 0x10110003UL, F_SB },
	{ 0x1011000fUL, F_SB }, { 0x10110010UL, F_SB },
	{ 0x10110013UL, F_SB }, { 0x10110014UL, F_SB },
	{ 0x10110005UL, F_SB }, { 0x1011000dUL, F_SB },
	{ 0x1011000cUL, F_SB }, { 0x1010000cUL, F_SB },
	{ 0x1010000dUL, F_SB }, { 0x1010000eUL, F_SB },
	{ 0x00000001UL, F_SB },
	{ 0x00000021UL, F_CRAFTY }, { 0x00000041UL, F_CRAFTY },
	{ 0x00000011UL, F_CRAFTY }, { 0x00000052UL, F_CRAFTY },
	{ 0x00000081UL, F_CRAFTY }, { 0x00000091UL, F_CRAFTY },
	{ 0x00000031UL, F_CRAFTY }, { 0x00000083UL, F_CRAFTY },
	{ 0x00000093UL, F_CRAFTY }, { 0x000001c3UL, F_CRAFTY },
	{ 0x00000051UL, F_CRAFTY }, { 0x00000061UL, F_CRAFTY },
	{ 0x00000071UL, F_CRAFTY }, { 0x000001b3UL, F_CRAFTY },
};

/* Temperature limits per model, tenths of a degree C (R16.4.7). */
static const short tlimit[5][2] = {
	{ 0, 0 }, { 400, 2300 }, { 400, 2100 }, { 400, 2100 }, { 400, 2100 }
};

/* Bits (protocol.py) */
#define VOL_ST1_HEATER	0x0020
#define VOL_ST1_PUMP	0x2000
#define VOL_ST2_FAHR	0x0200
#define Q_CMD_STATUS	0x01
#define Q_CMD_SETTINGS	0x06
#define Q_CMD_FIND	0x0D
#define Q_MASK_TEMP	0x02
#define Q_MASK_HEATER	0x20
#define Q_SET_FAHR	0x01
#define Q_SET_REACHED	0x02
#define Q_TEMP_NONE	0x8000
#define CR_ST_HEATER	0x0001
#define CR_ST_FAHR	0x0008
#define CR_PRJ_BOOST	0x0020
#define CR_PRJ_SUPER	0x0040
#define CR_PRJ_READY	0x0100
#define CR_PRJ2_REACHED	0x0004
#define CR_PRJ2_FIND	0x0008
#define CR_POWER_CHG	0x0001
#define CR_CODE		815		/* Sicherheitscode, before auto-off */

static void uuid_of(int c, unsigned char *u)
{
	unsigned long f = chars[c].first;
	int i;

	u[0] = f >> 24; u[1] = f >> 16; u[2] = f >> 8; u[3] = f;
	for (i = 0; i < 12; i++) u[4 + i] = tail[chars[c].family][i];
}

static int char_of(const unsigned char *u)
{
	int c, i;

	for (c = 0; c < NCHARS; c++) {
		unsigned long f = chars[c].first;

		if (u[0] != (unsigned char) (f >> 24)
		    || u[1] != (unsigned char) (f >> 16)
		    || u[2] != (unsigned char) (f >> 8)
		    || u[3] != (unsigned char) f) continue;
		for (i = 0; i < 12 && u[4 + i] == tail[chars[c].family][i]; i++)
			;
		if (i == 12) return c;
	}
	return -1;
}

/* Case-insensitive "is k in s". */
static int has(const char *s, const char *k)
{
	const char *a, *b;
	int ca, cb;

	for (; *s; s++) {
		for (a = s, b = k; *b; a++, b++) {
			ca = *a >= 'a' && *a <= 'z' ? *a - 32 : *a;
			cb = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
			if (ca != cb) break;
		}
		if (*b == 0) return 1;
	}
	return 0;
}

int sb_detect(const char *name)
{
	/* the library's order: the Volcano first, the Veazy before the Venty */
	if (has(name, "S&B VOLCANO")) return SB_VOLCANO;
	if (has(name, "VEAZY") || has(name, "S&B VZ")) return SB_VEAZY;
	if (has(name, "VENTY") || has(name, "S&B VY")) return SB_VENTY;
	if (has(name, "CRAFTY")) return SB_CRAFTY;
	return SB_UNKNOWN;
}

void sb_init(struct sb_state *st, int model)
{
	unsigned char *p = (unsigned char *) st;
	unsigned i;

	for (i = 0; i < sizeof(*st); i++) p[i] = 0;
	st->model = model;
	st->battery = 255;
	st->cur = st->target = st->boost = st->superboost = SB_TEMP_NONE;
}

int sb_temp_limits(int model, short *lo, short *hi)
{
	if (model <= SB_UNKNOWN || model > SB_CRAFTY) return SB_EINVAL;
	*lo = tlimit[model][0];
	*hi = tlimit[model][1];
	return 0;
}

/* Writes are built here. */
static struct sb_write *add(struct sb_write *w, int *n, int c, int len)
{
	struct sb_write *x = &w[(*n)++];
	int i;

	uuid_of(c, x->uuid);
	for (i = 0; i < (int) sizeof(x->data); i++) x->data[i] = 0;
	x->len = len;
	x->response = 0;		/* the vendor app writes without */
	x->delay = 0;
	return x;
}

static void le16(unsigned char *p, unsigned v)
{
	p[0] = v;
	p[1] = v >> 8;
}

static struct sb_write *qvap(struct sb_write *w, int *n, int cmd, int mask)
{
	struct sb_write *x = add(w, n, Q_MAIN, 20);	/* always 20 bytes */

	x->data[0] = cmd;
	x->data[1] = mask;
	return x;
}

int sb_encode(const struct sb_state *st, int req, long v, struct sb_write *w)
{
	struct sb_write *x;
	int n = 0, m = st->model;

	if (m <= SB_UNKNOWN || m > SB_CRAFTY) return SB_EINVAL;
	if (req == SB_TARGET && (v < tlimit[m][0] || v > tlimit[m][1]))
		return SB_ERANGE;			/* R16.4.7 */

	if (m == SB_VOLCANO) switch (req) {
	case SB_TARGET:		/* uint32, as the vendor app writes it */
		x = add(w, &n, V_TARGET, 4);
		le16(x->data, (unsigned) v);
		return n;
	case SB_HEATER:
		x = add(w, &n, v ? V_HEAT_ON : V_HEAT_OFF, 1);
		x->data[0] = 1;
		return n;
	case SB_PUMP:
		x = add(w, &n, v ? V_PUMP_ON : V_PUMP_OFF, 1);
		x->data[0] = 1;
		return n;
	case SB_BRIGHTNESS:
		if (v < 0 || v > 100) return SB_ERANGE;
		le16(add(w, &n, V_LED, 2)->data, (unsigned) v);
		return n;
	case SB_AUTOOFF:
		if (v < 0 || v > 0xFFFF) return SB_ERANGE;
		le16(add(w, &n, V_AUTOOFF, 2)->data, (unsigned) v);
		return n;
	default:
		return SB_ENOTSUP;
	}

	if (m == SB_VENTY || m == SB_VEAZY) switch (req) {
	case SB_TARGET:
		le16(qvap(w, &n, Q_CMD_STATUS, Q_MASK_TEMP)->data + 4,
								(unsigned) v);
		return n;
	case SB_HEATER:
		v = v ? 1 : 0;
		/* FALLTHROUGH */
	case SB_MODE:
		if (v < 0 || v > 3) return SB_ERANGE;
		/* mode 1 does not end (super)boost on a real Venty: off first,
		 * then normal 300 ms later (venty.py set_heater_mode) */
		if (v == 1 && (st->heater == 2 || st->heater == 3))
			qvap(w, &n, Q_CMD_STATUS, Q_MASK_HEATER)->data[11] = 0;
		x = qvap(w, &n, Q_CMD_STATUS, Q_MASK_HEATER);
		x->data[11] = v;
		if (n == 2) x->delay = 30;
		return n;
	case SB_BRIGHTNESS:
		if (v < 1 || v > 9) return SB_ERANGE;
		qvap(w, &n, Q_CMD_SETTINGS, 0x01)->data[2] = v;
		return n;
	case SB_FIND:
		qvap(w, &n, Q_CMD_FIND, 0x01);
		return n;
	default:
		return SB_ENOTSUP;
	}

	switch (req) {		/* Crafty */
	case SB_TARGET:
		le16(add(w, &n, C_TARGET, 2)->data, (unsigned) v);
		/* the vendor app rewrites the boost offset after every target
		 * write (crafty.js), or the device loses it */
		if (st->boost != SB_TEMP_NONE)
			le16(add(w, &n, C_BOOST, 2)->data,
						(unsigned) st->boost);
		return n;
	case SB_HEATER:
		add(w, &n, v ? C_HEAT_ON : C_HEAT_OFF, 2);	/* 00 00 */
		return n;
	case SB_BRIGHTNESS:
		if (v < 0 || v > 100) return SB_ERANGE;
		le16(add(w, &n, C_LED, 2)->data, (unsigned) v);
		return n;
	case SB_AUTOOFF:	/* the security code must come right before */
		if (v < 0 || v > 0xFFFF) return SB_ERANGE;
		le16(add(w, &n, C_CODE, 2)->data, CR_CODE);
		le16(add(w, &n, C_AUTOOFF, 2)->data, (unsigned) v);
		return n;
	case SB_FIND:		/* project status 2, bit 3 (clears in 30 s) */
		le16(add(w, &n, C_PRJ2, 2)->data,
				(unsigned) (st->reg[2] | CR_PRJ2_FIND));
		return n;
	default:		/* boost is the device's own button */
		return SB_ENOTSUP;
	}
}

/* The characteristics to read for a full update (the qvap devices send
 * their status as notifications on their one characteristic instead). */
int sb_reads(int model, unsigned char (*uuid)[16], int max)
{
	static const unsigned char vol[] = { V_CUR, V_TARGET, V_ST1, V_ST2,
		V_ST3, V_LED, V_AUTOOFF, V_COUNTDOWN };
	static const unsigned char cra[] = { C_CUR, C_TARGET, C_BATTERY,
		C_BOOST, C_STATUS, C_PRJ, C_PRJ2, C_POWER, C_LED, C_AUTOOFF,
		C_COUNTDOWN };
	const unsigned char *l;
	int n, i;

	if (model == SB_VOLCANO) { l = vol; n = sizeof(vol); }
	else if (model == SB_CRAFTY) { l = cra; n = sizeof(cra); }
	else if (model == SB_VENTY || model == SB_VEAZY) { l = 0; n = 0; }
	else return SB_EINVAL;
	for (i = 0; i < n && i < max; i++) uuid_of(l[i], uuid[i]);
	return i;
}

static unsigned u16(const unsigned char *d)
{
	return d[0] | (unsigned) d[1] << 8;
}

static void crafty_derived(struct sb_state *st)
{
	if (st->reg[0] & CR_ST_HEATER)
		st->heater = st->reg[1] & CR_PRJ_SUPER ? 3
			   : st->reg[1] & CR_PRJ_BOOST ? 2 : 1;
	else st->heater = 0;
	/* bit 8 holds while at temperature; project status 2 bit 2 only
	 * pulses on current firmware (protocol.py) */
	st->ready = (st->reg[1] & CR_PRJ_READY) || (st->reg[2] & CR_PRJ2_REACHED);
}

int sb_decode(struct sb_state *st, const unsigned char *uuid,
				const unsigned char *d, int len)
{
	int c = char_of(uuid);
	unsigned v;

	if (c < 0 || len < 1) return -1;
	v = len >= 2 ? u16(d) : d[0];
	switch (c) {
	case V_CUR:	st->cur = v; break;
	case V_TARGET:	st->target = v; break;
	case V_ST1:
		st->reg[0] = v;
		st->heater = (v & VOL_ST1_HEATER) != 0;
		st->pump = (v & VOL_ST1_PUMP) != 0;
		break;
	case V_ST2:
		st->reg[1] = v;
		st->fahrenheit = (v & VOL_ST2_FAHR) != 0;
		break;
	case V_ST3:	st->reg[2] = v; break;
	case V_LED:	st->brightness = v; break;
	case V_AUTOOFF:	st->auto_off = v; break;
	case V_COUNTDOWN: st->countdown = v; break;

	case Q_MAIN:
		if (d[0] == Q_CMD_STATUS) {
			if (len < 15) return -1;
			v = u16(d + 2);
			st->cur = v == Q_TEMP_NONE || v == 0 || v > 2500
						? SB_TEMP_NONE : (short) v;
			v = u16(d + 4);
			if (v >= (unsigned) tlimit[st->model][0]
					&& v <= (unsigned) tlimit[st->model][1])
				st->target = v;
			st->boost = d[6];
			st->superboost = d[7];
			st->battery = d[8];
			st->countdown = d[9] + d[10];	/* added, as the app */
			st->heater = d[11] <= 3 ? d[11] : 0;
			st->charger = d[13] != 0;
			st->fahrenheit = (d[14] & Q_SET_FAHR) != 0;
			st->ready = (d[14] & Q_SET_REACHED) != 0;
		} else if (d[0] == Q_CMD_SETTINGS) {
			if (len < 3) return -1;
			st->brightness = d[2];
		} else return -1;
		break;

	case C_CUR:	st->cur = v; break;
	case C_TARGET:	st->target = v; break;
	case C_BOOST:	st->boost = v; break;
	case C_BATTERY:	st->battery = d[0]; break;
	case C_STATUS:	/* 10 bytes: the register is at 8; else at 0 */
		st->reg[0] = len >= 10 ? u16(d + 8) : v;
		st->fahrenheit = (st->reg[0] & CR_ST_FAHR) != 0;
		crafty_derived(st);
		break;
	case C_PRJ:	st->reg[1] = v; crafty_derived(st); break;
	case C_PRJ2:	st->reg[2] = v; crafty_derived(st); break;
	case C_POWER:	st->charger = (v & CR_POWER_CHG) != 0; break;
	case C_LED:	st->brightness = v; break;
	case C_AUTOOFF:	st->auto_off = v; break;
	case C_COUNTDOWN: st->countdown = v; break;
	default:
		return -1;
	}
	if (st->model == SB_VOLCANO)		/* no ready bit: compare */
		st->ready = st->heater && st->cur != SB_TEMP_NONE
			&& st->target != SB_TEMP_NONE && st->cur >= st->target;
	return 0;
}
