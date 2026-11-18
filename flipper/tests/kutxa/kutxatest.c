/* kutxatest - libkutxa (src/lib/kutxa, spec section R) on the PC: the use
 * cases the user asked for (2026-11-18), ACR.1-ACR.9.  Written before the
 * library (test first, T33a makes it pass); it builds against kutxa.h.
 *
 *   basic       put/get/del, overwrite, long values, key limits, errors
 *   calendar    events keyed by start time: "this week", edit by id via an
 *               index, an index on the tag added later (ACR.2)
 *   reminders   10 000 reminders: "next one >= now" in <= height reads,
 *               fire = delete, the next one (ACR.3)
 *   logs        30 days of a sensor every minute, commit every 10 minutes:
 *               bytes written bounded, last 24 h backwards, delete older
 *               than 7 days, compaction smaller and equal (ACR.4)
 *   holidays    a packed read-only database: holiday rules per country and
 *               region (PL, DE, DE-BY, ES-PV), expanded for 2026 with the
 *               Easter date; name days in a second tree (ACR.5)
 *   irdb        100 000 IR codes bulk loaded: lookups <= 2.5 reads on
 *               average with a 4-page cache, file size near the data (ACR.6)
 *   schemaless  records gain a field later; an index on it built later
 *               equals a full scan; a broken record never over-reads (ACR.7)
 *   snapshot    a reader keeps its snapshot until kt_refresh (ACR.8)
 *   powercut    a power cut at every write and sync of a workload (the
 *               first 300, then every 1-40th); unsynced sectors are kept,
 *               lost or torn, each on its own: the file opens at the last
 *               commit or the one being made, never anything else; a
 *               damaged last commit rolls back (ACR.1)
 *   fuzz        mutated and random files never crash or loop (ACR.9)
 *   lock        a second writer gets EBUSY (real file, fcntl)
 *
 * Run by `make test` here; T33a adds it to tests/host (CI) and `make
 * kutxatest`.  Test data uses Basque words: bilera = meeting, lana = work,
 * mendia = mountain, urtebetetzea = birthday, kokapena = place.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kutxa.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static unsigned long rnd_state = 12345;
static unsigned long rnd(void)
{
	/* xorshift32: the same sequence on every host */
	unsigned long x = rnd_state & 0xFFFFFFFFUL;

	x ^= (x << 13) & 0xFFFFFFFFUL;
	x ^= x >> 17;
	x ^= (x << 5) & 0xFFFFFFFFUL;
	rnd_state = x;
	return x;
}

/* ================================================================== */
/* A file in memory that can lose power.                              */
/* ================================================================== */
#define SECT	512
#define MAXF	(160UL << 20)

struct mfile {
	unsigned char *cur;		/* what the program sees */
	unsigned long csize;
	unsigned char *dur;		/* what is on the card for sure */
	unsigned long dsize;
	unsigned char *pend;		/* 1 per sector written since the last sync */
	long cut_after;			/* writes + syncs until the power goes; -1 never */
	int dead;
	unsigned long writes, syncs;
};

static struct mfile *mf_new(void)
{
	struct mfile *m = calloc(1, sizeof *m);

	m->cur = calloc(1, MAXF);
	m->dur = calloc(1, MAXF);
	m->pend = calloc(1, MAXF / SECT);
	m->cut_after = -1;
	return m;
}

static void mf_free(struct mfile *m)
{
	free(m->cur); free(m->dur); free(m->pend); free(m);
}

static long mf_pread(void *h, void *buf, unsigned long n, unsigned long off)
{
	struct mfile *m = h;

	if (m->dead) { errno = EIO; return -1; }
	if (off >= m->csize) return 0;
	if (n > m->csize - off) n = m->csize - off;
	memcpy(buf, m->cur + off, n);
	return (long) n;
}

static long mf_pwrite(void *h, const void *buf, unsigned long n, unsigned long off)
{
	struct mfile *m = h;
	unsigned long s;

	if (m->dead) { errno = EIO; return -1; }
	if (m->cut_after == 0) { m->dead = 1; errno = EIO; return -1; }
	if (m->cut_after > 0) m->cut_after--;
	if (off + n > MAXF) { errno = ENOSPC; return -1; }
	memcpy(m->cur + off, buf, n);
	if (off + n > m->csize) m->csize = off + n;
	for (s = off / SECT; s * SECT < off + n; s++) m->pend[s] = 1;
	m->writes++;
	return (long) n;
}

static int mf_sync(void *h)
{
	struct mfile *m = h;

	if (m->dead) { errno = EIO; return -1; }
	if (m->cut_after == 0) { m->dead = 1; errno = EIO; return -1; }	/* during the sync */
	if (m->cut_after > 0) m->cut_after--;
	memcpy(m->dur, m->cur, m->csize > m->dsize ? m->csize : m->dsize);
	m->dsize = m->csize;
	memset(m->pend, 0, MAXF / SECT);
	m->syncs++;
	return 0;
}

static long mf_size(void *h) { return (long) ((struct mfile *) h)->csize; }

static int mf_truncate(void *h, unsigned long size)
{
	struct mfile *m = h;

	if (m->dead) { errno = EIO; return -1; }
	if (size < m->csize) memset(m->cur + size, 0, m->csize - size);
	m->csize = size;
	return 0;
}

static struct kt_io mf_io(struct mfile *m)
{
	struct kt_io io = { m, mf_pread, mf_pwrite, mf_sync, mf_size, mf_truncate };

	return io;
}

/* The power goes: every sector written since the last sync is kept, lost
 * or torn (first half new), each on its own, as an SD card may; the file
 * size is the old or the new one (the inode may not have reached the card). */
