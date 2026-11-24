/* SPDX-License-Identifier: GPL-3.0-or-later
 * sgedit - RAW .sub editing (issue #424 item 2).  See sgedit.h.
 */
#include "sgedit.h"

void sg_edit_init(SgEdit *e, SgEditOp op, uint32_t t0, uint32_t t1,
		  sg_edit_emit emit, void *ctx)
{
	e->op = op;
	e->t0 = t0;
	e->t1 = t1;
	e->at = 0;
	e->held = 0;
	e->have = 0;
	e->emit = emit;
	e->ctx = ctx;
}

/* Overlap length of [a, b) with [t0, t1). */
static uint32_t overlap(uint32_t a, uint32_t b, uint32_t t0, uint32_t t1)
{
	uint32_t lo = a > t0 ? a : t0;
	uint32_t hi = b < t1 ? b : t1;

	return hi > lo ? hi - lo : 0;
}

/* Add a kept piece (length > 0, sign = sign of dur) to the held run,
 * merging same-sign pieces; flush the previous run on a sign change. */
static int push(SgEdit *e, int32_t dur, uint32_t keep)
{
	int32_t piece = dur < 0 ? -(int32_t)keep : (int32_t)keep;
	int r;

	if(keep == 0)
		return 0;
	if(e->have && ((e->held < 0) == (piece < 0))) {
		e->held += piece;	/* same level: extend the run */
		return 0;
	}
	if(e->have) {			/* a new level: emit the old run */
		r = e->emit(e->ctx, e->held);
		if(r) return r;
	}
	e->held = piece;
	e->have = 1;
	return 0;
}

int sg_edit_feed(SgEdit *e, int32_t dur)
{
	uint32_t m, a, b, ov, keep;
	int r;

	if(dur == 0)
		return 0;
	m = dur < 0 ? (uint32_t)(-(int64_t)dur) : (uint32_t)dur;
	a = e->at;
	b = a + m;
	e->at = b;
	ov = overlap(a, b, e->t0, e->t1);
	keep = e->op == SG_EDIT_TRIM ? ov : m - ov;
	if(e->op == SG_EDIT_CUT && keep > 0) {
		/* the kept part may be split by the hole into a head
		 * (before t0) and a tail (after t1); both share dur's sign,
		 * so pushing them separately still merges correctly. */
		uint32_t head = a < e->t0 ? (b < e->t0 ? m
					     : e->t0 - a) : 0;
		uint32_t tail = b > e->t1 ? (a > e->t1 ? m
					     : b - e->t1) : 0;

		if(head) { r = push(e, dur, head); if(r) return r; }
		if(tail) { r = push(e, dur, tail); if(r) return r; }
		return 0;
	}
	return push(e, dur, keep);
}

int sg_edit_end(SgEdit *e)
{
	int r;

	if(e->have) {
		r = e->emit(e->ctx, e->held);
		e->have = 0;
		if(r) return r;
	}
	return 0;
}
