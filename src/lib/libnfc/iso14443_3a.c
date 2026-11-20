/* iso14443_3a.c - the ISO 14443-3A poller (spec Phase 14, issue #387).
 * See iso14443_3a.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The activation sequence, as the stock reader runs it:
 *   REQA (7 bits)            -> ATQA (2 bytes)
 *   for each cascade level L (SEL = 0x93, 0x95, 0x97):
 *     SEL, NVB=0x20          -> 5 bytes: uid_cln[0..3] + BCC
 *     SEL, NVB=0x70, uid_cln, BCC (+CRC)  -> SAK (1 byte, +CRC)
 *     if SAK bit 2 set, this level carried a cascade tag (CT): the first
 *     UID byte was 0x88, the real UID continues at the next level.
 * A 4-byte UID finishes at level 1, a 7-byte at level 2, a 10-byte at 3.
 */
#include "iso14443_3a.h"
#include <string.h>

static const uint8_t sel_cmd[3] = {
	ISO14443_3A_SEL_CL1, ISO14443_3A_SEL_CL2, ISO14443_3A_SEL_CL3,
};

int iso14443_3a_request(const nfc_xfer_t *x, int wake, uint8_t atqa[2])
{
	uint8_t tx = wake ? ISO14443_3A_WUPA : ISO14443_3A_REQA;
	uint8_t rx[4];
	size_t rxb = 0;
	int r;

	r = nfc_transceive(x, &tx, 7, rx, sizeof rx * 8, &rxb, 0, NFC_XF_SHORT);
	if(r != NFC_OK) return r;
	if(rxb < 16) return NFC_ERR_PROTO;	/* ATQA is 2 bytes */
	atqa[0] = rx[0];
	atqa[1] = rx[1];
	return NFC_OK;
}

int iso14443_3a_poll(const nfc_xfer_t *x, int wake, iso14443_3a_tag_t *tag)
{
	int level, r;
	uint8_t rx[8];
	size_t rxb;

	memset(tag, 0, sizeof *tag);
	r = iso14443_3a_request(x, wake, tag->atqa);
	if(r != NFC_OK) return r;

	for(level = 0; level < 3; level++) {
		uint8_t anticol[2], sel[7], bcc, i;

		/* Anticollision: ask for the whole UID of this level. */
		anticol[0] = sel_cmd[level];
		anticol[1] = 0x20;		/* NVB: 2 bytes sent, no UID bits */
		rxb = 0;
		r = nfc_transceive(x, anticol, NFC_BITS(2), rx, sizeof rx * 8,
				   &rxb, 0, 0);
		if(r != NFC_OK) return r;
		if(rxb < NFC_BITS(5)) return NFC_ERR_PROTO;	/* 4 UID + BCC */

		/* BCC check (XOR of the four UID bytes). */
		bcc = rx[0] ^ rx[1] ^ rx[2] ^ rx[3];
		if(bcc != rx[4]) return NFC_ERR_PROTO;

		/* SELECT this level: SEL, NVB=0x70, the 4 UID bytes, BCC, CRC. */
		sel[0] = sel_cmd[level];
		sel[1] = 0x70;
		for(i = 0; i < 4; i++) sel[2 + i] = rx[i];
		sel[6] = rx[4];			/* BCC */
		rxb = 0;
		r = nfc_transceive(x, sel, NFC_BITS(7), rx, sizeof rx * 8, &rxb,
				   0, NFC_XF_CRC_TX | NFC_XF_CRC_RX);
		if(r != NFC_OK) return r;
		if(rxb < 8) return NFC_ERR_PROTO;	/* SAK is 1 byte */
		tag->sak = rx[0];

		/* Collect this level's UID bytes.  SAK bit 2 (0x04) set means a
		 * cascade tag: the first byte was CT (0x88), the real UID is the
		 * other three and continues at the next level. */
		if(tag->sak & 0x04) {
			/* sel[2] should be the CT (0x88); keep bytes 1..3. */
			if(tag->uid_len + 3 > (uint8_t)sizeof tag->uid)
				return NFC_ERR_PROTO;
			memcpy(tag->uid + tag->uid_len, sel + 3, 3);
			tag->uid_len += 3;
			continue;		/* go to the next cascade level */
		}
		if(tag->uid_len + 4 > (uint8_t)sizeof tag->uid)
			return NFC_ERR_PROTO;
		memcpy(tag->uid + tag->uid_len, sel + 2, 4);
		tag->uid_len += 4;
		return NFC_OK;			/* activation complete */
	}
	return NFC_ERR_PROTO;			/* more than 3 levels: invalid */
}
