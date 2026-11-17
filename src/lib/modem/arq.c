/* arq.c - libmodem's ARQ byte stream (spec P.3, RP.4): selective repeat,
 * half duplex, a window of up to 8 frames.
 *
 * The sender sends every frame of its window that is not acknowledged yet,
 * the last with TURN, and waits for one ACK: `a` = the next sequence number
 * the receiver wants (everything before it arrived), bit i of `b` = frame
 * a+1+i arrived too.  What is missing goes again in the next burst.  Twenty
 * bursts in a row without progress and the link counts as lost.  Sequence
 * numbers are 8 bits on the air and counted in longs here.
 *
 * A window slot (wbuf + (seq % window) * (mtu + 2)): [0] payload length,
 * [1] state bits, [2..] the payload.
 */
#include "modem.h"

#define S_ACKED		1	/* sender: acknowledged; receiver: arrived */
#define S_LAST		2
#define S_SENT		4	/* sender: on the air at least once */

#define slot(q)	(wbuf + (int) ((q) % window) * (p->mtu + 2))

static long ack_wait(const struct mdm_phy *p)
{
	/* The receiver answers right after the TURN frame: its ACK on the air
	 * and two turnarounds (each with the scheduler's slack). */
	return p->airtime(p->ctx, MDM_OVH) + 2 * p->turn_ms;
}

int mdm_send(const struct mdm_phy *p, unsigned char sid, int window,
	int flags, mdm_get_fn get, void *u, unsigned char *wbuf, struct mdm_stat *st,
	void (*progress)(void *u, const struct mdm_stat *st))
{
	unsigned char fr[MDM_MAXFRAME], rx[MDM_MAXFRAME];
	struct mdm_frame f;
	long base = 0, next = 0, q, lastq;
	int eof = 0, noprog = 0, junk, n, k, r, off, moved, i;
	unsigned char *s;

	if (window < 1 || window > MDM_WMAX || p->mtu < 1
	    || p->mtu > MDM_MAXPAY) return MDM_EARG;
	st->frames = st->resent = st->bad = st->bytes = 0;
	st->flags = flags & MDM_F_Z;
	for (;;) {
		/* Fill the window from the source, whole frames where it can. */
		while (!eof && next < base + window) {
			s = slot(next);
			for (k = 0; k < p->mtu; k += r) {
				r = get(u, s + 2 + k, p->mtu - k);
				if (r < 0) return MDM_EIO;
				if (r == 0) { eof = 1; break; }
			}
			s[0] = (unsigned char) k;
			s[1] = (unsigned char) (eof ? S_LAST : 0);
			next++;
		}

		/* The burst: everything unacknowledged, TURN on the last. */
		lastq = -1;
		for (q = base; q < next; q++)
			if (!(slot(q)[1] & S_ACKED)) lastq = q;
		for (q = base; q < next; q++) {
			s = slot(q);
			if (s[1] & S_ACKED) continue;
			f.type = MDM_T_DATA;
			f.flags = (unsigned char) ((s[1] & S_LAST ? MDM_F_LAST : 0)
				| (q == lastq ? MDM_F_TURN : 0) | (flags & MDM_F_Z));
			f.sid = sid;
			f.a = (unsigned char) q;
			f.b = 0;
			f.len = s[0];
			f.pay = s + 2;
			n = mdm_pack(&f, fr, (int) sizeof(fr));
			if (n < 0 || p->send(p->ctx, fr, n) < 0) return MDM_EIO;
			if (s[1] & S_SENT) st->resent++;
			else { st->frames++; s[1] |= S_SENT; }
		}

		/* One ACK.  Noise and other transfers' frames are skipped, but
		 * only a few times, so a busy channel cannot hold us forever. */
		moved = 0;
		for (junk = 0; junk < 4; junk++) {
			n = p->recv(p->ctx, rx, (int) sizeof(rx), ack_wait(p));
			if (n < 0) return MDM_EIO;
			if (n == 0) break;			/* timeout */
			if (mdm_unpack(rx, n, p->mtu, &f) < 0
			    || f.type != MDM_T_ACK || f.sid != sid) {
				st->bad++;
				continue;
			}
			off = (f.a - (unsigned char) base) & 0xFF;
			if (off > next - base) {		/* not a seq we sent */
				st->bad++;
				continue;
			}
			for (i = 0; i < off; i++) {
				s = slot(base + i);
				if (!(s[1] & S_ACKED)) { st->bytes += s[0]; moved = 1; }
			}
			base += off;
			for (i = 0; i < MDM_WMAX; i++) {
				q = base + 1 + i;
				if (!(f.b & (1 << i)) || q >= next) continue;
				s = slot(q);
				if (!(s[1] & S_ACKED)) {
					s[1] |= S_ACKED;
					st->bytes += s[0];
					moved = 1;
				}
			}
			if (moved && progress) progress(u, st);
			break;
		}
		if (eof && base == next) return MDM_OK;		/* LAST acked */
		if (moved) noprog = 0;
		else if (++noprog >= MDM_RETRIES) return MDM_ELOST;
	}
}

