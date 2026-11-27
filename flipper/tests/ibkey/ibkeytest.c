/* ibkeytest - libibkey on the PC (spec R6.7 part 3b).
 *
 * For every Cyfral key and a few thousand Metakom keys: the pulse list
 * ibk_frame() gives (what IBIOCPLAY plays) is looped as the line a reader
 * sees, cut at many starting points, with jitter on every edge, and the
 * decoders (what `ikey read` runs on an IBIOCCAPT capture) must give the
 * same key back, as the same type.  Then garbage, a wrong bit rate and
 * Metakom bytes with odd parity must give nothing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ibkey.h"

#define CAPT	256		/* ikey's capture: 256 durations */

static int fails, checks;
static unsigned long rnd = 12345;

static unsigned rand32(void)
{
	rnd = rnd * 1103515245UL + 12345UL;
	return (unsigned) (rnd >> 8 & 0xFFFFFF);
}

static void check(int ok, const char *what)
{
	checks++;
	if (!ok) {
		fails++;
		printf("FAIL %s\n", what);
	}
}

static void hex(char *s, const unsigned char *k, int n)
{
	int i;

	for (i = 0; i < n; i++) sprintf(s + 2 * i, "%02X", k[i]);
}

/* The line a reader sees: the frame looped from segment `off`, jitter up
 * to +-j us on each segment; returns the first level. */
static int line(const unsigned short *f, int n, int lv, int off, int j,
	unsigned short *cap, int ncap)
{
	int i, d;

	for (i = 0; i < ncap; i++) {
		d = f[(off + i) % n];
		if (j > 0) d += (int) (rand32() % (2 * j + 1)) - j;
		cap[i] = (unsigned short) d;
	}
	return (lv ^ off) & 1;
}

/* One key: its frame, then decoded from `tries` different starts. */
static int round_trip(int type, const unsigned char *key, int tries,
	int jitter)
{
	unsigned short f[IBK_MAXFRAME], cap[CAPT];
	unsigned char got[IBK_MAXKEY];
	char a[16], b[16], msg[96];
	int n, lv, i, t, l2, ok = 1;

	n = ibk_frame(type, key, f, &lv, IBK_MAXFRAME);
	if (n != (type == IBK_CYFRAL ? 72 : 70)) {
		sprintf(msg, "%s frame: %d segments", ibk_name(type), n);
		check(0, msg);
		return 0;
	}
	for (i = 0; i < tries; i++) {
		l2 = line(f, n, lv, (int) (rand32() % (unsigned) n), jitter,
			cap, CAPT);
		memset(got, 0, sizeof(got));
		t = ibk_decode(cap, CAPT, l2, got);
		if (t != type || memcmp(got, key, (size_t) ibk_size(type)) != 0) {
			hex(a, key, ibk_size(type));
			hex(b, got, ibk_size(type));
			sprintf(msg, "%s %s: decoded as %s %s", ibk_name(type), a,
				t ? ibk_name(t) : "nothing", t ? b : "");
			check(0, msg);
			ok = 0;
			break;
		}
	}
	return ok;
}

