/* wav.c - WAV files to 8-bit PWM duty values for the piezo (libapp, see
 * app.h).  No malloc, no state outside the caller's struct.
 */
#include <string.h>
#include "app.h"

static unsigned long le32(const unsigned char *p)
{
	return p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 |
	       (unsigned long)p[3] << 24;
}

static unsigned le16(const unsigned char *p)
{
	return p[0] | p[1] << 8;
}

int wav_parse(const unsigned char *h, int n, struct wav_info *w)
{
	unsigned long off = 12, len;
	int fmt = 0;

	memset(w, 0, sizeof(*w));
	if (n < 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
		return -1;
	while (off + 8 <= (unsigned long)n) {
		len = le32(h + off + 4);
		if (!memcmp(h + off, "fmt ", 4)) {
			if (len < 16 || off + 8 + 16 > (unsigned long)n)
				return -1;
			if (le16(h + off + 8) != 1)	/* PCM only */
				return -2;
			w->chans = (unsigned char)le16(h + off + 10);
			w->rate = le32(h + off + 12);
			w->bits = (unsigned char)le16(h + off + 22);
			fmt = 1;
		} else if (!memcmp(h + off, "data", 4)) {
			if (!fmt) return -1;
			w->data_off = off + 8;
			w->data_len = len;
			if ((w->chans != 1 && w->chans != 2) ||
			    (w->bits != 8 && w->bits != 16) ||
			    w->rate < 1000 || w->rate > 96000)
				return -2;
			return 0;
		}
		if (len > 0x7fffffffUL - off - 9) return -1;
		off += 8 + len + (len & 1);	/* chunks are word aligned */
	}
	return -1;				/* header not complete yet */
}

int wav_convert(struct wav_cvt *c, const unsigned char *in, int n,
		unsigned char *out, int max, unsigned long to_rate, int *used)
{
	int fb = c->w.chans * (c->w.bits / 8), o = 0, i = 0, s, k;
	int burst = (int)(to_rate / c->w.rate) + 1;

	if (to_rate < 1000 || to_rate > 96000) { *used = 0; return -1; }
	while (i + fb <= n && o + burst <= max) {
		s = 0;
		for (k = 0; k < c->w.chans; k++) {
			const unsigned char *p = in + i + k * (c->w.bits / 8);
			s += c->w.bits == 8 ? p[0] : ((signed char)p[1] + 128);
		}
		s /= c->w.chans;
		c->phase += to_rate;
		while (c->phase >= c->w.rate && o < max) {
			out[o++] = (unsigned char)s;
			c->phase -= c->w.rate;
		}
		i += fb;
	}
	*used = i;
	return o;
}
