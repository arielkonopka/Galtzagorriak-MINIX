/* rgblob.c - the compiled region record (spec Q.3, RQ.2; task T32a).
 *
 * `region set` turns a parsed table (region.c) into one binary record,
 * which MM writes once into the installed region (T32b) and the kernel
 * reads in place (XIP, RQ.3).  Every field is written byte by byte,
 * little-endian, so the host tests, the PC tools and the Cortex-M4 see the
 * same bytes; the row layout keeps every field naturally aligned, so the
 * kernel can read a row as a C struct (struct mdm_rgrec) straight from
 * flash.  Only the active rows go in: "#?" rows are never used (Q.2).
 *
 *	header, 16 B:	0 "RGN1"  4 version (1)  5 rows  6 code[2]
 *			8 CRC-32 (IEEE, of the whole record with these 4
 *			  bytes zero)  12 size (u16)  14 0, 0
 *	row, 16 B:	0 lo Hz (u32)  4 hi Hz (u32)  8 dBm (s8)  9 use (u8)
 *			10 duty in 0.01 % (u16, 10000 = none)
 *			12 bw kHz (u16, 0 = any)  14 flags (u8)  15 0
 *
 * At most 32 rows: <= 528 B.  No malloc, no statics: 0 B RAM.
 */
#include <string.h>
#include "modem.h"

static void put16(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char) (v & 0xFF);
	p[1] = (unsigned char) (v >> 8 & 0xFF);
}

static void put32(unsigned char *p, unsigned long v)
{
	put16(p, (unsigned) (v & 0xFFFF));
	put16(p + 2, (unsigned) (v >> 16 & 0xFFFF));
}

static unsigned get16(const unsigned char *p)
{
	return (unsigned) p[0] | (unsigned) p[1] << 8;
}

static unsigned long get32(const unsigned char *p)
{
	return (unsigned long) get16(p) | (unsigned long) get16(p + 2) << 16;
}

unsigned long mdm_crc32(const unsigned char *p, int n, unsigned long crc)
{
	/* IEEE 802.3, reflected, bit by bit: no table in RAM or flash */
	int k;

	crc = ~crc & 0xFFFFFFFFUL;
	while (n-- > 0) {
		crc ^= *p++;
		for (k = 0; k < 8; k++)
			crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320UL : crc >> 1;
	}
	return ~crc & 0xFFFFFFFFUL;
}

/* CRC of the record with its CRC field taken as zero */
static unsigned long blob_crc(const unsigned char *b, int len)
{
	static const unsigned char zero[4] = { 0, 0, 0, 0 };
	unsigned long c;

	c = mdm_crc32(b, 8, 0);
	c = mdm_crc32(zero, 4, c);
	return mdm_crc32(b + 12, len - 12, c);
}

int mdm_rg_compile(const struct mdm_region *rg, unsigned char *out, int max)
{
	const struct mdm_rgrow *r;
	unsigned char *p;
	int i, n = 0, len;

	if (rg->code[0] == 0) return -1;
	for (i = 0; i < rg->n; i++)
		if (!(rg->row[i].flags & MDM_RG_UNCHECKED)) n++;
	if (n == 0 || n > MDM_RG_ROWS) return -1;
	len = MDM_RGB_HDR + n * MDM_RGB_ROW;
	if (len > max) return -1;
	memset(out, 0, (size_t) len);
	memcpy(out, "RGN1", 4);
	out[4] = MDM_RGB_VERSION;
	out[5] = (unsigned char) n;
	out[6] = (unsigned char) rg->code[0];
	out[7] = (unsigned char) rg->code[1];
	put16(out + 12, (unsigned) len);
	p = out + MDM_RGB_HDR;
	for (i = 0; i < rg->n; i++) {
		r = &rg->row[i];
		if (r->flags & MDM_RG_UNCHECKED) continue;
		if (r->dbm > 10 || r->dbm < -128 || r->use < 0 || r->use > 3
		    || r->duty_pm <= 0 || r->duty_pm > 1000
		    || r->bw_khz > 0xFFFFU || r->lo >= r->hi)
			return -1;
		put32(p, r->lo);
		put32(p + 4, r->hi);
		p[8] = (unsigned char) (r->dbm & 0xFF);
		p[9] = (unsigned char) r->use;
		put16(p + 10, (unsigned) r->duty_pm * 10U);
		put16(p + 12, r->bw_khz);
		p[14] = 0;		/* RQ.8: a later NFC/RFID row kind */
		p += MDM_RGB_ROW;
	}
	put32(out + 8, blob_crc(out, len));
	return len;
}

int mdm_rg_blob_check(const unsigned char *b, int len)
{
	const unsigned char *p;
	int i, n;

	if (len < MDM_RGB_HDR || memcmp(b, "RGN1", 4) != 0
	    || b[4] != MDM_RGB_VERSION)
		return -1;
	n = b[5];
	if (n == 0 || n > MDM_RG_ROWS
	    || (int) get16(b + 12) != MDM_RGB_HDR + n * MDM_RGB_ROW
	    || (int) get16(b + 12) > len)
		return -1;
	len = (int) get16(b + 12);
	if (get32(b + 8) != blob_crc(b, len)) return -1;
	for (i = 0, p = b + MDM_RGB_HDR; i < n; i++, p += MDM_RGB_ROW) {
		if (get32(p) >= get32(p + 4) || (signed char) p[8] > 10
		    || p[9] > 3 || get16(p + 10) == 0 || get16(p + 10) > 10000
		    || get16(p + 10) % 10 != 0)	/* v1: 0.1 % steps */
			return -1;
	}
	return len;
}

unsigned long mdm_rg_blob_crc(const unsigned char *b)
{
	return get32(b + 8);
}

int mdm_rg_blob_load(const unsigned char *b, int len, struct mdm_region *rg)
{
	const unsigned char *p;
	struct mdm_rgrow *r;
	int i;

	if (mdm_rg_blob_check(b, len) < 0) return -1;
	mdm_rg_init(rg);
	rg->code[0] = (char) b[6];
	rg->code[1] = (char) b[7];
	rg->n = b[5];
	for (i = 0, p = b + MDM_RGB_HDR; i < rg->n; i++, p += MDM_RGB_ROW) {
		r = &rg->row[i];
		r->lo = get32(p);
		r->hi = get32(p + 4);
		r->dbm = (signed char) p[8];
		r->use = p[9];
		r->duty_pm = (int) (get16(p + 10) / 10U);
		r->bw_khz = get16(p + 12);
		r->flags = 0;
	}
	return 0;
}