static void mf_powercut(struct mfile *m)
{
	unsigned long s, nsect = (m->csize + SECT - 1) / SECT;
	unsigned long size = (rnd() & 1) ? m->csize : m->dsize;

	for (s = 0; s < nsect; s++) {
		unsigned long o = s * SECT, r;

		if (!m->pend[s]) continue;
		r = rnd() % 8;
		if (r < 4) memcpy(m->dur + o, m->cur + o, SECT);	/* kept */
		else if (r == 4) memcpy(m->dur + o, m->cur + o, SECT / 2); /* torn */
		/* else lost: the old bytes (zeros past the old end) stay */
	}
	if (size > m->dsize) {
		/* bytes past the old end that never reached the card read as 0 */
		for (s = m->dsize / SECT; s < nsect; s++)
			if (!m->pend[s]) memset(m->dur + s * SECT, 0, SECT);
	}
	memcpy(m->cur, m->dur, size > m->csize ? size : m->csize);
	if (size < m->csize) memset(m->cur + size, 0, m->csize - size);
	m->csize = m->dsize = size;
	memset(m->pend, 0, MAXF / SECT);
	m->dead = 0;
	m->cut_after = -1;
}

static void mf_copy(struct mfile *dst, const struct mfile *src)
{
	memcpy(dst->cur, src->cur, src->csize);
	memcpy(dst->dur, src->cur, src->csize);
	dst->csize = dst->dsize = src->csize;
	dst->dead = 0;
	dst->cut_after = -1;
}

/* ================================================================== */
/* A reference model: a sorted array of keys and values.              */
/* ================================================================== */
struct ment { unsigned char k[KT_MAXKEY]; size_t kl; unsigned char *v; size_t vl; };
struct model { struct ment *e; size_t n, cap; };

static int kcmp(const void *a, size_t al, const void *b, size_t bl)
{
	int c = memcmp(a, b, al < bl ? al : bl);

	return c ? c : (al > bl) - (al < bl);
}

static size_t m_find(const struct model *m, const void *k, size_t kl, int *found)
{
	size_t lo = 0, hi = m->n;

	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		int c = kcmp(m->e[mid].k, m->e[mid].kl, k, kl);

		if (c == 0) { *found = 1; return mid; }
		if (c < 0) lo = mid + 1; else hi = mid;
	}
	*found = 0;
	return lo;
}

static void m_put(struct model *m, const void *k, size_t kl, const void *v, size_t vl)
{
	int f;
	size_t i = m_find(m, k, kl, &f);

	if (!f) {
		if (m->n == m->cap) {
			m->cap = m->cap ? m->cap * 2 : 256;
			m->e = realloc(m->e, m->cap * sizeof *m->e);
		}
		memmove(m->e + i + 1, m->e + i, (m->n - i) * sizeof *m->e);
		m->n++;
		memcpy(m->e[i].k, k, kl);
		m->e[i].kl = kl;
	} else {
		free(m->e[i].v);
	}
	m->e[i].v = malloc(vl ? vl : 1);
	memcpy(m->e[i].v, v, vl);
	m->e[i].vl = vl;
}

static void m_del(struct model *m, const void *k, size_t kl)
{
	int f;
	size_t i = m_find(m, k, kl, &f);

	if (!f) return;
	free(m->e[i].v);
	memmove(m->e + i, m->e + i + 1, (m->n - i - 1) * sizeof *m->e);
	m->n--;
}

static void m_free(struct model *m)
{
	size_t i;

	for (i = 0; i < m->n; i++) free(m->e[i].v);
	free(m->e);
	m->e = NULL;
	m->n = m->cap = 0;
}

static void m_dup(struct model *d, const struct model *s)
{
	size_t i;

	m_free(d);
	for (i = 0; i < s->n; i++) m_put(d, s->e[i].k, s->e[i].kl, s->e[i].v, s->e[i].vl);
}

/* Does tree t hold exactly the model?  Full forward scan. */
static int same_as(KT *db, int t, const struct model *m)
{
	struct kt_cur c;
	static unsigned char k[KT_MAXKEY], v[KT_MAXVAL];
	size_t kl, vl, i = 0;

	if (kt_seek(db, t, &c, "", 0) != 0) return m->n == 0 && errno == ENOENT;
	for (;;) {
		kl = sizeof k; vl = sizeof v;
		if (kt_next(&c, k, &kl, v, &vl) != 0) break;
		if (i >= m->n || kcmp(k, kl, m->e[i].k, m->e[i].kl) != 0
		    || vl != m->e[i].vl || memcmp(v, m->e[i].v, vl) != 0)
			return 0;
		i++;
	}
	return errno == ENOENT && i == m->n;
}

/* ================================================================== */
static unsigned char mem1[KT_MEM(8)], mem2[KT_MEM(8)];

static KT *open_m(struct mfile *f, struct kt_io *io, void *mem, size_t ms, int fl)
{
	*io = mf_io(f);
	return kt_open_io(io, mem, ms, fl | KT_CREAT | KT_NOLOCK);
}

static void test_basic(void)
{
	struct mfile *f = mf_new();
	struct kt_io io;
	KT *db = open_m(f, &io, mem1, sizeof mem1, 0);
	static unsigned char big[10000], out[10000], key[KT_MAXKEY + 1];
	size_t vl, i;
	int r;

	CHECK(db != NULL, "open a new file");
	if (!db) { mf_free(f); return; }
	CHECK(kt_put(db, 0, "etxea", 5, "house", 5) == 0, "put");
	vl = sizeof out;
	CHECK(kt_get(db, 0, "etxea", 5, out, &vl) == 0 && vl == 5
	      && memcmp(out, "house", 5) == 0, "get before commit");
	CHECK(kt_put(db, 0, "etxea", 5, "home", 4) == 0, "overwrite");
	vl = sizeof out;
	CHECK(kt_get(db, 0, "etxea", 5, out, &vl) == 0 && vl == 4, "overwritten");
	vl = 2;
	CHECK(kt_get(db, 0, "etxea", 5, out, &vl) == -1 && errno == E2BIG && vl == 4,
	      "E2BIG gives the size");
	vl = sizeof out;
	CHECK(kt_get(db, 0, "ez", 2, out, &vl) == -1 && errno == ENOENT, "ENOENT");
	for (i = 0; i < sizeof big; i++) big[i] = (unsigned char) rnd();
	CHECK(kt_put(db, 0, "luzea", 5, big, sizeof big) == 0, "10 KB value");
	memset(key, 'k', sizeof key);
	CHECK(kt_put(db, 0, key, KT_MAXKEY, "x", 1) == 0, "255-byte key");
	CHECK(kt_put(db, 0, key, KT_MAXKEY + 1, "x", 1) == -1 && errno == EINVAL,
	      "256-byte key refused");
	CHECK(kt_put(db, 0, "", 0, "x", 1) == -1 && errno == EINVAL, "empty key refused");
	CHECK(kt_commit(db) == 0, "commit");
	CHECK(kt_del(db, 0, "etxea", 5) == 0, "del");
	kt_abort(db);
	vl = sizeof out;
	CHECK(kt_get(db, 0, "etxea", 5, out, &vl) == 0, "abort keeps the commit");
	kt_close(db);

	db = open_m(f, &io, mem1, sizeof mem1, KT_RDONLY);
	vl = sizeof out;
	CHECK(db && kt_get(db, 0, "luzea", 5, out, &vl) == 0 && vl == sizeof big
	      && memcmp(out, big, vl) == 0, "10 KB value after reopen");
	r = db ? kt_put(db, 0, "a", 1, "b", 1) : 0;
	CHECK(r == -1 && errno == EROFS, "EROFS on a reader");
	if (db) kt_close(db);

	CHECK(kt_open_io(&io, mem1, KT_MEM(2), KT_NOLOCK) == NULL
	      && (errno == EINVAL || errno == ENOMEM), "cache below 3 pages refused");
	mf_free(f);
}

