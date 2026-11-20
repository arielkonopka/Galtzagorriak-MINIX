/* gpiorules.c - the `gpio` canary's rules (spec R7b.4.4, task T35); see
 * gpiorules.h.
 */
#include <string.h>
#include "gpiorules.h"

void gr_reset(struct gr_state *s)
{
	memset(s, 0, sizeof(*s));
	s->stable = GR_UNKNOWN;
}

/* A level alarm: enter on bad, leave on !bad unless latched. */
static int level(const struct gr_rule *r, struct gr_state *s, int bad)
{
	if (bad && !s->alarm) { s->alarm = 1; return GR_ALARM; }
	if (!bad && s->alarm && !r->latch) { s->alarm = 0; return GR_CLEAR; }
	return GR_NONE;
}

/* Debounce raw (0/1): returns 1 when the stable level has just changed
 * (or settled for the first time; old is then GR_UNKNOWN). */
static int settle(const struct gr_rule *r, struct gr_state *s,
				unsigned long now, int raw, int *old)
{
	if (!s->started || raw != s->raw) {
		s->started = 1;
		s->raw = (unsigned char)raw;
		s->since = now;
	}
	if (s->raw == s->stable || now - s->since < r->debounce) return 0;
	*old = s->stable;
	s->stable = s->raw;
	return 1;
}

static int pulse(const struct gr_rule *r, struct gr_state *s,
						unsigned long now, long v)
{
	unsigned long w = r->window ? r->window : 1, gap;
	int ev = GR_NONE, e;

	if (!s->started) {
		s->started = 1;
		s->win = now;
		s->count = 0;
	}
	if (v > 0) s->count += (unsigned long)v;
	gap = now - s->win;
	if (gap < w) return GR_NONE;
	s->rate = (long)s->count;	/* the window that just closed */
	ev = level(r, s, s->rate < r->lo || s->rate > r->hi);
	if (gap >= 2 * w) {		/* then at least one empty window */
		s->rate = 0;
		e = level(r, s, 0 < r->lo || 0 > r->hi);
		if (ev != GR_ALARM && e != GR_NONE) ev = e; /* keep alarms */
	}
	s->count = 0;
	s->win = now - gap % w;
	return ev;
}

int gr_sample(const struct gr_rule *r, struct gr_state *s,
					unsigned long now, long v)
{
	int old, bad;

	s->value = v;
	switch (r->kind) {
	case GR_LOOP:
		if (!settle(r, s, now, v != 0, &old)) return GR_NONE;
		return level(r, s, s->stable);
	case GR_CONTACT:
		if (!settle(r, s, now, v != 0, &old) || old == GR_UNKNOWN)
			return GR_NONE;		/* first level: the baseline */
		if (r->when & (s->stable ? GR_ON_OPEN : GR_ON_CLOSE)) {
			s->count++;
			s->alarm = 1;
			return GR_ALARM;
		}
		return GR_NONE;
	case GR_PULSE:
		return pulse(r, s, now, v);
	case GR_VOLT:
		/* outside the window, or (in alarm) not yet hyst inside it */
		bad = v < r->lo || v > r->hi;
		if (s->alarm && (v < r->lo + r->hyst || v > r->hi - r->hyst))
			bad = 1;
		if (!settle(r, s, now, bad, &old)) return GR_NONE;
		return level(r, s, s->stable);
	}
	return GR_NONE;
}
