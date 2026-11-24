/* dht.c - see dht.h (issue #358). */
#include "dht.h"

/* A high pulse inside these is the response (nominal 80 us). */
#define RESP_MIN	60
#define RESP_MAX	110
/* A bit's high pulse: below SPLIT a 0 (nominal 27 us), above a 1 (70 us);
 * outside MIN..MAX it is no bit. */
#define BIT_MIN		10
#define SPLIT		48
#define BIT_MAX		100

int dht_decode(const unsigned short *us, int n, int first_level,
	       struct dht_reading *r)
{
	int i, level, seen = 0, bits = 0;
	unsigned char b[5];

	for (i = 0; i < 5; i++) b[i] = 0;
	for (i = 0; i < n; i++) {
		level = (first_level + i) & 1;
		if (!level) continue;
		if (!seen) {			/* wait for the response */
			if (us[i] >= RESP_MIN && us[i] <= RESP_MAX) seen = 1;
			continue;
		}
		if (us[i] < BIT_MIN || us[i] > BIT_MAX) return DHT_TIMING;
		b[bits >> 3] = (unsigned char) (b[bits >> 3] << 1
						| (us[i] > SPLIT));
		if (++bits == DHT_BITS) break;
	}
	if (bits < DHT_BITS) return DHT_SHORT;
	if ((unsigned char) (b[0] + b[1] + b[2] + b[3]) != b[4])
		return DHT_CHECKSUM;
	r->rh_x10 = b[0] << 8 | b[1];
	r->temp_x10 = (b[2] & 0x7F) << 8 | b[3];
	if (b[2] & 0x80) r->temp_x10 = -r->temp_x10;
	return DHT_OK;
}

int dht_frame(const struct dht_reading *r, int bad_sum, unsigned short *us)
{
	unsigned char b[5];
	int n = 0, i, t = r->temp_x10 < 0 ? -r->temp_x10 : r->temp_x10;

	b[0] = (unsigned char) (r->rh_x10 >> 8);
	b[1] = (unsigned char) r->rh_x10;
	b[2] = (unsigned char) (t >> 8 & 0x7F);
	if (r->temp_x10 < 0) b[2] |= 0x80;
	b[3] = (unsigned char) t;
	b[4] = (unsigned char) (b[0] + b[1] + b[2] + b[3] + bad_sum);
	us[n++] = 80;			/* low */
	us[n++] = 80;			/* high */
	for (i = 0; i < DHT_BITS; i++) {
		us[n++] = 50;
		us[n++] = (b[i >> 3] >> (7 - (i & 7)) & 1) ? 70 : 27;
	}
	us[n++] = 50;			/* the last low, then it lets go */
	return n;
}

static int put(char *p, int pos, int v)
{
	/* v in tenths, at least one digit before the point, a sign */
	if (v < 0) { p[pos++] = '-'; v = -v; }
	if (v >= 1000) p[pos++] = (char) ('0' + v / 1000 % 10);
	if (v >= 100) p[pos++] = (char) ('0' + v / 100 % 10);
	p[pos++] = (char) ('0' + v / 10 % 10);
	p[pos++] = '.';
	p[pos++] = (char) ('0' + v % 10);
	return pos;
}

int dht_format(const struct dht_reading *r, char *buf)
{
	const char *s;
	int n = 0;

	for (s = "temp="; *s; s++) buf[n++] = *s;
	n = put(buf, n, r->temp_x10);
	for (s = " rh="; *s; s++) buf[n++] = *s;
	n = put(buf, n, r->rh_x10);
	buf[n] = 0;
	return n;
}