/* ---- calendar (ACR.2) ---- */
#define T2026	1767225600LL		/* 2026-01-01T00:00:00Z */
#define DAY	86400LL
static const char *titles[] = { "bilera", "mendia", "urtebetetzea", "dentista", "lana" };
static const char *tags[] = { "lana", "etxea", "lagunak" };

static size_t ev_key(unsigned char *k, long long start, unsigned long id)
{
	size_t n = kt_key_i64(k, start);

	k[n++] = (unsigned char) (id >> 24); k[n++] = (unsigned char) (id >> 16);
	k[n++] = (unsigned char) (id >> 8); k[n++] = (unsigned char) id;
	return n;
}

static size_t ev_rec(unsigned char *buf, size_t cap, long long start, unsigned long id,
		     const char *title, const char *tag)
{
	struct krec r;

	kr_init(&r, buf, cap);
	kr_putint(&r, "id", (long long) id);
	kr_puttime(&r, "start", start);
	kr_putstr(&r, "title", title);
	kr_putstr(&r, "tag", tag);
	return r.len;
}

static void test_calendar(void)
{
	struct mfile *f = mf_new();
	struct kt_io io;
	KT *db = open_m(f, &io, mem1, sizeof mem1, 0);
	struct model m = { 0 };
	unsigned char k[64], v[256], ik[64], kk[KT_MAXKEY], vv[512];
	struct kt_cur c;
	size_t kl, vl, n, i, ikl, want, got;
	long long start, wk0 = T2026 + 320 * DAY, wk1 = wk0 + 7 * DAY;
	unsigned long id;
	int idx_id, idx_tag;
	const void *pk;
	size_t pkl;

	if (!db) { CHECK(0, "open"); mf_free(f); return; }
	idx_id = kt_index(db, 0, "cal.id", "id", 1000000);
	CHECK(idx_id > 0, "index on id");
	for (id = 1; id <= 2000; id++) {
		start = T2026 + (long long) (rnd() % (730 * 24)) * 3600;
		kl = ev_key(k, start, id);
		vl = ev_rec(v, sizeof v, start, id, titles[rnd() % 5], tags[rnd() % 3]);
		CHECK(kt_put(db, 0, k, kl, v, vl) == 0, "put event %lu", id);
		m_put(&m, k, kl, v, vl);
		if (id % 100 == 0) CHECK(kt_commit(db) == 0, "commit");
	}
	CHECK(same_as(db, 0, &m), "2000 events equal the model");

	/* this week: one seek, a short scan, in time order */
	kl = kt_key_i64(k, wk0);
	CHECK(kt_seek(db, 0, &c, k, kl) == 0, "seek week start");
	n = 0;
	for (;;) {
		kl = sizeof kk; vl = sizeof vv;
		if (kt_next(&c, kk, &kl, vv, &vl) != 0) break;
		if (kt_key_geti64(kk) >= wk1) break;
		n++;
	}
	want = 0;
	for (i = 0; i < m.n; i++) {
		start = kt_key_geti64(m.e[i].k);
		if (start >= wk0 && start < wk1) want++;
	}
	CHECK(n == want, "week: %zu events, model %zu", n, want);

	/* edit by id: event 777 moves one day later; the index follows */
	ikl = kt_ixkey(ik, sizeof ik, KR_INT, "\0\0\0\0\0\0\x03\x09", 8);
	CHECK(kt_seek(db, idx_id, &c, ik, ikl) == 0, "seek id 777");
	kl = sizeof kk; vl = sizeof vv;
	CHECK(kt_next(&c, kk, &kl, vv, &vl) == 0 && kl > ikl
	      && memcmp(kk, ik, ikl) == 0, "index has id 777");
	CHECK(kt_ixsplit(kk, kl, &pk, &pkl) == 0 && pkl == 12, "primary key in index key");
	start = kt_key_geti64(pk);
	memcpy(k, pk, pkl);
	CHECK(kt_del(db, 0, k, pkl) == 0, "delete old 777");
	m_del(&m, k, pkl);
	kl = ev_key(k, start + DAY, 777);
	vl = ev_rec(v, sizeof v, start + DAY, 777, "bilera", "lana");
	CHECK(kt_put(db, 0, k, kl, v, vl) == 0, "put moved 777");
	m_put(&m, k, kl, v, vl);
	CHECK(kt_commit(db) == 0, "commit edit");
	CHECK(kt_seek(db, idx_id, &c, ik, ikl) == 0, "seek id 777 again");
	kl = sizeof kk; vl = sizeof vv;
	CHECK(kt_next(&c, kk, &kl, vv, &vl) == 0 && kt_ixsplit(kk, kl, &pk, &pkl) == 0
	      && kt_key_geti64(pk) == start + DAY, "index points to the moved event");
	kl = sizeof kk; vl = sizeof vv;
	CHECK(kt_next(&c, kk, &kl, vv, &vl) != 0 || memcmp(kk, ik, ikl) != 0,
	      "only one index entry for 777");

	/* an index on the tag, added later, equals a scan */
	idx_tag = kt_index(db, 0, "cal.tag", "tag", 1000000);
	CHECK(idx_tag > 0 && kt_commit(db) == 0, "index on tag");
	ikl = kt_ixkey(ik, sizeof ik, KR_STR, "lana", 4);
	got = 0;
	if (kt_seek(db, idx_tag, &c, ik, ikl) == 0)
		for (;;) {
			kl = sizeof kk; vl = sizeof vv;
			if (kt_next(&c, kk, &kl, vv, &vl) != 0 || memcmp(kk, ik, ikl) != 0) break;
			got++;
		}
	want = 0;
	for (i = 0; i < m.n; i++) {
		struct kfield fl;

		if (kr_get(m.e[i].v, m.e[i].vl, "tag", &fl) == 0 && fl.vlen == 4
		    && memcmp(fl.val, "lana", 4) == 0) want++;
	}
	CHECK(got == want && want > 0, "tag=lana: index %zu, scan %zu", got, want);
	kt_close(db);
	m_free(&m);
	mf_free(f);
}

