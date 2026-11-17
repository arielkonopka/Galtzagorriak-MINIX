/* p4.c - the RFID line code P4 (spec P.5): the tag->reader direction of
 * the 125 kHz modem.
 *
 * A cycle is Q carrier periods with the antenna loaded, then (L-1)*Q not;
 * L = 2, 3, 4, 5 carries the bit pairs 00, 01, 10, 11 (most significant
 * pair first), L = 7 is the sync before a frame.  The loaded part is the
 * same in every cycle, so the time between two edges of the same kind is
 * L*Q whichever kind it is: rising to rising spans this cycle, falling to
 * falling the unloaded rest of this one and the loaded start of the next.
 * The reader's comparator is exact one way and drifts the other (the
 * envelope settles slowly), and taking whole cycles cancels the drift.  The
 * level of the first edge is unknown, so both pairings are decoded and the
 * CRC picks.
 */
#include "modem.h"

int mdm_p4_encode(const unsigned char *b, int n, int q,
	unsigned long *dur, unsigned long *pulse, int max)
{
	int i, k, m = 0, l;

	if (q < 1 || 1 + 4 * n > max) return -1;
	dur[m] = (unsigned long) (MDM_P4_SYNC * q - 1);
	pulse[m++] = (unsigned long) q;
	for (i = 0; i < n; i++)
		for (k = 6; k >= 0; k -= 2) {
			l = 2 + ((b[i] >> k) & 3);
			dur[m] = (unsigned long) (l * q - 1);
			pulse[m++] = (unsigned long) q;
		}
	return m;
}

long mdm_p4_us(int n, int q)
{
	/* random data: L averages 3.5 */
	return ((long) MDM_P4_SYNC * 2 + 4L * n * 7) * q * MDM_P4_PERIOD_US / 2;
}

int mdm_p4_decode(const unsigned long *d, int nd, int q, int mtu,
	void (*frame)(void *u, const unsigned char *buf, int n), void *u)
{
	unsigned char buf[MDM_MAXFRAME];
	unsigned long qus = (unsigned long) q * MDM_P4_PERIOD_US, t, l, err;
	int off, i, got, nsym, want, found = 0;
	unsigned acc;
	struct mdm_frame f;

	if (mtu > MDM_MAXPAY) mtu = MDM_MAXPAY;
	for (off = 0; off < 2; off++) {
		got = -1;			/* no sync yet */
		nsym = 0;
		acc = 0;
		want = MDM_HDR;
		for (i = off; i + 1 < nd; i += 2) {
			t = d[i] + d[i + 1];
			if (d[i] > 0x00FFFFFFUL || d[i + 1] > 0x00FFFFFFUL) {
				got = -1;		/* nonsense: start over */
				continue;
			}
			l = (t + qus / 2) / qus;
			err = t > l * qus ? t - l * qus : l * qus - t;
			if (err * 100 > qus * 45) l = 0;	/* between lengths */
			if (l == MDM_P4_SYNC) {
				got = 0;
				nsym = 0;
				acc = 0;
				want = MDM_HDR;
				continue;
			}
			if (got < 0) continue;
			if (l < 2 || l > 5) { got = -1; continue; }
			acc = (acc << 2) | (unsigned) (l - 2);
			if (++nsym < 4) continue;
			buf[got++] = (unsigned char) acc;
			nsym = 0;
			acc = 0;
			if (got == MDM_HDR) {
				if (buf[4] > mtu) { got = -1; continue; }
				want = MDM_OVH + buf[4];
			}
			if (got == want) {
				if (mdm_unpack(buf, got, mtu, &f) == 0) {
					frame(u, buf, got);
					found++;
				}
				got = -1;
			}
		}
	}
	return found;
}
