/* SPDX-License-Identifier: GPL-3.0-or-later
 * sgedit - editing a Sub-GHz RAW capture (trim, cut), pure logic
 * (Galtzagorriak MINIX, issue #424 item 2).
 *
 * A .sub RAW file is a list of signed durations in microseconds: a
 * positive value is the carrier on that long, a negative value off that
 * long (stock's RAW_Data, as subghz_file_encoder_worker reads them).  The
 * editor works on that list:
 *   - trim: keep only the part between two times t0 < t1 (us from the
 *     start of the capture); durations straddling a boundary are clipped;
 *   - cut: remove the part between t0 and t1, joining the two sides.
 * Adjacent durations of the same sign in the result are merged, so the
 * output stays a clean alternating (or at least well-formed) RAW list.
 *
 * These functions touch no files and no globals: the caller streams the
 * input in and the output out, so the whole capture need never be in RAM
 * at once (the Flipper side feeds them from sgfile.c a value at a time).
 * Host-tested in flipper/tests/subghz.  No flash writes.
 */
#ifndef SUBGHZ_SGEDIT_H
#define SUBGHZ_SGEDIT_H

#include <stdint.h>

typedef enum {
	SG_EDIT_TRIM,	/* keep [t0, t1) */
	SG_EDIT_CUT	/* drop [t0, t1) */
} SgEditOp;

/* A streaming editor: feed every input duration through sg_edit_feed in
 * order, then call sg_edit_end; emit() receives each output duration.
 * emit returns 0 on success, non-zero to stop (a write error), which
 * sg_edit_feed/sg_edit_end then return.  ctx is passed through to emit. */
typedef int (*sg_edit_emit)(void *ctx, int32_t dur);

typedef struct {
	SgEditOp op;
	uint32_t t0, t1;	/* the window, in microseconds */
	uint32_t at;		/* time consumed so far */
	int32_t held;		/* a pending output duration (for merging) */
	int have;		/* 1 if held is valid */
	sg_edit_emit emit;
	void *ctx;
} SgEdit;

/* Start an edit.  t0 < t1 (if not, trim keeps nothing and cut is a no-op).
 * A duration of 0 is ignored by feed. */
void sg_edit_init(SgEdit *e, SgEditOp op, uint32_t t0, uint32_t t1,
		  sg_edit_emit emit, void *ctx);
/* Feed one input duration (non-zero, sign = level).  0, or emit's non-zero. */
int sg_edit_feed(SgEdit *e, int32_t dur);
/* Flush the last held duration.  0, or emit's non-zero. */
int sg_edit_end(SgEdit *e);

#endif /* SUBGHZ_SGEDIT_H */
