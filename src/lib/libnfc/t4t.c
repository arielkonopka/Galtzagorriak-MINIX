/* t4t.c - NFC Forum Type 4 Tag, read only (spec Phase 14, issue #451).
 * See t4t.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "t4t.h"
#include <stdio.h>
#include <string.h>

const uint8_t t4t_ndef_aid[7] = { 0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01 };

#define CHUNK	128		/* READ BINARY Le at most (resp on stack) */

static int apdu(iso_dep_t *d, const uint8_t *c, size_t n, uint8_t *r,
		size_t room, size_t *rn)
{
	int e = iso_dep_xchg(d, c, n, r, room, rn);

	if(e != NFC_OK) return e;
	if(*rn < 2 || iso_dep_sw(r, *rn) != 0x9000) return NFC_ERR_NOTSUP;
	*rn -= 2;
	return NFC_OK;
}

static int select_fid(iso_dep_t *d, uint16_t fid)
{
	uint8_t c[7] = { 0x00, 0xA4, 0x00, 0x0C, 0x02 }, r[4];
	size_t rn;

	c[5] = (uint8_t)(fid >> 8);
	c[6] = (uint8_t)fid;
	return apdu(d, c, sizeof c, r, sizeof r, &rn);
}

static int read_bin(iso_dep_t *d, uint16_t off, uint8_t le, uint8_t *out,
		    size_t *got)
{
	uint8_t c[5] = { 0x00, 0xB0 }, r[CHUNK + 2];
	size_t rn;
	int e;

	c[2] = (uint8_t)(off >> 8);
	c[3] = (uint8_t)off;
	c[4] = le;
	e = apdu(d, c, sizeof c, r, sizeof r, &rn);
	if(e != NFC_OK) return e;
	if(rn > le) rn = le;
	memcpy(out, r, rn);
	*got = rn;
	return rn ? NFC_OK : NFC_ERR_PROTO;
}

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

int t4t_read(iso_dep_t *d, t4t_t *t)
{
	uint8_t c[13] = { 0x00, 0xA4, 0x04, 0x00, 0x07 }, r[CHUNK + 2];
	size_t rn;
	unsigned off;
	int e;

	memset(t, 0, sizeof *t);
	memcpy(c + 5, t4t_ndef_aid, 7);
	c[12] = 0x00;
	e = apdu(d, c, sizeof c, r, sizeof r, &rn);
	if(e != NFC_OK) return e;
	if((e = select_fid(d, 0xE103)) != NFC_OK) return e;
	if((e = read_bin(d, 0, 15, r, &rn)) != NFC_OK) return e;
	/* CCLEN(2) ver MLe(2) MLc(2) T=04 L=06 fid(2) max(2) rd wr */
	if(rn < 15 || be16(r) < 15 || r[7] != 0x04 || r[8] < 6)
		return NFC_ERR_PROTO;
	t->ver = r[2];
	t->mle = be16(r + 3);
	t->mlc = be16(r + 5);
	t->fid = be16(r + 9);
	t->fmax = be16(r + 11);
	t->rd = r[13];
	t->wr = r[14];
	if((t->ver >> 4) < 2 || t->mle < 0x0F || t->fmax < 2)
		return NFC_ERR_PROTO;		/* 1.0 or a bad CC */
	if(t->rd != 0x00) return NFC_ERR_NOTSUP;	/* not open for read */
	if((e = select_fid(d, t->fid)) != NFC_OK) return e;
	if((e = read_bin(d, 0, 2, r, &rn)) != NFC_OK) return e;
	if(rn < 2) return NFC_ERR_PROTO;
	t->nlen = be16(r);
	if(t->nlen > t->fmax - 2) return NFC_ERR_PROTO;
	for(off = 0; off < t->nlen && off < T4T_NDEF_MAX; off += rn) {
		unsigned le = t->nlen - off;

		if(le > T4T_NDEF_MAX - off) le = T4T_NDEF_MAX - off;
		if(le > t->mle) le = t->mle;
		if(le > CHUNK) le = CHUNK;
		e = read_bin(d, (uint16_t)(2 + off), (uint8_t)le, t->ndef + off,
			     &rn);
		if(e != NFC_OK) return e;
	}
	t->got = (uint16_t)(off > T4T_NDEF_MAX ? T4T_NDEF_MAX : off);
	return NFC_OK;
}

