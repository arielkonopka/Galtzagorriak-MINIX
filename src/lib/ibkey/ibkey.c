/* ibkey.c - Cyfral and Metakom iButton keys (spec R6.7 part 3b).
 *
 * Ported from the Unleashed firmware (DarkFlippers/unleashed-firmware,
 * branch dev, 2026-11-19), lib/ibutton/protocols/misc/protocol_cyfral.c
 * and protocol_metakom.c, GPLv3 as this project.  The state machines are
 * theirs, step for step; what changed: times are microseconds (Unleashed
 * counts CPU cycles, 64 per us), the protocol dictionary and furi are
 * gone, C89 for MINIX's user programs, and ibk_frame() turns the
 * encoder's yields into a pulse list for /dev/ibutton (IBIOCPLAY).
 *
 * The level given with a duration is the level of that segment.  That is
 * what the encoders yield (Unleashed plays them with
 * furi_hal_ibutton_pin_write(level)) and what the decoders take.
 */
#include <string.h>
#include "ibkey.h"

#define CYFRAL_0_LOW	IBK_2THIRD_US
#define CYFRAL_0_HI	IBK_THIRD_US
#define CYFRAL_1_LOW	IBK_THIRD_US
#define CYFRAL_1_HI	IBK_2THIRD_US
#define CYFRAL_MAX_PERIOD_US 230

#define METAKOM_0_LOW	IBK_THIRD_US
#define METAKOM_0_HI	IBK_2THIRD_US
#define METAKOM_1_LOW	IBK_2THIRD_US
#define METAKOM_1_HI	IBK_THIRD_US
#define METAKOM_SAMPLES	10

/* Cyfral states */
#define CY_WAIT_START	0
#define CY_READ_NIBBLE	1
#define CY_READ_STOP	2
/* Metakom states */
#define MK_WAIT_SYNC	0	/* measuring the bit period */
#define MK_WAIT_START_BIT 1
#define MK_WAIT_START_WORD 2
#define MK_READ_WORD	3
#define MK_READ_STOP	4
/* bit states (both) */
#define BIT_WAIT_HIGH	0
#define BIT_WAIT_LOW	1

int ibk_type(name)
const char *name;
{
	if (strcmp(name, "Cyfral") == 0) return IBK_CYFRAL;
	if (strcmp(name, "Metakom") == 0) return IBK_METAKOM;
	return 0;
}

const char *ibk_name(type)
int type;
{
	return type == IBK_CYFRAL ? "Cyfral" : type == IBK_METAKOM ? "Metakom"
		: "?";
}

int ibk_size(type)
int type;
{
	return type == IBK_CYFRAL ? 2 : type == IBK_METAKOM ? 4 : 0;
}

static int even_parity(b)
unsigned b;
{
	int n = 0;

	for (b &= 0xFF; b != 0; b >>= 1) n += b & 1;
	return n % 2 == 0;
}

int ibk_valid(type, key)
int type;
const unsigned char *key;
{
	int i;

	if (type == IBK_CYFRAL) return 1;
	if (type != IBK_METAKOM) return -1;
	for (i = 0; i < 4; i++)
		if (!even_parity(key[i])) return 0;
	return 1;
}

static unsigned long key_value(type, key)
int type;
const unsigned char *key;
{
	unsigned long v = 0;
	int i;

	for (i = ibk_size(type) - 1; i >= 0; i--) v = v << 8 | key[i];
	return v;
}

static void value_key(d, data)
struct ibk_dec *d;
unsigned long data;
{
	int i;

	for (i = 0; i < ibk_size(d->type); i++)
		d->key[i] = (unsigned char) (data >> (8 * i) & 0xFF);
}


/*==========================================================================*
 *				encoders				    *
 *==========================================================================*/
/* protocol_cyfral_encoder_encode: each 2 bits of the code become a
 * nibble with one 0 in it; the first pair ends up in the top nibble. */
static unsigned long cyfral_encode(data)
unsigned long data;
{
	static const unsigned char nib[4] = { 0xE, 0xD, 0xB, 0x7 };
	unsigned long value = 0;
	int i;

	for (i = 0; i <= 7; i++)
		value = (value << 4 | nib[data >> (i * 2) & 3]) & 0xFFFFFFFFUL;
	return value;
}

/* protocol_cyfral_encoder_yield, step i of 72: the start nibble 0001,
 * then the 32 code bits least significant first; each bit a low, then a
 * high. */