/* ---- reminders (ACR.3) ---- */
static void test_reminders(void)
{
	struct mfile *f = mf_new();
	struct kt_io io;
	static unsigned char mem[KT_MEM(4)];
	KT *db = open_m(f, &io, mem, sizeof mem, 0);
	struct model m = { 0 };
	unsigned char k[16], kk[KT_MAXKEY], vv[64];
	struct kt_cur c;
	struct kt_stat s0, s1;
	size_t kl, vl, i, fi;
	long long now = T2026 + 100 * DAY;
	int fnd;

	if (!db) { CHECK(0, "open"); mf_free(f); return; }
	for (i = 0; i < 10000; i++) {
		long long t = T2026 + (long long) (rnd() % (365 * 1440)) * 60;

		kl = ev_key(k, t, (unsigned long) i);
		kt_put(db, 0, k, kl, "ohartu", 6);	/* "notice" */
		m_put(&m, k, kl, "ohartu", 6);
	}
	CHECK(kt_commit(db) == 0, "commit 10 000");
	kt_stat(db, &s0);
	CHECK(s0.height[0] >= 2 && s0.height[0] <= 3, "height %d", s0.height[0]);
	for (i = 0; i < 50; i++) {
		kl = kt_key_i64(k, now);
		kt_stat(db, &s0);
		CHECK(kt_seek(db, 0, &c, k, kl) == 0, "next >= now");
		kl = sizeof kk; vl = sizeof vv;
		CHECK(kt_next(&c, kk, &kl, vv, &vl) == 0, "a next reminder");
		kt_stat(db, &s1);
		CHECK(s1.reads - s0.reads <= s0.height[0], "reads %lu > height",
		      s1.reads - s0.reads);
		fi = m_find(&m, k, kt_key_i64(k, now), &fnd);
		CHECK(fi < m.n && kcmp(kk, kl, m.e[fi].k, m.e[fi].kl) == 0, "the right one");
		/* it fires: delete it, the clock moves on */
		now = kt_key_geti64(kk) + 1;
		CHECK(kt_del(db, 0, kk, kl) == 0 && kt_commit(db) == 0, "fired");
		m_del(&m, kk, kl);
	}
	CHECK(same_as(db, 0, &m), "reminders equal the model");
	kt_close(db);
	m_free(&m);
	mf_free(f);
}

/* ---- logs (ACR.4) ---- */
static void test_logs(void)
{
	struct mfile *f = mf_new(), *g = mf_new();
	struct kt_io io, io2;
	KT *db = open_m(f, &io, mem1, sizeof mem1, 0), *db2;
	struct model m = { 0 };
	unsigned char k[16], v[32], kk[KT_MAXKEY], vv[64];
	struct kt_cur c;
	struct kt_stat st;
	size_t kl, vl, n;
	long long t, end = T2026 + 30 * DAY;
	unsigned long commits = 0, data = 0;
	long del;
	struct krec r;
	int more;

	if (!db) { CHECK(0, "open"); mf_free(f); mf_free(g); return; }
	for (t = T2026; t < end; t += 60) {
		kl = kt_key_i64(k, t);
		kr_init(&r, v, sizeof v);
		kr_putint(&r, "c", 180 + (long long) (rnd() % 60));	/* 0.1 degC */
		kr_putint(&r, "rh", 30 + (long long) (rnd() % 50));
		kt_put(db, 0, k, kl, r.buf, r.len);
		m_put(&m, k, kl, r.buf, r.len);
		data += kl + r.len;
		if ((t / 60) % 10 == 9) { CHECK(kt_commit(db) == 0, "commit"); commits++; }
	}
	kt_commit(db); commits++;
	kt_stat(db, &st);
	CHECK(same_as(db, 0, &m), "43 200 readings equal the model");
	/* append path: only the right edge changes, so each commit writes at
	 * most the path plus the commit record, and the data once */
	CHECK(st.written <= commits * ((unsigned long) st.height[0] + 1) * KT_PAGE + 2 * data,
	      "written %lu B for %lu B of data in %lu commits", st.written, data, commits);

	/* the last 24 hours, newest first */
	CHECK(kt_seeklast(db, 0, &c) == 0, "seek last");
	n = 0;
	for (;;) {
		kl = sizeof kk; vl = sizeof vv;
		if (kt_prev(&c, kk, &kl, vv, &vl) != 0) break;
		if (kt_key_geti64(kk) < end - DAY) break;
		n++;
	}
	CHECK(n == 1440, "last 24 h: %zu readings", n);

	/* keep a week */
	kl = kt_key_i64(k, end - 7 * DAY);
	del = kt_delrange(db, 0, NULL, 0, k, kl);
	CHECK(del == 23 * 1440, "deleted %ld, want %d", del, 23 * 1440);
	for (t = T2026; t < end - 7 * DAY; t += 60) { kl = kt_key_i64(k, t); m_del(&m, k, kl); }
	CHECK(kt_commit(db) == 0 && same_as(db, 0, &m), "a week left");

	/* compaction: smaller, same contents */
	io2 = mf_io(g);
	do more = kt_compact_io(db, &io2, 64); while (more == 1);
	CHECK(more == 0, "compaction done");
	db2 = kt_open_io(&io2, mem2, sizeof mem2, KT_RDONLY | KT_NOLOCK);
	CHECK(db2 && same_as(db2, 0, &m), "compacted equals");
	CHECK(g->csize < f->csize / 2, "compacted %lu of %lu B", g->csize, f->csize);
	if (db2) kt_close(db2);
	kt_close(db);
	m_free(&m);
	mf_free(f); mf_free(g);
}

