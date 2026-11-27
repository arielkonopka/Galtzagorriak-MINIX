/* ibkey.h - Cyfral and Metakom iButton keys (Galtzagorriak MINIX, spec
 * R6.7 part 3b): the line code of the Unleashed firmware's
 * lib/ibutton/protocols/misc/protocol_cyfral.c and protocol_metakom.c,
 * decoder and encoder, as a small library for ikey and its host test.
 *
 * Both keys send their code round and round once a reader powers them;
 * the reader sees the line's levels as durations.  Here a "segment" is
 * one level on the line: level 1 high, 0 low, and how long it lasted
 * (microseconds).  Segments alternate.
 *
 * A key is its bytes as Unleashed keeps them and as its CLI prints them:
 * the protocol's integer (Cyfral 16 bits, Metakom 32) least significant
 * byte first.  "Cyfral A1B2" is the bytes A1 B2.
 *
 * No malloc, no I/O: the caller owns the decoder state (struct ibk_dec,
 * ~60 B) and the buffers.
 */
#ifndef IBKEY_H
#define IBKEY_H

#define IBK_CYFRAL	1
#define IBK_METAKOM	2
#define IBK_NTYPES	2

#define IBK_MAXKEY	4	/* bytes in the longest key (Metakom) */

/* One frame as a pulse list (ibk_frame) needs at most this many
 * segments: Cyfral 72, Metakom 70. */
#define IBK_MAXFRAME	72

/* Timing (us): both keys use a 125 us bit, split one third / two thirds
 * (Unleashed: 125 us x 0.33 and x 0.66). */
#define IBK_PERIOD_US	125
#define IBK_THIRD_US	41
#define IBK_2THIRD_US	83

struct ibk_dec {
	int type;			/* IBK_* */
	unsigned long data;		/* the code, as it comes in */
	unsigned long period;		/* Cyfral: this bit's low + high;
					 * Metakom: the average bit */
	unsigned long low;		/* Metakom: the stored first half */
	unsigned long sample[10];	/* Metakom: bit periods at the start */
	unsigned char state, bit_state, nibble, valid, index, bit_index;
	unsigned char tmp, count;
	unsigned char key[IBK_MAXKEY];	/* out, once ibk_feed gave 1 */
};

/* "Cyfral" -> IBK_CYFRAL, "Metakom" -> IBK_METAKOM, else 0. */
int ibk_type(const char *name);
const char *ibk_name(int type);
/* Bytes in a key of this type (2, 4), 0 for an unknown type. */
int ibk_size(int type);
/* 1 if the key can be sent: always for Cyfral; Metakom needs every byte
 * of even parity (its decoder checks it).  -1: an unknown type. */
int ibk_valid(int type, const unsigned char *key);

/* One whole frame of the key as segments, to be played round and round:
 * dur[] gets the durations (us), *level the level of dur[0].  Equal
 * neighbouring levels are joined, also across the end of the list (it
 * is a loop), so the count is even and the levels alternate.  Returns the
 * count (at most IBK_MAXFRAME), or -1 for an unknown type or no room. */
int ibk_frame(int type, const unsigned char *key, unsigned short *dur,
	int *level, int max);

/* The decoder: start it, then feed it the segments in order.  ibk_feed
 * gives 1 when a whole key has come in (then d->key holds it, ibk_size
 * bytes); feeding on starts the next key from scratch. */
void ibk_start(struct ibk_dec *d, int type);
int ibk_feed(struct ibk_dec *d, int level, unsigned long us);

/* Feed a capture (dur[n], first level `level`) to every decoder; the
 * first key found goes to key[] and its type is returned, 0 none. */
int ibk_decode(const unsigned short *dur, int n, int level,
	unsigned char *key);

/*==========================================================================*
 * Dallas families (spec R6.7 part 4, issue #490), Unleashed's
 * lib/ibutton/protocols/dallas: DS1990, DS1992, DS1996, DS1971, DS1420 and
 * "DSGeneric" (any other family code).  The table says what a family has:
 * its name, its family code and its memory.  The .ibtn file calls the
 * memory "Sram Data" or "Eeprom Data" (ibd_memkey).
 *==========================================================================*/
#define IBD_NFAM	6
#define IBD_GENERIC	5	/* the last row: no family of its own */
#define IBD_NOMEM	0
#define IBD_SRAM	1
#define IBD_EEPROM	2

struct ibd_fam {
	const char *name;
	unsigned char code;	/* family code (ROM byte 0) */
	unsigned char mem;	/* IBD_NOMEM, IBD_SRAM, IBD_EEPROM */
	unsigned char abytes;	/* address bytes of READ MEMORY (1 or 2) */
	unsigned char page;	/* bytes the scratchpad takes at a time */
	unsigned short size;	/* memory bytes */
};

/* "DS1992" -> its row; the older "Dallas" is DS1990, "DSGeneric" and
 * "(non-specific)" the generic row; NULL: not a Dallas name. */
const struct ibd_fam *ibd_byname(const char *name);
/* The row of a family code; an unknown code gets the generic row. */
const struct ibd_fam *ibd_bycode(int code);
const char *ibd_memkey(const struct ibd_fam *f);
/* Maxim CRC8 (the ROM's last byte) of n bytes. */
int ibd_crc8(const unsigned char *b, int n);

/* A TM01x blank (spec R6.7 part 4) holds a Cyfral or Metakom key as the
 * 36-bit frame Unleashed's blanks/tm01x.c writes: ibk_tm01x_image gives
 * the 8 ROM bytes to shift in (most significant bit first, each bit
 * inverted on the line), -1 for a key it cannot write (an unknown type,
 * a Metakom byte of odd parity).  ibk_tm01x_norm turns what READ ROM gave
 * back into the same form, to compare. */
int ibk_tm01x_image(int type, const unsigned char *key, unsigned char *rom);
void ibk_tm01x_norm(unsigned char *rom);
#define IBK_TM01X_BITS	36

#endif /* IBKEY_H */
