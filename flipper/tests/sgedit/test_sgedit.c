/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host unit tests for the RAW .sub editor (src/lib/subghz/sgedit, issue
 * #424 item 2).  make -C flipper/tests/sgedit test (ASan/UBSan).
 */
#include <stdio.h>
#include <string.h>
#include "sgedit.h"

static int fails;
#define CHECK(c) do { if (!(c)) { \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* Collect emitted durations into an array. */
struct sink { int32_t v[64]; int n; };
static int collect(void *ctx, int32_t dur)
{
	struct sink *s = ctx;

	if (s->n < (int)(sizeof s->v / sizeof s->v[0])) s->v[s->n++] = dur;
	return 0;
}

static void run(SgEditOp op, uint32_t t0, uint32_t t1,
		const int32_t *in, int nin, struct sink *out)
{
	SgEdit e;
	int i;

	out->n = 0;
	sg_edit_init(&e, op, t0, t1, collect, out);
	for (i = 0; i < nin; i++) sg_edit_feed(&e, in[i]);
	sg_edit_end(&e);
}

static int eq(const struct sink *s, const int32_t *exp, int n)
{
	int i;

	if (s->n != n) return 0;
	for (i = 0; i < n; i++) if (s->v[i] != exp[i]) return 0;
	return 1;
}

/* trim to a window fully inside one run clips both ends. */
static void test_trim_basic(void)
{
	/* +100, -100, +100, -100 : times 0,100,200,300 */
	const int32_t in[] = { 100, -100, 100, -100 };
	struct sink out;
	const int32_t exp[] = { -50, 100, -50 };	/* [150,350) */

	run(SG_EDIT_TRIM, 150, 350, in, 4, &out);
	CHECK(eq(&out, exp, 3));
}

/* trim aligned to boundaries keeps whole durations. */
static void test_trim_aligned(void)
{
	const int32_t in[] = { 100, -200, 100 };	/* 0,100,300 */
	struct sink out;
	const int32_t exp[] = { -200 };

	run(SG_EDIT_TRIM, 100, 300, in, 3, &out);
	CHECK(eq(&out, exp, 1));
}

/* cut a middle window: the first on-run's tail joins the second on-run's
 * head (both carrier-on), so they merge into one. */
static void test_cut_middle(void)
{
	/* +100,-100,+100,-100 ; cut [50,250): keep [0,50)+[250,300) on-runs
	 * (merge to +100) then the final -100 */
	const int32_t in[] = { 100, -100, 100, -100 };
	struct sink out;
	const int32_t exp[] = { 100, -100 };

	run(SG_EDIT_CUT, 50, 250, in, 4, &out);
	CHECK(eq(&out, exp, 2));
}

/* cut joins two same-level runs into one (the gap between is removed). */
static void test_cut_merges(void)
{
	/* +100,-100,+100 ; cut the middle -100 entirely -> +100 then +100
	 * become one +200 */
	const int32_t in[] = { 100, -100, 100 };
	struct sink out;
	const int32_t exp[] = { 200 };

	run(SG_EDIT_CUT, 100, 200, in, 3, &out);
	CHECK(eq(&out, exp, 1));
}

/* a duration that contains the whole hole keeps head+tail of its level. */
static void test_cut_inside_one(void)
{
	const int32_t in[] = { 300 };		/* one long +300 */
	struct sink out;
	const int32_t exp[] = { 200 };		/* remove 100 us from the middle */

	run(SG_EDIT_CUT, 100, 200, in, 1, &out);
	CHECK(eq(&out, exp, 1));
}

/* trimming to an empty / inverted window yields nothing. */
static void test_trim_empty(void)
{
	const int32_t in[] = { 100, -100 };
	struct sink out;

	run(SG_EDIT_TRIM, 200, 100, in, 2, &out);	/* t0 > t1 */
	CHECK(out.n == 0);
	run(SG_EDIT_TRIM, 500, 600, in, 2, &out);	/* past the end */
	CHECK(out.n == 0);
}

/* cut nothing (window past the end) returns the input, runs merged. */
static void test_cut_noop(void)
{
	const int32_t in[] = { 100, -100, 100 };
	struct sink out;
	const int32_t exp[] = { 100, -100, 100 };

	run(SG_EDIT_CUT, 1000, 2000, in, 3, &out);
	CHECK(eq(&out, exp, 3));
}

int main(void)
{
	test_trim_basic();
	test_trim_aligned();
	test_cut_middle();
	test_cut_merges();
	test_cut_inside_one();
	test_trim_empty();
	test_cut_noop();
	if (fails == 0) printf("sgedit: all tests passed\n");
	else printf("sgedit: %d checks FAILED\n", fails);
	return fails ? 1 : 0;
}
