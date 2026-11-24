/* iso_dep.c - ISO 14443-4 for the reader (spec Phase 14, issue #451).
 * See iso_dep.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * PCB coding (ISO/IEC 14443-4 7.1.1.1), without CID/NAD:
 *   I-block  0x02 | chaining 0x10 | bn
 *   R(ACK)   0xA2 | bn          R(NAK)  0xB2 | bn
 *   S(DESELECT) 0xC2            S(WTX)  0xF2, INF = WTXM (6 bits)
 */
#include "iso_dep.h"
#include <string.h>

#define PCB_I		0x02
#define PCB_CHAIN	0x10
#define PCB_RACK	0xA2
#define PCB_RNAK	0xB2
#define PCB_DESEL	0xC2
#define PCB_WTX		0xF2
#define FWI_DEFAULT	4
#define FWT(fwi)	((uint32_t)4096u << (fwi))	/* 256 * 16 / fc * 2^FWI */
#define WTX_MAX		64	/* a card asking for more is stuck */
#define XF_CRC		(NFC_XF_CRC_TX | NFC_XF_CRC_RX)

static const uint16_t fsc_tab[9] = { 16, 24, 32, 40, 48, 64, 96, 128, 256 };

uint16_t iso_dep_fsc(uint8_t fsci)
{
	return fsci < 9 ? fsc_tab[fsci] : 256;
}

int iso_dep_parse_ats(iso_dep_t *d, const uint8_t *ats, size_t n)
{
	uint8_t t0, fwi = FWI_DEFAULT;
	size_t i = 2;

	if(n < 1 || ats[0] != n || n > ISO_DEP_ATS_MAX) return NFC_ERR_PROTO;
	memcpy(d->ats, ats, n);
	d->ats_len = (uint8_t)n;
	d->fsc = 32;				/* FSCI 2: no T0 */
	d->fwt = FWT(FWI_DEFAULT);
	if(n < 2) return NFC_OK;
	t0 = ats[1];
	d->fsc = iso_dep_fsc(t0 & 0x0F);
	if(t0 & 0x10) i++;			/* TA(1): bit rates */
	if(t0 & 0x20) {				/* TB(1): FWI, SFGI */
		if(i >= n) return NFC_ERR_PROTO;
		fwi = ats[i] >> 4;
		i++;
	}
	if(t0 & 0x40) i++;			/* TC(1): CID/NAD */
	if(i > n) return NFC_ERR_PROTO;
	if(fwi > 14) fwi = FWI_DEFAULT;		/* 15 is RFU */
	d->fwt = FWT(fwi);
	return NFC_OK;
}

int iso_dep_activate(iso_dep_t *d, const nfc_xfer_t *x)
{
	uint8_t rats[2] = { ISO_DEP_RATS, 0x80 };	/* FSDI 8, CID 0 */
	size_t bits = 0;
	int r;

	memset(d, 0, sizeof *d);
	d->x = x;
	r = nfc_transceive(x, rats, NFC_BITS(2), d->rx, NFC_BITS(sizeof d->rx),
			   &bits, FWT(FWI_DEFAULT), XF_CRC);
	if(r != NFC_OK) return r;
	return iso_dep_parse_ats(d, d->rx, NFC_BYTES(bits));
}

/* Send d->tx[0..txn) and take the card's block into d->rx; *rn its
 * length.  Answers S(WTX); on a lost or broken answer sends R(NAK) once
 * and re-sends our block if the card says it never got it. */