static void vectors(void)
{
	/* twoflip's (Unleashed CLI: ikey emulate Cyfral A1B2 ...) */
	static const unsigned char cy[2] = { 0xA1, 0xB2 };
	static const unsigned char mk[4] = { 0x03, 0x05, 0x09, 0x0A };
	static const unsigned short start[8] = { 83, 41, 83, 41, 83, 41, 41, 83 };
	unsigned short f[IBK_MAXFRAME];
	int lv, n, i;
	unsigned long sum = 0;

	check(ibk_type("Cyfral") == IBK_CYFRAL && ibk_type("Metakom") ==
		IBK_METAKOM && ibk_type("Dallas") == 0, "type names");
	check(ibk_size(IBK_CYFRAL) == 2 && ibk_size(IBK_METAKOM) == 4,
		"key sizes");
	n = ibk_frame(IBK_CYFRAL, cy, f, &lv, IBK_MAXFRAME);
	check(n == 72 && lv == 0 && memcmp(f, start, sizeof(start)) == 0,
		"Cyfral frame: low first, the start nibble 0001");
	for (i = 0; i < n; i++) sum += f[i];
	check(sum == 36 * (IBK_THIRD_US + IBK_2THIRD_US),
		"Cyfral frame: 36 bits of 124 us");
	check(ibk_frame(IBK_CYFRAL, cy, f, &lv, 10) == -1,
		"Cyfral frame: no room -> -1");
	n = ibk_frame(IBK_METAKOM, mk, f, &lv, IBK_MAXFRAME);
	check(n == 70 && lv == 1, "Metakom frame: 70, high first (the sync "
		"joined to the last low)");
	for (sum = 0, i = 0; i < n; i++) sum += f[i];
	check(sum == 35 * (IBK_THIRD_US + IBK_2THIRD_US) + IBK_PERIOD_US,
		"Metakom frame: sync + 35 bits");
	check(f[n - 1] > IBK_PERIOD_US, "Metakom frame: the last low holds "
		"the sync");
	check(ibk_frame(0, cy, f, &lv, IBK_MAXFRAME) == -1, "unknown type");
	check(ibk_valid(IBK_METAKOM, mk) == 1, "Metakom 0305090A: even "
		"parity");
	check(round_trip(IBK_CYFRAL, cy, 72, 0), "Cyfral A1B2 round trip");
	check(round_trip(IBK_METAKOM, mk, 70, 0), "Metakom 0305090A round trip");
	check(round_trip(IBK_CYFRAL, cy, 200, 8), "Cyfral A1B2, +-8 us");
	check(round_trip(IBK_METAKOM, mk, 200, 8), "Metakom 0305090A, +-8 us");
}

static void every_cyfral(void)
{
	unsigned char k[2];
	unsigned v;
	int bad = 0;

	for (v = 0; v < 0x10000 && bad < 5; v++) {
		k[0] = (unsigned char) v;
		k[1] = (unsigned char) (v >> 8);
		if (!round_trip(IBK_CYFRAL, k, 2, 5)) bad++;
	}
	check(bad == 0, "all 65536 Cyfral keys, +-5 us");
}

static unsigned char even_byte(void)
{
	unsigned b;
	int n, i;

	for (;;) {
		b = rand32() & 0xFF;
		for (n = i = 0; i < 8; i++) n += b >> i & 1;
		if (n % 2 == 0) return (unsigned char) b;
	}
}

static void many_metakom(void)
{
	unsigned char k[4];
	int i, j, bad = 0;

	for (i = 0; i < 5000 && bad < 5; i++) {
		for (j = 0; j < 4; j++) k[j] = even_byte();
		if (ibk_valid(IBK_METAKOM, k) != 1) bad++;
		else if (!round_trip(IBK_METAKOM, k, 4, 5)) bad++;
	}
	check(bad == 0, "5000 Metakom keys, +-5 us");
	k[0] = 0x01; k[1] = k[2] = k[3] = 0;
	check(ibk_valid(IBK_METAKOM, k) == 0, "Metakom 01000000: odd parity");
}

static void negatives(void)
{
	static unsigned short cap[20000];
	static const unsigned char cy[2] = { 0x34, 0x12 };
	unsigned short f[IBK_MAXFRAME], c2[CAPT];
	unsigned char got[IBK_MAXKEY];
	int i, n, lv, l2, hits = 0;

	for (i = 0; i < 20000; i++) cap[i] = (unsigned short) (10 + rand32() % 290);
	for (i = 0; i + CAPT <= 20000; i += CAPT / 2)
		if (ibk_decode(cap + i, CAPT, i & 1, got)) hits++;
	check(hits == 0, "random durations decode as nothing");
	n = ibk_frame(IBK_CYFRAL, cy, f, &lv, IBK_MAXFRAME);
	for (i = 0; i < n; i++) f[i] = (unsigned short) (f[i] * 2);
	l2 = line(f, n, lv, 0, 0, c2, CAPT);
	check(ibk_decode(c2, CAPT, l2, got) == 0,
		"Cyfral at half the bit rate (250 us > 230 us): nothing");
	for (i = 0; i < CAPT; i++) c2[i] = 60;
	check(ibk_decode(c2, CAPT, 0, got) == 0, "a square wave: nothing");
	check(ibk_decode(c2, 0, 0, got) == 0, "an empty capture: nothing");
}