/* ---- holidays and name days (ACR.5) ---- */
/* Rules as the holiday app stores them: "MM-DD" fixed, "E+n" Easter + n
 * days (Gregorian).  Real data for these four, from their laws. */
static const char *hol[][3] = {
	{ "DE", "01-01", "Neujahr" }, { "DE", "05-01", "Tag der Arbeit" },
	{ "DE", "10-03", "Tag der Deutschen Einheit" }, { "DE", "12-25", "1. Weihnachtstag" },
	{ "DE", "12-26", "2. Weihnachtstag" }, { "DE", "E-2", "Karfreitag" },
	{ "DE", "E+1", "Ostermontag" }, { "DE", "E+39", "Christi Himmelfahrt" },
	{ "DE", "E+50", "Pfingstmontag" },
	{ "DE-BY", "01-06", "Heilige Drei Könige" }, { "DE-BY", "11-01", "Allerheiligen" },
	{ "DE-BY", "E+60", "Fronleichnam" },
	{ "ES-PV", "E+1", "Lunes de Pascua / Aste Santuko astelehena" },
	{ "PL", "01-01", "Nowy Rok" }, { "PL", "01-06", "Trzech Króli" },
	{ "PL", "05-01", "Święto Pracy" }, { "PL", "05-03", "Święto Konstytucji 3 Maja" },
	{ "PL", "08-15", "Wniebowzięcie NMP" }, { "PL", "11-01", "Wszystkich Świętych" },
	{ "PL", "11-11", "Narodowe Święto Niepodległości" }, { "PL", "12-24", "Wigilia" },
	{ "PL", "12-25", "Boże Narodzenie" }, { "PL", "12-26", "Drugi dzień Bożego Narodzenia" },
	{ "PL", "E+0", "Wielkanoc" }, { "PL", "E+1", "Poniedziałek Wielkanocny" },
	{ "PL", "E+49", "Zielone Świątki" }, { "PL", "E+60", "Boże Ciało" },
};
#define NHOL (sizeof hol / sizeof hol[0])

/* Gregorian Easter (Meeus/Jones/Butcher): day of year from 0. */
static int easter_yday(int y)
{
	int a = y % 19, b = y / 100, c = y % 100, d = b / 4, e = b % 4;
	int f = (b + 8) / 25, g = (b - f + 1) / 3, h = (19 * a + b - d - g + 15) % 30;
	int i = c / 4, k = c % 4, l = (32 + 2 * e + 2 * i - h - k) % 7;
	int m = (a + 11 * h + 22 * l) / 451, mon = (h + l - 7 * m + 114) / 31;
	int day = (h + l - 7 * m + 114) % 31 + 1;
	static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;

	return cum[mon - 1] + day - 1 + (mon > 2 && leap);
}

static int rule_yday(const char *rule, int y)
{
	static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0, mon, day;

	if (rule[0] == 'E') return easter_yday(y) + atoi(rule + 1);
	mon = atoi(rule); day = atoi(rule + 3);
	return cum[mon - 1] + day - 1 + (mon > 2 && leap);
}

/* the region's own rules and its parent country's ("DE-BY" -> "DE") */
static int region_days(KT *db, int t, const char *region, int year, int *yd, int max)
{
	char pre[8];
	unsigned char kk[KT_MAXKEY + 1], vv[256];
	struct kt_cur c;
	size_t kl, vl, pl;
	int n = 0, pass;

	for (pass = 0; pass < 2; pass++) {
		if (pass == 0) snprintf(pre, sizeof pre, "%.2s/", region);
		else if (strlen(region) > 2) snprintf(pre, sizeof pre, "%s/", region);
		else break;
		pl = strlen(pre);
		if (kt_seek(db, t, &c, pre, pl) != 0) continue;
		for (;;) {
			kl = sizeof kk; vl = sizeof vv;
			if (kt_next(&c, kk, &kl, vv, &vl) != 0 || kl < pl || memcmp(kk, pre, pl)) break;
			kk[kl] = 0;
			if (n < max) yd[n++] = rule_yday((char *) kk + pl, year);
		}
	}
	return n;
}

static int icmp(const void *a, const void *b) { return *(const int *) a - *(const int *) b; }

