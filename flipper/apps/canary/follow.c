/* follow.c - the tracker-following rule (spec M.2, task T35); see follow.h. */
#include <string.h>
#include "follow.h"

void flw_init(struct flw *f)
{
	memset(f, 0, sizeof(*f));
}

int flw_seen(const struct flw_rule *r, struct flw *f,
					unsigned long id, unsigned long now)
{
	struct flw_ent *e = 0, *old = 0;
	int i;

	for (i = 0; i < FLW_MAX; i++) {
		struct flw_ent *p = &f->e[i];
		if (p->used && p->id == id) { e = p; break; }
		if (!p->used) { if (!old || old->used) old = p; }
		else if (!p->flagged && (!old ||
			(old->used && now - p->last > now - old->last)))
			old = p;
	}
	if (!e) {
		if (!old) return 0;	/* all flagged: nothing to evict */
		e = old;
		memset(e, 0, sizeof(*e));
		e->used = 1;
		e->id = id;
		e->first = e->last = now;
		e->n = 1;
		return 0;
	}
	if (now - e->last >= r->apart) {
		if (e->n < 255) e->n++;
		e->last = now;
	}
	if (!e->flagged && e->n >= r->sightings &&
	    e->last - e->first >= r->span) {
		e->flagged = 1;
		return 1;
	}
	return 0;
}

void flw_expire(const struct flw_rule *r, struct flw *f, unsigned long now)
{
	int i;
	for (i = 0; i < FLW_MAX; i++)
		if (f->e[i].used && now - f->e[i].last > 2 * r->span)
			f->e[i].used = 0;
}

int flw_flagged(const struct flw *f)
{
	int i, n = 0;
	for (i = 0; i < FLW_MAX; i++)
		if (f->e[i].used && f->e[i].flagged) n++;
	return n;
}
