/* frame.c - libmodem frames (spec P.3): CRC-16, pack, unpack, carousel. */
#include "modem.h"

unsigned short mdm_crc16(const unsigned char *p, int n, unsigned short crc)
{
	/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF (the caller's), MSB
	 * first, no final xor.  Bitwise: 0 B of table, ~1 us a byte. */
	int i;

	while (n-- > 0) {
		crc ^= (unsigned short) (*p++ << 8);
		for (i = 0; i < 8; i++)
			crc = (unsigned short) (crc & 0x8000
				? (crc << 1) ^ 0x1021 : crc << 1);
	}
	return crc;
}

int mdm_pack(const struct mdm_frame *f, unsigned char *out, int max)
{
	unsigned short crc;
	int i, n = MDM_OVH + f->len;

	if (f->len > MDM_MAXPAY || n > max) return -1;
	out[0] = (unsigned char) (f->type << 4 | (f->flags & 0x0F));
	out[1] = f->sid;
	out[2] = f->a;
	out[3] = f->b;
	out[4] = f->len;
	for (i = 0; i < f->len; i++) out[MDM_HDR + i] = f->pay[i];
	crc = mdm_crc16(out, MDM_HDR + f->len, 0xFFFF);
	out[MDM_HDR + f->len] = (unsigned char) (crc >> 8);
	out[MDM_HDR + f->len + 1] = (unsigned char) crc;
	return n;
}

int mdm_unpack(const unsigned char *in, int n, int mtu, struct mdm_frame *f)
{
	unsigned short crc;
	int len, type;

	/* The length first, from nothing but n: in[4] is only trusted when
	 * it agrees with the bytes that really came. */
	if (n < MDM_OVH || n > MDM_MAXFRAME) return -1;
	len = in[4];
	if (len > mtu || len > MDM_MAXPAY || n != MDM_OVH + len) return -1;
	type = in[0] >> 4;
	if (type < MDM_T_DATA || type > MDM_T_DGRAM) return -1;
	crc = mdm_crc16(in, MDM_HDR + len, 0xFFFF);
	if (in[MDM_HDR + len] != (unsigned char) (crc >> 8)
	    || in[MDM_HDR + len + 1] != (unsigned char) crc) return -1;
	f->type = (unsigned char) type;
	f->flags = (unsigned char) (in[0] & 0x0F);
	f->sid = in[1];
	f->a = in[2];
	f->b = in[3];
	f->len = (unsigned char) len;
	f->pay = in + MDM_HDR;
	return 0;
}

/* ---- carousel ---- */

void mdm_car_init(struct mdm_car *c, unsigned char *map)
{
	int i;

	c->map = map;
	for (i = 0; i < MDM_CAR_MAX / 8; i++) map[i] = 0;
	c->sid = -1;
	c->total = -1;
	c->have = 0;
	c->size = -1;
	c->z = 0;
}

int mdm_car_pack(unsigned char sid, int idx, int flags,
	const unsigned char *data, int n, unsigned char *out, int max)
{
	struct mdm_frame f;

	if (idx < 0 || idx >= MDM_CAR_MAX) return -1;
	f.type = MDM_T_CAR;
	f.flags = (unsigned char) (flags & (MDM_F_LAST | MDM_F_Z));
	f.sid = sid;
	f.a = (unsigned char) idx;
	f.b = (unsigned char) (idx >> 8);
	f.len = (unsigned char) n;
	f.pay = data;
	return mdm_pack(&f, out, max);
}

int mdm_car_take(struct mdm_car *c, const struct mdm_frame *f, int chunk)
{
	int idx;

	if (f->type != MDM_T_CAR) return 0;
	if (c->sid >= 0 && f->sid != c->sid) return 0;	/* another transfer */
	idx = f->a | f->b << 8;
	if (idx >= MDM_CAR_MAX) return -1;
	if (f->flags & MDM_F_LAST) {
		if (f->len > chunk) return -1;
		if (c->total >= 0 && c->total != idx + 1) return -1;
	} else {
		if (f->len != chunk) return -1;
		if (c->total >= 0 && idx >= c->total) return -1;
	}
	if (c->sid >= 0 && (f->flags & MDM_F_Z) != c->z) return -1;
	if (c->sid < 0) {
		c->sid = f->sid;
		c->z = f->flags & MDM_F_Z;
	}
	if (f->flags & MDM_F_LAST) {
		c->total = idx + 1;
		c->size = (long) idx * chunk + f->len;
	}
	if (c->map[idx >> 3] & (1 << (idx & 7))) return 0;
	c->map[idx >> 3] |= (unsigned char) (1 << (idx & 7));
	c->have++;
	return 1;
}

int mdm_car_done(const struct mdm_car *c)
{
	/* Count the bits below total again: frames with a bigger index that
	 * came before the LAST one (another sender with the same sid, or a
	 * forged one) must not stand in for missing ones. */
	int i;

	if (c->total < 0 || c->have < c->total) return 0;
	for (i = 0; i < c->total; i++)
		if (!(c->map[i >> 3] & (1 << (i & 7)))) return 0;
	return 1;
}