static void test_holidays(void)
{
	struct mfile *f = mf_new();
	struct kt_io io;
	KT *db = open_m(f, &io, mem1, sizeof mem1, 0);
	struct model m = { 0 };
	char k[32];
	unsigned char out[256];
	size_t i, vl;
	int t, yd[64], n;
	/* PL 2026: Easter is 5 April, Corpus Christi 4 June */
	static const int pl2026[] = { 0, 5, 94, 95, 120, 122, 143, 154, 226, 304, 314, 357, 358, 359 };
	static const char *names[][2] = {	/* test data, not a real calendar */
		{ "01-20", "Sebastian" }, { "06-24", "Ion, Joanes" },
		{ "07-31", "Iñaki, Ignazio" }, { "11-19", "Ane, Miren" },
	};

	if (!db) { CHECK(0, "open"); mf_free(f); return; }
	for (i = 0; i < NHOL; i++) {
		snprintf(k, sizeof k, "%s/%s", hol[i][0], hol[i][1]);
		m_put(&m, k, strlen(k), hol[i][2], strlen(hol[i][2]));
	}
	/* the packed build: keys ascending (the model is sorted) */
	CHECK(kt_load_begin(db, 0) == 0, "load begin");
	for (i = 0; i < m.n; i++)
		CHECK(kt_load(db, m.e[i].k, m.e[i].kl, m.e[i].v, m.e[i].vl) == 0, "load %zu", i);
	CHECK(kt_load_end(db) == 0, "load end");
	t = kt_tree(db, "izenak", KT_CREAT);	/* name days */
	CHECK(t > 0 && kt_load_begin(db, t) == 0, "second tree");
	for (i = 0; i < 4; i++)
		kt_load(db, names[i][0], 5, names[i][1], strlen(names[i][1]));
	CHECK(kt_load_end(db) == 0, "names loaded");
	CHECK(kt_load_begin(db, t) == -1 && errno == EINVAL, "load into a non-empty tree");
	kt_close(db);

	db = open_m(f, &io, mem1, sizeof mem1, KT_RDONLY);
	if (!db) { CHECK(0, "reopen"); m_free(&m); mf_free(f); return; }
	CHECK(same_as(db, 0, &m), "holidays equal");
	vl = sizeof out;
	CHECK(kt_get(db, 0, "PL/11-11", 8, out, &vl) == 0
	      && vl == strlen("Narodowe Święto Niepodległości"), "PL 11 November");
	n = region_days(db, 0, "PL", 2026, yd, 64);
	qsort(yd, (size_t) n, sizeof *yd, icmp);
	CHECK(n == 14 && memcmp(yd, pl2026, sizeof pl2026) == 0, "PL 2026: %d days", n);
	n = region_days(db, 0, "DE-BY", 2026, yd, 64);
	CHECK(n == 12, "DE-BY = DE + 3 own: %d", n);
	n = region_days(db, 0, "DE", 2026, yd, 64);
	CHECK(n == 9, "DE alone: %d", n);
	n = region_days(db, 0, "ES-PV", 2026, yd, 64);
	CHECK(n == 1 && yd[0] == 95, "ES-PV Easter Monday 6 April 2026");
	t = kt_tree(db, "izenak", 0);
	vl = sizeof out;
	CHECK(t > 0 && kt_get(db, t, "11-19", 5, out, &vl) == 0 && vl == 10, "name day");
	CHECK(kt_tree(db, "ez-dago", 0) == -1 && errno == ENOENT, "no such tree");
	kt_close(db);
	m_free(&m);
	mf_free(f);
}

/* ---- IR code database, bulk (ACR.6) ---- */
static void test_irdb(void)
{
	struct mfile *f = mf_new();
	struct kt_io io;
	static unsigned char mem[KT_MEM(4)];
	KT *db = open_m(f, &io, mem, sizeof mem, 0);
	char k[64];
	unsigned char v[16], out[64];
	struct kt_stat s0, s1;
	unsigned long raw = 0, i, maxr = 0, sumr = 0;
	size_t vl;
	static const char *btn[] = { "Ch_next", "Mute", "Power", "Vol_dn", "Vol_up" };	/* sorted */

	if (!db) { CHECK(0, "open"); mf_free(f); return; }
	CHECK(kt_load_begin(db, 0) == 0, "load begin");
	for (i = 0; i < 100000; i++) {
		/* ascending: brand number, model number, button number */
		snprintf(k, sizeof k, "b%04lu/m%03lu/%s", i / 500, (i / 5) % 100, btn[i % 5]);
		memset(v, (int) i, sizeof v);	/* protocol, address, command */
		CHECK(kt_load(db, k, strlen(k), v, sizeof v) == 0, "load %lu", i);
		raw += strlen(k) + sizeof v;
	}
	CHECK(kt_load(db, "a", 1, v, 1) == -1 && errno == EINVAL, "descending key refused");
	CHECK(kt_load_end(db) == 0, "load end");
	CHECK(f->csize < raw * 13 / 10 + 16 * KT_PAGE, "packed %lu B for %lu B", f->csize, raw);
	kt_stat(db, &s0);
	for (i = 0; i < 2000; i++) {
		unsigned long j = rnd() % 100000;

		snprintf(k, sizeof k, "b%04lu/m%03lu/%s", j / 500, (j / 5) % 100, btn[j % 5]);
		kt_stat(db, &s0);
		vl = sizeof out;
		CHECK(kt_get(db, 0, k, strlen(k), out, &vl) == 0 && vl == 16
		      && out[0] == (unsigned char) j, "lookup %s", k);
		kt_stat(db, &s1);
		sumr += s1.reads - s0.reads;
		if (s1.reads - s0.reads > maxr) maxr = s1.reads - s0.reads;
	}
	CHECK(maxr <= s1.height[0], "max reads %lu > height %d", maxr, s1.height[0]);
	CHECK(sumr * 10 <= 2000 * 25, "average reads %.2f > 2.5", sumr / 2000.0);
	kt_close(db);
	mf_free(f);
}