int mdm_recv(const struct mdm_phy *p, int window, long idle_ms,
	mdm_put_fn put, void *u, unsigned char *wbuf, struct mdm_stat *st,
	void (*progress)(void *u, const struct mdm_stat *st))
{
	unsigned char rx[MDM_MAXFRAME], fr[MDM_OVH];
	struct mdm_frame f;
	long expect = 0, linger;
	int sid = -1, fin = 0, n, off, i;
	unsigned char *s, bits;

	if (window < 1 || window > MDM_WMAX || p->mtu < 1
	    || p->mtu > MDM_MAXPAY) return MDM_EARG;
	st->frames = st->resent = st->bad = st->bytes = st->flags = 0;
	for (i = 0; i < window; i++) wbuf[i * (p->mtu + 2) + 1] = 0;
	/* After the LAST frame: long enough for the sender to miss our ACK,
	 * time out and send its burst again, twice. */
	linger = 2 * (window * p->airtime(p->ctx, p->mtu + MDM_OVH)
		+ ack_wait(p) + p->turn_ms);
	for (;;) {
		n = p->recv(p->ctx, rx, (int) sizeof(rx), fin ? linger : idle_ms);
		if (n < 0) return MDM_EIO;
		if (n == 0) return fin ? MDM_OK : MDM_EIDLE;
		if (mdm_unpack(rx, n, p->mtu, &f) < 0 || f.type != MDM_T_DATA) {
			st->bad++;
			continue;
		}
		off = (f.a - (unsigned char) expect) & 0xFF;
		if (sid < 0) {
			if (off >= window) { st->bad++; continue; }
			sid = f.sid;			/* the first burst: ours */
			st->flags = f.flags & MDM_F_Z;
		} else if (f.sid != sid) {
			st->bad++;
			continue;
		}
		if (!fin && off < window) {
			s = slot(expect + off);
			if (!(s[1] & S_ACKED)) {
				s[0] = f.len;
				s[1] = (unsigned char) (S_ACKED
					| (f.flags & MDM_F_LAST ? S_LAST : 0));
				for (i = 0; i < f.len; i++) s[2 + i] = f.pay[i];
			} else st->resent++;
		} else st->resent++;		/* an old one again */

		/* Hand over what is now in order. */
		while (!fin && ((s = slot(expect))[1] & S_ACKED)) {
			if (put(u, s + 2, s[0]) < 0) return MDM_EIO;
			st->frames++;
			st->bytes += s[0];
			if (s[1] & S_LAST) fin = 1;
			s[1] = 0;
			expect++;
			if (progress) progress(u, st);
		}

		if (f.flags & MDM_F_TURN) {
			bits = 0;
			for (i = 0; i < MDM_WMAX && i + 1 < window; i++)
				if (!fin && (slot(expect + 1 + i)[1] & S_ACKED))
					bits |= (unsigned char) (1 << i);
			f.type = MDM_T_ACK;
			f.flags = (unsigned char) (MDM_F_TURN
				| (fin ? MDM_F_LAST : 0));
			f.sid = (unsigned char) sid;
			f.a = (unsigned char) expect;
			f.b = bits;
			f.len = 0;
			f.pay = fr;
			n = mdm_pack(&f, fr, (int) sizeof(fr));
			if (n < 0 || p->send(p->ctx, fr, n) < 0) return MDM_EIO;
		}
	}
}