static int cyfral_yield(code, i, us)
unsigned long code;
int i;
unsigned *us;
{
	int hi = i % 2, bit;

	if (i < 8) bit = i >= 6;		/* start word 0b0001 */
	else bit = (int) (code >> ((i - 8) / 2) & 1);
	if (!hi) *us = bit ? CYFRAL_1_LOW : CYFRAL_0_LOW;
	else *us = bit ? CYFRAL_1_HI : CYFRAL_0_HI;
	return hi;
}

/* protocol_metakom_encoder_yield, step i of 71: a sync low of a whole
 * period, the start word 010, the 32 bits most significant first; each
 * bit a high, then a low. */
static int metakom_yield(data, i, us)
unsigned long data;
int i;
unsigned *us;
{
	int lo, bit;

	if (i == 0) {
		*us = IBK_PERIOD_US;
		return 0;
	}
	lo = (i - 1) % 2;
	if (i <= 6) bit = (i - 1) / 2 == 1;	/* start word 0b010 */
	else bit = (int) (data >> (31 - (i - 7) / 2) & 1);
	if (!lo) *us = bit ? METAKOM_1_LOW : METAKOM_0_LOW;
	else *us = bit ? METAKOM_1_HI : METAKOM_0_HI;
	return !lo;
}

int ibk_frame(type, key, dur, level, max)
int type;
const unsigned char *key;
unsigned short *dur;
int *level;
int max;
{
	unsigned long v;
	unsigned us;
	int i, n = 0, steps, lv, last = -1;

	if (type == IBK_CYFRAL) steps = 72;
	else if (type == IBK_METAKOM) steps = 71;
	else return -1;
	v = key_value(type, key);
	if (type == IBK_CYFRAL) v = cyfral_encode(v);
	for (i = 0; i < steps; i++) {
		lv = type == IBK_CYFRAL ? cyfral_yield(v, i, &us)
					: metakom_yield(v, i, &us);
		if (lv == last) {		/* the same level goes on */
			dur[n - 1] = (unsigned short) (dur[n - 1] + us);
			continue;
		}
		if (n >= max) return -1;
		if (n == 0) *level = lv;
		dur[n++] = (unsigned short) us;
		last = lv;
	}
	if (n > 1 && last == *level) {		/* the loop joins them */
		dur[n - 1] = (unsigned short) (dur[n - 1] + dur[0]);
		memmove(dur, dur + 1, (size_t) (n - 1) * sizeof(*dur));
		n--;
		*level = !*level;
	}
	return n;
}


/*==========================================================================*
 *				decoders				    *
 *==========================================================================*/
void ibk_start(d, type)
struct ibk_dec *d;
int type;
{
	memset(d, 0, sizeof(*d));
	d->type = type;
	d->bit_state = BIT_WAIT_LOW;
	d->valid = 1;
	/* Unleashed starts the nibble at 0, so after a restart a single 1
	 * bit already looks like the start nibble 0001, and the decoder can
	 * stay a bit off the frame from then on (it then only finds the key
	 * when noise breaks the pattern).  All ones: the start nibble needs
	 * four real bits. */
	d->nibble = 0xF;
}

/* protocol_cyfral_decoder_process_bit: 0 on an error (restart), else 1;
 * *ready when a bit is complete (its low, then its high). */
static int cyfral_bit(d, level, us, ready, value)
struct ibk_dec *d;
int level;
unsigned long us;
int *ready, *value;
{
	*ready = 0;
	if (d->bit_state == BIT_WAIT_LOW) {
		if (!level) return 0;
		d->period += us;
		*ready = 1;
		d->bit_state = BIT_WAIT_HIGH;
		if (d->period > CYFRAL_MAX_PERIOD_US) return 0;
		*value = !(d->period / 2 > us);
		return 1;
	}
	if (level) return 0;
	d->period = us;
	d->bit_state = BIT_WAIT_LOW;
	return 1;
}