/* ---- records without a schema (ACR.7) ---- */
static void test_schemaless(void)
{
	struct mfile *f = mf_new();
	struct kt_io io;
	KT *db = open_m(f, &io, mem1, sizeof mem1, 0);
	unsigned char v[128], kk[KT_MAXKEY], vv[256], ik[64], bad[64];
	struct krec r;
	struct kfield fl;
	struct kt_cur c;
	char k[16];
	size_t kl, vl, pos, ikl, got = 0, i;
	int idx;
	long long x;

	if (!db) { CHECK(0, "open"); mf_free(f); return; }
	for (i = 0; i < 500; i++) {		/* old records: no place */
		kr_init(&r, v, sizeof v);
		kr_putstr(&r, "izena", "landare");	/* name: plant */
		kr_putint(&r, "ura", (long long) i);	/* water, ml */
		snprintf(k, sizeof k, "p%04zu", i);
		kt_put(db, 0, k, strlen(k), r.buf, r.len);
	}
	for (i = 500; i < 1000; i++) {		/* new ones: with place */
		kr_init(&r, v, sizeof v);
		kr_putstr(&r, "izena", "landare");
		kr_putint(&r, "ura", (long long) i);
		kr_putstr(&r, "kokapena", i % 2 ? "balkoia" : "sukaldea");
		snprintf(k, sizeof k, "p%04zu", i);
		kt_put(db, 0, k, strlen(k), r.buf, r.len);
	}
	CHECK(kt_commit(db) == 0, "commit");
	vl = sizeof vv;
	CHECK(kt_get(db, 0, "p0001", 5, vv, &vl) == 0
	      && kr_get(vv, vl, "kokapena", &fl) == -1 && errno == ENOENT, "old: no place");
	CHECK(kr_getint(vv, vl, "ura", &x) == 0 && x == 1, "old: water");
	vl = sizeof vv;
	CHECK(kt_get(db, 0, "p0999", 5, vv, &vl) == 0
	      && kr_get(vv, vl, "kokapena", &fl) == 0 && fl.type == KR_STR
	      && fl.vlen == 7 && memcmp(fl.val, "balkoia", 7) == 0, "new: place");
	pos = 0; i = 0;
	while (kr_next(vv, vl, &pos, &fl) == 0) i++;
	CHECK(i == 3 && errno == ENOENT, "three fields walked");

	idx = kt_index(db, 0, "p.kokapena", "kokapena", 1000000);
	CHECK(idx > 0 && kt_commit(db) == 0, "index added later");
	ikl = kt_ixkey(ik, sizeof ik, KR_STR, "balkoia", 7);
	if (kt_seek(db, idx, &c, ik, ikl) == 0)
		for (;;) {
			kl = sizeof kk; vl = sizeof vv;
			if (kt_next(&c, kk, &kl, vv, &vl) != 0 || memcmp(kk, ik, ikl) != 0) break;
			got++;
		}
	CHECK(got == 250, "balkoia: %zu via index", got);

	/* a broken record: lengths that point past the end */
	memset(bad, 0, sizeof bad);
	bad[0] = 60; memcpy(bad + 1, "izena", 5);
	pos = 0;
	CHECK(kr_next(bad, 8, &pos, &fl) == -1 && errno == EIO, "bad name length");
	bad[0] = 2; bad[1] = 'a'; bad[2] = 'b'; bad[3] = KR_STR; bad[4] = 0xFF; bad[5] = 0x7F;
	pos = 0;
	CHECK(kr_next(bad, 6, &pos, &fl) == -1 && errno == EIO, "bad value length");
	kt_close(db);
	mf_free(f);
}

/* ---- snapshots (ACR.8) ---- */
static void test_snapshot(void)
{
	struct mfile *f = mf_new();
	struct kt_io io, io2;
	KT *w = open_m(f, &io, mem1, sizeof mem1, 0), *r;
	unsigned char out[16];
	size_t vl;

	if (!w) { CHECK(0, "open"); mf_free(f); return; }
	kt_put(w, 0, "egoera", 6, "zaharra", 7);	/* state: old */
	kt_commit(w);
	io2 = mf_io(f);
	r = kt_open_io(&io2, mem2, sizeof mem2, KT_RDONLY | KT_NOLOCK);
	CHECK(r != NULL, "reader");
	if (!r) { kt_close(w); mf_free(f); return; }
	kt_put(w, 0, "egoera", 6, "berria", 6);		/* state: new */
	kt_commit(w);
	vl = sizeof out;
	CHECK(kt_get(r, 0, "egoera", 6, out, &vl) == 0 && vl == 7, "reader keeps the old");
	CHECK(kt_refresh(r) == 1, "refresh takes the new commit");
	vl = sizeof out;
	CHECK(kt_get(r, 0, "egoera", 6, out, &vl) == 0 && vl == 6, "reader sees the new");
	CHECK(kt_refresh(r) == 0, "nothing newer");
	kt_close(r);
	kt_close(w);
	mf_free(f);
}

/* ---- power cuts (ACR.1) ---- */
#define PC_OPS	600

/* One workload: random puts and deletes, a commit every 1-40 changes.
 * snap[i] = the model after the i-th successful commit (snap[0] = empty);
 * snap[ns+1] = what the commit the cut hit would have made (*during = 1).
 * Returns ns, the number of commits that returned 0. */
static int workload(struct mfile *f, struct model *snap, int maxsnap, int *during)
{
	struct kt_io io;
	KT *db = open_m(f, &io, mem1, sizeof mem1, 0);
	struct model cur = { 0 };
	unsigned char k[24], v[600];
	size_t kl, vl;
	int i, ns = 0, left = 1 + (int) (rnd() % 40), last;

	*during = 0;
	if (!db) return 0;
	for (i = 0; i <= PC_OPS; i++) {
		last = i == PC_OPS;
		if (!last) {
			kl = 4 + rnd() % 12;
			snprintf((char *) k, sizeof k, "%0*lu", (int) kl, rnd() % 3000);
			if (rnd() % 4 == 0) {
				if (kt_del(db, 0, k, kl) != 0 && errno != ENOENT) break;
				m_del(&cur, k, kl);
			} else {
				vl = rnd() % 8 == 0 ? 450 + rnd() % 150 : rnd() % 60;
				memset(v, (int) rnd(), vl);
				if (kt_put(db, 0, k, kl, v, vl) != 0) break;
				m_put(&cur, k, kl, v, vl);
			}
		}
		if (--left == 0 || last) {
			if (ns + 1 >= maxsnap) break;
			m_dup(&snap[ns + 1], &cur);
			if (kt_commit(db) != 0) { *during = 1; break; }
			ns++;
			left = 1 + (int) (rnd() % 40);
		}
	}
	kt_close(db);
	m_free(&cur);
	return ns;
}

static void test_powercut(void)
{
	struct mfile *f = mf_new();
	static struct model snap[PC_OPS + 3];
	struct kt_io io;
	KT *db;
	unsigned long total, cut, runs = 0, bad = 0, seed;
	int ns, during, s, ok;

	/* how many writes does the whole workload make? */
	seed = rnd_state;
	ns = workload(f, snap, PC_OPS + 3, &during);
	CHECK(ns > 10 && !during, "workload commits %d", ns);
	total = f->writes + f->syncs;
	for (cut = 0; cut < total + 2; cut += (cut < 300 ? 1 : 1 + rnd() % 40)) {
		struct mfile *g = mf_new();

		for (s = 0; s < PC_OPS + 3; s++) m_free(&snap[s]);
		rnd_state = seed;
		g->cut_after = (long) cut;
		ns = workload(g, snap, PC_OPS + 3, &during);
		rnd_state = seed + cut;		/* what the card keeps */
		mf_powercut(g);
		db = open_m(g, &io, mem1, sizeof mem1, 0);
		ok = db != NULL;
		if (ok) {
			/* the last finished commit, or the one the cut hit */
			ok = same_as(db, 0, &snap[ns]) || (during && same_as(db, 0, &snap[ns + 1]));
			ok = ok && kt_check(db, NULL, NULL) == 0;
			/* and it keeps working */
			ok = ok && kt_put(db, 0, "gero", 4, "bai", 3) == 0 && kt_commit(db) == 0;
			kt_close(db);
		}
		if (!ok) {
			bad++;
			if (bad <= 5) printf("  cut after write %lu: wrong state\n", cut);
		}
		runs++;
		mf_free(g);
	}
	CHECK(bad == 0, "power cuts: %lu of %lu runs bad", bad, runs);
	for (s = 0; s < PC_OPS + 3; s++) m_free(&snap[s]);
	mf_free(f);
}