/* Dallas families (issue #490) and the TM01x frames. */
static void families(void)
{
	static const unsigned char cy[2] = { 0xA1, 0xB2 };
	static const unsigned char mk[4] = { 0x03, 0x05, 0x09, 0x0A };
	static const unsigned char odd[4] = { 0x01, 0x05, 0x09, 0x0A };
	/* Unleashed's tm01x_make_image for these (computed apart, in Python,
	 * from protocol_cyfral_encoder_encode and bit_lib_reverse_32) */
	static const unsigned char cy_img[8] =
		{ 0x1D, 0xE7, 0xDD, 0xD7, 0xB0, 0, 0, 0 };
	static const unsigned char mk_img[8] =
		{ 0x20, 0xA0, 0x90, 0x50, 0x30, 0, 0, 0 };
	static const unsigned char rom[8] =
		{ 0x01, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x8F };
	unsigned char img[8], got[8];
	const struct ibd_fam *f;
	int i, t, k;

	check(ibd_byname("DS1992") && ibd_byname("DS1992")->size == 128
	      && ibd_byname("DS1992")->mem == IBD_SRAM
	      && ibd_byname("DS1992")->page == 4, "DS1992: 128 B SRAM, pages of 4");
	f = ibd_byname("DS1996");
	check(f && f->code == 0x0C && f->size == 8192 && f->page == 32
	      && f->abytes == 2, "DS1996: 8 KB, pages of 32, 2 address bytes");
	f = ibd_byname("DS1971");
	check(f && f->code == 0x14 && f->mem == IBD_EEPROM && f->size == 32
	      && f->abytes == 1, "DS1971: 32 B EEPROM, 1 address byte");
	check(ibd_byname("Dallas") == ibd_byname("DS1990"),
		"the old name Dallas is DS1990");
	check(ibd_byname("DSGeneric") == ibd_byname("(non-specific)")
	      && ibd_byname("DSGeneric") == ibd_bycode(0x55),
		"DSGeneric: any other family code");
	check(ibd_bycode(0x81) == ibd_byname("DS1420")
	      && ibd_bycode(0x08) == ibd_byname("DS1992"), "family by code");
	check(ibd_byname("Cyfral") == NULL && ibd_byname("DS9999") == NULL,
		"not a Dallas name");
	check(strcmp(ibd_memkey(ibd_byname("DS1971")), "Eeprom Data") == 0
	      && strcmp(ibd_memkey(ibd_byname("DS1996")), "Sram Data") == 0,
		"the file keys of the memory");
	check(ibd_crc8(rom, 7) == 0x8F, "CRC8 of 01 A1 B2 C3 D4 E5 F6 is 8F");
	/* the stock DS1990A example: 01 00 00 00 00 00 00 + CRC 0x3D? is
	 * not known here; CRC of 8 bytes with its CRC is 0 */
	check(ibd_crc8(rom, 8) == 0, "CRC8 over a whole ROM is 0");

	check(ibk_tm01x_image(IBK_CYFRAL, cy, img) == 0
	      && memcmp(img, cy_img, 8) == 0, "TM01x image of Cyfral A1B2");
	check(ibk_tm01x_image(IBK_METAKOM, mk, img) == 0
	      && memcmp(img, mk_img, 8) == 0, "TM01x image of Metakom 0305090A");
	check(ibk_tm01x_image(IBK_METAKOM, odd, img) == -1,
		"TM01x: Metakom of odd parity is refused");
	check(ibk_tm01x_image(0, cy, img) == -1, "TM01x: unknown type");
	/* what READ ROM gives back from a blank: the 36 frame bits inverted,
	 * the first one the least significant bit of byte 0; norm restores */
	for (t = 0; t < 2; t++) {
		(void) ibk_tm01x_image(t ? IBK_METAKOM : IBK_CYFRAL,
					t ? mk : cy, img);
		memset(got, 0, sizeof got);
		for (i = 0; i < 64; i++) {
			k = i < IBK_TM01X_BITS ? !(img[i >> 3] >> (7 - (i & 7)) & 1)
							: 1;
			got[i >> 3] |= (unsigned char) (k << (i & 7));
		}
		ibk_tm01x_norm(got);
		check(memcmp(got, img, 8) == 0, t ? "TM01x read back: Metakom"
						   : "TM01x read back: Cyfral");
	}
}

int main(void)
{
	vectors();
	every_cyfral();
	many_metakom();
	negatives();
	families();
	printf("ibkeytest: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