/* protocol_cyfral_decoder_feed */
static int cyfral_feed(d, level, us)
struct ibk_dec *d;
int level;
unsigned long us;
{
	unsigned long data;
	int ready, value;

	if (!cyfral_bit(d, level, us, &ready, &value)) {
		ibk_start(d, IBK_CYFRAL);
		return 0;
	}
	if (!ready) return 0;
	switch (d->state) {
	case CY_WAIT_START:
		d->nibble = (unsigned char) ((d->nibble << 1 | value) & 0xF);
		if (d->nibble == 1) {
			d->nibble = 0;
			d->state = CY_READ_NIBBLE;
		}
		break;
	case CY_READ_NIBBLE:
		d->nibble = (unsigned char) (d->nibble << 1 | value);
		if (++d->bit_index == 4) {
			switch (d->nibble) {
			case 0xE: d->data = d->data << 2 | 3; break;
			case 0xD: d->data = d->data << 2 | 2; break;
			case 0xB: d->data = d->data << 2 | 1; break;
			case 0x7: d->data = d->data << 2 | 0; break;
			default: d->valid = 0; break;
			}
			d->data &= 0xFFFF;
			d->nibble = 0;
			d->bit_index = 0;
			d->index++;
		}
		if (d->index == 8) d->state = CY_READ_STOP;
		break;
	case CY_READ_STOP:
		d->nibble = (unsigned char) ((d->nibble << 1 | value) & 0xF);
		if (++d->bit_index < 4) break;
		if (d->bit_index == 4 && d->nibble == 1 && d->valid) {
			data = d->data;
			ibk_start(d, IBK_CYFRAL);	/* the next key afresh */
			value_key(d, data);
			return 1;
		}
		ibk_start(d, IBK_CYFRAL);
		break;
	}
	return 0;
}

/* metakom_process_bit: a bit is a high (stored), then a low; 1 when the
 * low has come, with both times. */
static int metakom_bit(d, level, us, high, low)
struct ibk_dec *d;
int level;
unsigned long us, *high, *low;
{
	if (d->bit_state == BIT_WAIT_LOW) {
		if (level) return 0;
		*low = d->low;
		*high = us;
		d->bit_state = BIT_WAIT_HIGH;
		return 1;
	}
	if (level) {
		d->low = us;
		d->bit_state = BIT_WAIT_LOW;
	}
	return 0;
}

/* protocol_metakom_decoder_feed.  (Unleashed's names: what it calls the
 * low time is the bit's first, high segment.) */
static int metakom_feed(d, level, us)
struct ibk_dec *d;
int level;
unsigned long us;
{
	unsigned long high = 0, low = 0, data;
	int i;

	if (!metakom_bit(d, level, us, &high, &low)) return 0;
	switch (d->state) {
	case MK_WAIT_SYNC:
		d->sample[d->index++] = high + low;
		if (d->index == METAKOM_SAMPLES) {
			for (i = 0; i < METAKOM_SAMPLES; i++)
				d->period += d->sample[i];
			d->period /= METAKOM_SAMPLES;
			d->index = 0;
			d->state = MK_WAIT_START_BIT;
		}
		break;
	case MK_WAIT_START_BIT:
		d->count++;
		if (high > d->period) {
			d->count = 0;
			d->state = MK_WAIT_START_WORD;
		}
		if (d->count > 40) ibk_start(d, IBK_METAKOM);
		break;
	case MK_WAIT_START_WORD:
	case MK_READ_WORD:
	case MK_READ_STOP:
		d->tmp = (unsigned char) (d->tmp << 1 | !(low < d->period / 2));
		d->count++;
		if (d->state == MK_READ_WORD) {
			if (d->count != 8) break;
			if (!even_parity(d->tmp)) {
				ibk_start(d, IBK_METAKOM);
				break;
			}
			d->data = (d->data << 8 | d->tmp) & 0xFFFFFFFFUL;
			d->tmp = d->count = 0;
			if (++d->index == 4) {
				if (high > d->period)	/* the stop: a sync */
					d->state = MK_READ_STOP;
				else
					ibk_start(d, IBK_METAKOM);
			}
			break;
		}
		if (d->count != 3) break;
		if ((d->tmp & 7) != 2) {		/* not 0b010 */
			ibk_start(d, IBK_METAKOM);
			break;
		}
		if (d->state == MK_READ_STOP) {
			data = d->data;
			ibk_start(d, IBK_METAKOM);	/* the next key afresh */
			value_key(d, data);
			return 1;
		}
		d->tmp = d->count = 0;
		d->state = MK_READ_WORD;
		break;
	}
	return 0;
}