/* The card damages pages of the last commit (written long ago in the same
 * erase block, as some cards do on a power cut): open at the commit before. */
static void test_rollback(void)
{
	struct mfile *f = mf_new();
	struct kt_io io;
	KT *db = open_m(f, &io, mem1, sizeof mem1, 0);
	struct model m = { 0 };
	struct kt_stat st;
	unsigned long before, end;
	char k[16];
	int i;

	if (!db) { CHECK(0, "open"); mf_free(f); return; }
	for (i = 0; i < 300; i++) {
		snprintf(k, sizeof k, "k%05d", i);
		kt_put(db, 0, k, strlen(k), "zaharra", 7);
		m_put(&m, k, strlen(k), "zaharra", 7);
	}
	kt_commit(db);
	before = f->csize;
	for (i = 0; i < 300; i += 7) {
		snprintf(k, sizeof k, "k%05d", i);
		kt_put(db, 0, k, strlen(k), "berria", 6);
	}
	kt_commit(db);
	kt_close(db);
	end = f->csize;
	/* damage one page of the last commit, not the commit record */
	f->cur[before + (end - before) / 3] ^= 0x5A;
	db = open_m(f, &io, mem1, sizeof mem1, 0);
	CHECK(db != NULL, "opens after damage");
	if (db) {
		kt_stat(db, &st);
		CHECK(st.rollback == 1, "rollback reported");
		CHECK(same_as(db, 0, &m), "the commit before");
		kt_close(db);
	}
	m_free(&m);
	mf_free(f);
}

/* ---- fuzz (ACR.9) ---- */
static int walk_all(KT *db)
{
	struct kt_cur c;
	unsigned char kk[KT_MAXKEY], vv[KT_MAXVAL];
	size_t kl, vl;
	long n = 0;
	int t, i;

	for (i = 0; i < 2; i++) {
		t = i ? kt_tree(db, "bigarrena", 0) : 0;
		if (t < 0 || kt_seek(db, t, &c, "", 0) != 0) continue;
		for (;;) {
			kl = sizeof kk; vl = sizeof vv;
			if (kt_next(&c, kk, &kl, vv, &vl) != 0) break;
			if (++n > 1000000) return -1;		/* a loop */
		}
	}
	kt_check(db, NULL, NULL);
	return 0;
}

static void test_fuzz(void)
{
	struct mfile *f = mf_new(), *g = mf_new();
	struct kt_io io;
	KT *db = open_m(f, &io, mem1, sizeof mem1, 0);
	char k[16];
	static unsigned char v[500];
	int i, j, loops = 0;

	if (!db) { CHECK(0, "open"); mf_free(f); mf_free(g); return; }
	kt_tree(db, "bigarrena", KT_CREAT);
	for (i = 0; i < 3000; i++) {
		snprintf(k, sizeof k, "z%05d", (int) (rnd() % 5000));
		memset(v, i, sizeof v);
		kt_put(db, 0, k, strlen(k), v, (size_t) (rnd() % 500));
		if (i % 100 == 99) kt_commit(db);
	}
	kt_commit(db);
	kt_close(db);
	for (i = 0; i < 3000; i++) {
		mf_copy(g, f);
		if (i % 10 == 0) g->csize = rnd() % (f->csize + 1);
		else if (i % 10 == 1)
			for (j = 0; j < (int) g->csize; j++) g->cur[j] = (unsigned char) rnd();
		else
			for (j = 0; j < 1 + i % 16; j++)
				g->cur[rnd() % g->csize] ^= (unsigned char) (1 + rnd() % 255);
		io = mf_io(g);
		db = kt_open_io(&io, mem2, sizeof mem2, KT_RDONLY | KT_NOLOCK);
		if (db) { if (walk_all(db) != 0) loops++; kt_close(db); }
	}
	CHECK(loops == 0, "fuzz: %d endless walks", loops);
	mf_free(f); mf_free(g);
}

/* ---- one writer (real file) ---- */
static void test_lock(void)
{
	char path[] = "/tmp/kutxatest.XXXXXX";
	int fd = mkstemp(path);
	KT *a, *b;
	pid_t p;
	int st;

	if (fd < 0) { CHECK(0, "mkstemp"); return; }
	close(fd);
	unlink(path);
	a = kt_open(path, mem1, sizeof mem1, KT_CREAT);
	CHECK(a != NULL, "writer opens");
	if (a) { kt_put(a, 0, "a", 1, "b", 1); kt_commit(a); }
	p = fork();
	if (p == 0) {
		/* fcntl locks are per process: try from another one */
		b = kt_open(path, mem2, sizeof mem2, 0);
		_exit(b == NULL && errno == EBUSY ? 0 : 1);
	}
	waitpid(p, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "second writer gets EBUSY");
	b = kt_open(path, mem2, sizeof mem2, KT_RDONLY);
	CHECK(b != NULL, "a reader opens beside the writer");
	if (b) kt_close(b);
	if (a) kt_close(a);
	unlink(path);
}

int main(void)
{
	test_basic();
	test_calendar();
	test_reminders();
	test_logs();
	test_holidays();
	test_irdb();
	test_schemaless();
	test_snapshot();
	test_powercut();
	test_rollback();
	test_fuzz();
	test_lock();
	if (fails) { printf("kutxatest: %d FAILED\n", fails); return 1; }
	printf("kutxatest: all passed\n");
	return 0;
}