static int block(iso_dep_t *d, size_t txn, size_t *rn)
{
	uint8_t s[2];
	const uint8_t *tx = d->tx;
	size_t n = txn, bits;
	uint32_t fwt = d->fwt;
	int r, nak = 0, wtx = 0;

	for(;;) {
		bits = 0;
		r = nfc_transceive(d->x, tx, NFC_BITS(n), d->rx,
				   NFC_BITS(sizeof d->rx), &bits, fwt, XF_CRC);
		fwt = d->fwt;
		if(r == NFC_ERR_TIMEOUT || r == NFC_ERR_CRC) {
			if(nak++) return r;
			s[0] = PCB_RNAK | d->bn;
			tx = s;
			n = 1;
			continue;
		}
		if(r != NFC_OK) return r;
		if(bits < 8) return NFC_ERR_PROTO;
		if((d->rx[0] & 0xF7) == PCB_WTX) {	/* S(WTX) */
			uint8_t m;

			if(bits < 16 || ++wtx > WTX_MAX) return NFC_ERR_PROTO;
			m = d->rx[1] & 0x3F;
			if(m == 0) return NFC_ERR_PROTO;
			if(m > 59) m = 59;		/* the limit in 7.3 */
			s[0] = PCB_WTX;
			s[1] = m;
			tx = s;
			n = 2;
			d->wtx++;
			fwt = d->fwt * m;
			if(fwt > FWT(14)) fwt = FWT(14);
			continue;
		}
		/* R(ACK) with the other block number after our R(NAK): the
		 * card lost our I-block; send it again (rule 6). */
		if(tx == s && (d->rx[0] & 0xF6) == PCB_RACK
		   && (d->rx[0] & 1) != d->bn && nak <= 1) {
			tx = d->tx;
			n = txn;
			nak++;
			continue;
		}
		*rn = NFC_BYTES(bits);
		return NFC_OK;
	}
}

int iso_dep_xchg(iso_dep_t *d, const uint8_t *cmd, size_t n,
		 uint8_t *resp, size_t room, size_t *rn)
{
	size_t maxinf, off = 0, got = 0, bl;
	int r, over = 0;

	*rn = 0;
	maxinf = (d->fsc > 3 ? d->fsc - 3 : 1);	/* PCB + CRC_A */
	if(maxinf > sizeof d->tx - 1) maxinf = sizeof d->tx - 1;
	for(;;) {				/* the command, chained */
		size_t c = n - off > maxinf ? maxinf : n - off;
		int more = off + c < n;

		d->tx[0] = PCB_I | d->bn | (more ? PCB_CHAIN : 0);
		memcpy(d->tx + 1, cmd + off, c);
		r = block(d, 1 + c, &bl);
		if(r != NFC_OK) return r;
		if(!more) break;
		if((d->rx[0] & 0xF6) != PCB_RACK || (d->rx[0] & 1) != d->bn)
			return NFC_ERR_PROTO;
		d->bn ^= 1;
		off += c;
	}
	for(;;) {				/* the answer, chained */
		uint8_t pcb = d->rx[0];
		size_t h = 1 + ((pcb & 0x08) ? 1 : 0) + ((pcb & 0x04) ? 1 : 0);

		if((pcb & 0xE2) != PCB_I || (pcb & 1) != d->bn || bl < h)
			return NFC_ERR_PROTO;
		d->bn ^= 1;
		for(; h < bl; h++, got++) {
			if(got < room) resp[got] = d->rx[h];
			else if(room >= 2) {	/* keep the last two (SW) */
				resp[room - 2] = resp[room - 1];
				resp[room - 1] = d->rx[h];
				over = 1;
			} else over = 1;
		}
		if(!(pcb & PCB_CHAIN)) break;
		d->tx[0] = PCB_RACK | d->bn;
		r = block(d, 1, &bl);
		if(r != NFC_OK) return r;
	}
	*rn = over ? room : got;
	return over ? NFC_ERR_OVERFLOW : NFC_OK;
}

void iso_dep_deselect(iso_dep_t *d)
{
	size_t bits = 0;
	uint8_t s = PCB_DESEL;

	if(d->x)
		(void)nfc_transceive(d->x, &s, 8, d->rx, NFC_BITS(sizeof d->rx),
				     &bits, d->fwt, XF_CRC);
}
