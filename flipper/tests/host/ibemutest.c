/* Host test of the Dallas key's 1-Wire slave (src/kernel/arm/ibemu.c,
 * spec R6.7 part 3): a simulated reader drives slots through ie_step()
 * as the contact's edges would, and checks what the key answers.
 * READ ROM (0x33 and 0x0F), SEARCH ROM on our branch and off it, an
 * unknown command, a reset in the middle, a missed falling edge. */
#include <stdio.h>
#include <string.h>

typedef unsigned char u8_t;
typedef unsigned short u16_t;
#define PRIVATE static
#include "../../../src/kernel/arm/ibemu.c"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); \
	fails++; } else printf("  ok   %s\n", what); } while (0)

static struct ie_state e;

/* A reset: 480 us low.  1 if the key answered with presence. */
static int reset(void)
{
	(void) ie_step(&e, 0, 0);
	return ie_step(&e, 1, 480) == IE_PRESENCE;
}

/* The reader writes a bit: 6 us low for a 1, 60 us for a 0. */
static void wbit(int b)
{
	(void) ie_step(&e, 0, 0);
	(void) ie_step(&e, 1, b ? 6 : 60);
}

static void wbyte(int v)
{
	int i;

	for (i = 0; i < 8; i++) wbit(v >> i & 1);
}

/* The reader reads a bit: 6 us low; the key holds a 0 until 30 us. */
static int rbit(void)
{
	int held = ie_step(&e, 0, 0) == IE_HOLD0;

	(void) ie_step(&e, 1, held ? 30 : 6);
	return !held;
}

static void rrom(u8_t *rom)
{
	int i;

	memset(rom, 0, 8);
	for (i = 0; i < 64; i++) rom[i >> 3] |= (u8_t) (rbit() << (i & 7));
}

/* SEARCH ROM: per bit read bit and complement, write `choose` (or, with
 * choose < 0, the key's own bit).  Returns 1 if every pair said "one key,
 * this bit" and the key followed to the end. */
static int search(int flip_at)
{
	int i, b, c, ok = 1;

	for (i = 0; i < 64; i++) {
		b = rbit();
		c = rbit();
		if (b == c) ok = 0;		/* both 1: nobody; both 0: two */
		wbit(i == flip_at ? !b : b);
	}
	return ok;
}

int main(void)
{
	static const u8_t key[8] = { 0x01, 0x12, 0x34, 0x56, 0x78, 0x9A,
				     0x00, 0xC5 };
	u8_t got[8];
	int i;

	memcpy(e.rom, key, 8);
	e.st = IE_IDLE;

	CHECK(ie_step(&e, 0, 0) == 0 && ie_step(&e, 1, 6) == 0
		&& e.st == IE_IDLE, "idle: slots before a reset do nothing");
	CHECK(reset(), "reset: presence");
	wbyte(0x33);
	CHECK(e.st == IE_SEND, "READ ROM 0x33 taken");
	rrom(got);
	CHECK(memcmp(got, key, 8) == 0, "READ ROM: the 64 bits are the ROM");
	CHECK(e.served == 1 && e.st == IE_IDLE, "READ ROM: served, then idle");
	CHECK(rbit() == 1, "after the ROM: the key is quiet (reads 1)");

	CHECK(reset(), "reset again");
	wbyte(0x0F);
	rrom(got);
	CHECK(memcmp(got, key, 8) == 0 && e.served == 2, "old READ ROM 0x0F");

	CHECK(reset(), "reset for SEARCH ROM");
	wbyte(0xF0);
	CHECK(e.st == IE_SEARCH, "SEARCH ROM 0xF0 taken");
	CHECK(search(-1) && e.served == 3 && e.st == IE_IDLE,
		"SEARCH ROM on our branch: bit and complement, to the end");

	CHECK(reset(), "reset for a SEARCH off our branch");
	wbyte(0xF0);
	(void) search(10);
	CHECK(e.st == IE_IDLE && e.served == 3,
		"SEARCH ROM: another branch at bit 10, the key drops out");

	CHECK(reset(), "reset for an unknown command");
	wbyte(0xCC);
	CHECK(e.st == IE_IDLE && rbit() == 1, "SKIP ROM 0xCC: quiet");

	CHECK(reset(), "reset, READ ROM cut by a reset");
	wbyte(0x33);
	for (i = 0; i < 20; i++) (void) rbit();
	CHECK(reset() && e.st == IE_CMD && e.bit == 0,
		"a reset in the middle starts over");
	wbyte(0x33);
	rrom(got);
	CHECK(memcmp(got, key, 8) == 0, "... and the ROM is read whole");

	/* the interrupt came after a short pulse was over: only a rising
	 * edge, low time unknown (0).  On a 0 bit the key could not answer:
	 * it drops out until the next reset. */
	CHECK(reset(), "reset, missed fall on a 0 bit");
	wbyte(0x33);
	(void) ie_step(&e, 1, 0);	/* bit 0 of 0x01 is 1: fine */
	CHECK(e.st == IE_SEND && e.bit == 1, "missed fall on a 1 bit: on");
	(void) ie_step(&e, 1, 0);	/* bit 1 is 0: missed */
	CHECK(e.st == IE_IDLE, "missed fall on a 0 bit: quiet until reset");

	CHECK(reset(), "reset, a command written with missed falls");
	for (i = 0; i < 8; i++) (void) ie_step(&e, 1, 0);	/* 0xFF */
	CHECK(e.st == IE_IDLE, "missed falls read as 1s (0xFF: unknown)");

	e.st = IE_OFF;
	CHECK(ie_step(&e, 0, 0) == 0 && ie_step(&e, 1, 480) == 0,
		"off: no presence");

	printf(fails ? "ibemutest: %d FAILED\n" : "ibemutest: PASS\n", fails);
	return fails != 0;
}
