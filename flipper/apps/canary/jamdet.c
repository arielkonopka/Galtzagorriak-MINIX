/* jamdet.c - the jamming alarm (spec M.2, task T35); see jamdet.h. */
#include <string.h>
#include "jamdet.h"

void jd_reset(struct jd_state *s)
{
	memset(s, 0, sizeof(*s));
}

int jd_sample(const struct jd_rule *r, struct jd_state *s,
					unsigned long now, int rssi)
{
	int hi = rssi >= r->hi;

	if (s->started && now - s->last > r->hold) {	/* we slept */
		s->high = 0;
		s->up = 0;
		s->down = now;
	}
	s->started = 1;
	s->last = now;
	if (hi) {
		if (!s->high) {
			/* a short dip is not a gap: keep the run */
			if (s->up == 0 || now - s->down > r->gap)
				s->up = now ? now : 1;
			s->high = 1;
		}
		if (!s->alarm && now - s->up >= r->hold) {
			s->alarm = 1;
			return JD_ALARM;
		}
		return JD_NONE;
	}
	if (s->high) { s->high = 0; s->down = now; }
	if (s->alarm && now - s->down >= r->clear) {
		s->alarm = 0;
		s->up = 0;
		return JD_CLEAR;
	}
	if (!s->alarm && now - s->down > r->gap) s->up = 0;
	return JD_NONE;
}