/* NFC Forum URI RTD, Table 3: the abbreviations 0x00..0x23. */
static const char *const uri_pre[] = {
	"", "http://www.", "https://www.", "http://", "https://", "tel:",
	"mailto:", "ftp://anonymous:anonymous@", "ftp://ftp.", "ftps://",
	"sftp://", "smb://", "nfs://", "ftp://", "dav://", "news:",
	"telnet://", "imap:", "rtsp://", "urn:", "pop:", "sip:", "sips:",
	"tftp:", "btspp://", "btl2cap://", "btgoep://", "tcpobex://",
	"irdaobex://", "file://", "urn:epc:id:", "urn:epc:tag:",
	"urn:epc:pat:", "urn:epc:raw:", "urn:epc:", "urn:nfc:",
};

/* Append len bytes of p to out, printable ASCII only. */
static void put(char *out, size_t n, const uint8_t *p, size_t len)
{
	size_t o = strlen(out), i;

	for(i = 0; i < len && o + 1 < n; i++)
		out[o++] = (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '.';
	out[o] = 0;
}

void t4t_ndef_summary(const uint8_t *m, size_t len, char *out, size_t n)
{
	uint8_t hdr, tl, tnf;
	size_t i = 1, pl, il = 0;
	const uint8_t *type, *pay;

	if(n == 0) return;
	out[0] = 0;
	if(len < 3) { put(out, n, (const uint8_t *)"Empty", 5); return; }
	hdr = m[0];
	tnf = hdr & 0x07;
	tl = m[i++];
	if(hdr & 0x10) pl = m[i++];		/* SR: 1-byte length */
	else {
		if(len < i + 4) goto bad;
		pl = (size_t)m[i] << 24 | (size_t)m[i + 1] << 16
			| (size_t)m[i + 2] << 8 | m[i + 3];
		i += 4;
	}
	if(hdr & 0x08) {			/* IL */
		if(len < i + 1) goto bad;
		il = m[i++];
	}
	if(len < i + tl + il) goto bad;
	type = m + i;
	pay = type + tl + il;
	if(pay + pl > m + len) pl = (size_t)(m + len - pay);	/* cut */
	if(tnf == 0) { put(out, n, (const uint8_t *)"Empty", 5); return; }
	if(tnf == 1 && tl == 1 && type[0] == 'U' && pl >= 1) {
		const char *pre = pay[0] < sizeof uri_pre / sizeof uri_pre[0]
			? uri_pre[pay[0]] : "";

		put(out, n, (const uint8_t *)"URI ", 4);
		put(out, n, (const uint8_t *)pre, strlen(pre));
		put(out, n, pay + 1, pl - 1);
		return;
	}
	if(tnf == 1 && tl == 1 && type[0] == 'T' && pl >= 1) {
		size_t ll = pay[0] & 0x3F;

		if(ll + 1 > pl) goto bad;
		put(out, n, (const uint8_t *)"Text ", 5);
		put(out, n, pay + 1, ll);
		put(out, n, (const uint8_t *)": ", 2);
		if(pay[0] & 0x80)		/* UTF-16: shown as dots */
			put(out, n, (const uint8_t *)"(UTF-16)", 8);
		else
			put(out, n, pay + 1 + ll, pl - 1 - ll);
		return;
	}
	put(out, n, (const uint8_t *)(tnf == 2 ? "MIME " : "Type "), 5);
	put(out, n, type, tl);
	return;
bad:
	out[0] = 0;
	put(out, n, (const uint8_t *)"Malformed NDEF", 14);
}
