/* crypto1.c - the MIFARE Classic Crypto-1 stream cipher.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Galtzagorriak MINIX, spec Phase 14, issue #387.  See crypto1.h.
 *
 * Public algorithm (Garcia et al.; Crapto1, GPL-2.0-or-later -> GPL-3.0
 * here).  48-bit LFSR split into the odd and even bit halves so that one
 * clock touches a 24-bit word; the nonlinear filter f() taps five nibbles.
 */
#include "crypto1.h"

#define BIT(x, n)	((uint32_t)((x) >> (n)) & 1u)

/* Feedback taps, split for the odd/even halves (standard polynomials). */
#define LF_POLY_ODD	0x29CE5Cu
#define LF_POLY_EVEN	0x870804u

/* The nonlinear filter: five 4-bit lookups into fa/fb, combined by fc. */
uint32_t crypto1_filter(uint32_t x)
{
	uint32_t f;
	f  = 0xf22c0u >> (x & 0xf) & 16u;
	f |= 0x6c9c0u >> (x >> 4 & 0xf) & 8u;
	f |= 0x3c8b0u >> (x >> 8 & 0xf) & 4u;
	f |= 0x1e458u >> (x >> 12 & 0xf) & 2u;
	f |= 0x0d938u >> (x >> 16 & 0xf) & 1u;
	return BIT(0xEC57E80Au, f);
}

void crypto1_reset(crypto1_t *c)
{
	c->odd = 0;
	c->even = 0;
}

void crypto1_init(crypto1_t *c, uint64_t key)
{
	int i;
	crypto1_reset(c);
	/* Load the 48 key bits: even LFSR positions into `even`, odd into
	 * `odd`, high key bit first, as the air order has it. */
	for(i = 47; i > 0; i -= 2) {
		c->odd = c->odd << 1 | BIT(key, (i - 1) ^ 7);
		c->even = c->even << 1 | BIT(key, i ^ 7);
	}
}

uint8_t crypto1_bit(crypto1_t *c, uint8_t in, int is_encrypted)
{
	uint32_t out = crypto1_filter(c->odd);
	uint32_t feed = out & (is_encrypted ? 1u : 0u);

	feed ^= (in ? 1u : 0u);
	feed ^= LF_POLY_ODD & c->odd;
	feed ^= LF_POLY_EVEN & c->even;
	/* parity of feed */
	feed ^= feed >> 16;
	feed ^= feed >> 8;
	feed ^= feed >> 4;
	feed ^= feed >> 2;
	feed ^= feed >> 1;

	c->even = c->even << 1 | (feed & 1u);
	/* swap halves: the even half becomes the odd for the next clock */
	{
		uint32_t t = c->odd;
		c->odd = c->even;
		c->even = t;
	}
	return (uint8_t)out;
}

uint8_t crypto1_byte(crypto1_t *c, uint8_t in, int is_encrypted)
{
	uint8_t ret = 0;
	int i;
	for(i = 0; i < 8; i++)
		ret |= crypto1_bit(c, BIT(in, i), is_encrypted) << i;
	return ret;
}

uint32_t crypto1_word(crypto1_t *c, uint32_t in, int is_encrypted)
{
	uint32_t ret = 0;
	int i;
	for(i = 0; i < 32; i++)
		ret |= (uint32_t)crypto1_bit(c, BIT(in, i ^ 24), is_encrypted) << (24 ^ i);
	return ret;
}

/* SWAPENDIAN, as in Crapto1: swap the byte pairs, then the halves. */
static uint32_t swapendian(uint32_t x)
{
	x = (x >> 8 & 0x00ff00ffu) | (x & 0x00ff00ffu) << 8;
	x = x >> 16 | x << 16;
	return x;
}

/* The tag's 16-bit nonce PRNG, advanced n clocks.  Taps at 16,18,19,21 in
 * the endian-swapped word, exactly the standard prng_successor(). */
uint32_t crypto1_prng_successor(uint32_t x, uint32_t n)
{
	x = swapendian(x);
	while(n--)
		x = x >> 1 | (BIT(x, 16) ^ BIT(x, 18) ^ BIT(x, 19) ^ BIT(x, 21)) << 31;
	return swapendian(x);
}