int ibk_feed(d, level, us)
struct ibk_dec *d;
int level;
unsigned long us;
{
	if (d->type == IBK_CYFRAL) return cyfral_feed(d, level, us);
	if (d->type == IBK_METAKOM) return metakom_feed(d, level, us);
	return 0;
}

int ibk_decode(dur, n, level, key)
const unsigned short *dur;
int n, level;
unsigned char *key;
{
	struct ibk_dec d;
	int t, i;

	for (t = 1; t <= IBK_NTYPES; t++) {
		ibk_start(&d, t);
		for (i = 0; i < n; i++) {
			if (ibk_feed(&d, (level ^ i) & 1, (unsigned long) dur[i])) {
				memcpy(key, d.key, (size_t) ibk_size(t));
				return t;
			}
		}
	}
	return 0;
}


/*==========================================================================*
 *				Dallas families				    *
 *==========================================================================*/
static const struct ibd_fam fams[IBD_NFAM] = {
	{ "DS1990", 0x01, IBD_NOMEM, 0, 0, 0 },
	{ "DS1992", 0x08, IBD_SRAM, 2, 4, 128 },
	{ "DS1996", 0x0C, IBD_SRAM, 2, 32, 8192 },
	{ "DS1971", 0x14, IBD_EEPROM, 1, 32, 32 },
	{ "DS1420", 0x81, IBD_NOMEM, 0, 0, 0 },
	{ "DSGeneric", 0x00, IBD_NOMEM, 0, 0, 0 }
};

const struct ibd_fam *ibd_byname(name)
const char *name;
{
	int i;

	if (strcmp(name, "Dallas") == 0) return &fams[0];
	if (strcmp(name, "(non-specific)") == 0) return &fams[IBD_GENERIC];
	for (i = 0; i < IBD_NFAM; i++)
		if (strcmp(name, fams[i].name) == 0) return &fams[i];
	return NULL;
}

const struct ibd_fam *ibd_bycode(code)
int code;
{
	int i;

	for (i = 0; i < IBD_GENERIC; i++)
		if (fams[i].code == (code & 0xFF)) return &fams[i];
	return &fams[IBD_GENERIC];
}

const char *ibd_memkey(f)
const struct ibd_fam *f;
{
	return f->mem == IBD_EEPROM ? "Eeprom Data" : "Sram Data";
}

int ibd_crc8(b, n)
const unsigned char *b;
int n;
{
	int crc = 0, i, j;

	for (i = 0; i < n; i++) {
		crc ^= b[i];
		for (j = 0; j < 8; j++)
			crc = (crc & 1) ? (crc >> 1) ^ 0x8C : crc >> 1;
	}
	return crc;
}

/*==========================================================================*
 *				TM01x frames				    *
 *==========================================================================*/
/* blanks/tm01x.c: tm01x_make_image; the code is Metakom's data, or
 * Cyfral's encoded word reversed bit for bit; the prefix is 1 / 2. */
static unsigned long rev32(v)
unsigned long v;
{
	unsigned long r = 0;
	int i;

	for (i = 0; i < 32; i++) r |= (v >> i & 1UL) << (31 - i);
	return r;
}

static unsigned rev8(v)
unsigned v;
{
	unsigned r = 0;
	int i;

	for (i = 0; i < 8; i++) r |= (v >> i & 1u) << (7 - i);
	return r;
}

int ibk_tm01x_image(type, key, rom)
int type;
const unsigned char *key;
unsigned char *rom;
{
	unsigned long code;
	unsigned prefix;

	if (ibk_valid(type, key) != 1) return -1;
	if (type == IBK_CYFRAL) {
		code = rev32(cyfral_encode(key_value(type, key)));
		prefix = 1;
	} else {
		code = key_value(type, key);
		prefix = 2;
	}
	rom[0] = (unsigned char) (prefix << 4 | (code >> 28 & 0x0F));
	rom[1] = (unsigned char) (code >> 20);
	rom[2] = (unsigned char) (code >> 12);
	rom[3] = (unsigned char) (code >> 4);
	rom[4] = (unsigned char) (code << 4);
	rom[5] = rom[6] = rom[7] = 0;
	return 0;
}

void ibk_tm01x_norm(rom)
unsigned char *rom;
{
	int i;

	for (i = 0; i < 5; i++) rom[i] = (unsigned char) ~rev8(rom[i]);
	rom[4] &= 0xF0;
	rom[5] = rom[6] = rom[7] = 0;
}
