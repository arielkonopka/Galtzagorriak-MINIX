/* libblk host tests (spec 12h, R12.38): src/lib/blk/libblk.c against a
 * fake block space.  The fake blk_* below keep the blocks in an array,
 * zeroed at blk_open (as MM's blocks read after a new reservation), and
 * count the sectors read and written, so the tests see exactly what the
 * library sends to MM.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <libblk.h>

#define NBLK	64

static unsigned char card[NBLK][BLK_SIZE];
static unsigned long nblk;		/* the reservation, blocks; 0: none */
static int open_ok = 1;
static long rd_sect, wr_sect, wr_ops;
static int fail_wr;			/* next write fails with this errno */
static int fails, checks;

#define CHECK(c, what) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

int blk_open(unsigned long bytes, int flags)
{
	unsigned long n = (bytes + BLK_SIZE - 1) / BLK_SIZE;

	if (!open_ok) { errno = ENOSYS; return -1; }
	if (n > NBLK) {
		if (!(flags & BLK_ANY)) { errno = ENOSPC; return -1; }
		n = NBLK;
	}
	memset(card, 0, sizeof(card));
	nblk = n;
	return (int) n;
}

int blk_close(void)
{
	nblk = 0;
	return 0;
}

int blk_info(struct blkinfo *bi)
{
	memset(bi, 0, sizeof(*bi));
	bi->blocks = nblk;
	return 0;
}

static int range(unsigned long id, unsigned off, unsigned len)
{
	if (nblk == 0) { errno = EBADF; return -1; }
	if (id >= nblk) { errno = ERANGE; return -1; }
	if (len == 0 || (off | len) % BLK_SECT || off + len > BLK_SIZE) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int blk_read(unsigned long id, unsigned off, void *buf, unsigned len)
{
	if (range(id, off, len) < 0) return -1;
	memcpy(buf, &card[id][off], len);
	rd_sect += len / BLK_SECT;
	return 0;
}

int blk_write(unsigned long id, unsigned off, const void *buf, unsigned len)
{
	if (range(id, off, len) < 0) return -1;
	if (fail_wr) { errno = fail_wr; return -1; }
	memcpy(&card[id][off], buf, len);
	wr_sect += len / BLK_SECT;
	wr_ops++;
	return 0;
}

int blk_get(unsigned long id, void *buf)
{
	return blk_read(id, 0, buf, BLK_SIZE);
}

int blk_put(unsigned long id, const void *buf)
{
	return blk_write(id, 0, buf, BLK_SIZE);
}

int blk_putf(unsigned long id, const void *buf, int flags)
{
	(void) flags;
	return blk_put(id, buf);
}

static unsigned long pool[(3 * (4096 + 8)) / sizeof(unsigned long) + 1];

static void counters(void)
{
	rd_sect = wr_sect = wr_ops = 0;
}

/* Frames of fsize over 256 KB: every byte written through vm_get reads
 * back, across many evictions, and only dirty frames are written. */
static void pattern(unsigned fsize)
{
	struct vm v;
	unsigned long off, size = 256UL * 1024;
	unsigned char *p;
	char what[64];
	int ok = 1;

	sprintf(what, "%u: vm_open", fsize);
	CHECK(vm_open(&v, pool, sizeof(pool), size, fsize, 0) == 0, what);
	CHECK(v.size == size, "size reserved");
	CHECK(v.nframes == sizeof(pool) / (fsize + 8), "frames from the pool");
	CHECK((char *) v.f >= (char *) pool && (char *) (v.f + v.nframes)
		<= (char *) pool + sizeof(pool), "metadata inside the pool");
	for (off = 0; off < size; off += 4) {
		p = vm_get(&v, off, VM_RW);
		if (p == NULL) { ok = 0; break; }
		p[0] = (unsigned char) (off >> 2);
		p[1] = (unsigned char) (off >> 10);
		p[2] = (unsigned char) (off >> 18);
		p[3] = 0xA5;
	}
	sprintf(what, "%u: writes through vm_get", fsize);
	CHECK(ok, what);
	CHECK(vm_flush(&v) == 0, "vm_flush");
	counters();
	CHECK(vm_flush(&v) == 0 && wr_ops == 0, "second vm_flush writes nothing");
	for (off = 0; off < size && ok; off += 4) {
		p = vm_get(&v, off, VM_RD);
		if (p == NULL || p[0] != (unsigned char) (off >> 2)
			|| p[1] != (unsigned char) (off >> 10)
			|| p[2] != (unsigned char) (off >> 18) || p[3] != 0xA5)
			ok = 0;
	}
	sprintf(what, "%u: reads back", fsize);
	CHECK(ok, what);
	CHECK(wr_ops == 0, "reading writes nothing");
	CHECK(rd_sect == (long) (size / BLK_SECT), "each frame read once");
	CHECK(vm_get(&v, size, VM_RD) == NULL && errno == ERANGE,
		"one past the end: ERANGE");
	CHECK(vm_close(&v) == 0 && nblk == 0, "vm_close ends the reservation");
}

static void lru(void)
{
	struct vm v;
	char *a, *b;

	/* 1 KB frames: 3 of them in a pool for 3. */
	CHECK(vm_open(&v, pool, 3 * (1024 + 8), 64 * 1024, 1024, 0) == 0,
		"lru: open");
	CHECK(v.nframes == 3, "lru: 3 frames");
	a = vm_get(&v, 0, VM_RW);
	*a = 1;
	vm_get(&v, 1024, VM_RD);
	vm_get(&v, 2048, VM_RD);
	vm_get(&v, 0, VM_RD);		/* unit 0 is now the newest */
	counters();
	vm_get(&v, 3072, VM_RD);	/* evicts unit 1 (clean): no write */
	CHECK(wr_ops == 0, "lru: the least recently used clean frame goes");
	vm_get(&v, 4096, VM_RD);	/* evicts unit 2 */
	vm_get(&v, 5120, VM_RD);	/* evicts unit 0: dirty, written */
	CHECK(wr_ops == 1 && wr_sect == 2 && card[0][0] == 1,
		"lru: a dirty frame is written (2 sectors) when it goes");

	/* Pins: a pinned frame stays, and with all pinned, EBUSY. */
	a = vm_get(&v, 8192, VM_RW | VM_PIN);
	*a = 7;
	vm_get(&v, 9216, VM_RD | VM_PIN);
	vm_get(&v, 10240, VM_RD | VM_PIN);
	CHECK(vm_get(&v, 11264, VM_RD) == NULL && errno == EBUSY,
		"pin: all frames pinned gives EBUSY");
	b = vm_get(&v, 8192 + 5, VM_RD);
	CHECK(b == a + 5 && *a == 7, "pin: the pinned pointer stays valid");
	vm_unpin(&v, 9216);
	CHECK(vm_get(&v, 11264, VM_RD) != NULL, "unpin frees a frame");
	CHECK(vm_get(&v, 8192, VM_RD) == a, "pin: unit 8 is still there");

	/* A failing write keeps the frame dirty; vm_flush retries. */
	vm_unpin(&v, 8192);
	vm_unpin(&v, 10240);
	fail_wr = ENXIO;
	CHECK(vm_flush(&v) < 0 && errno == ENXIO && v.err == ENXIO,
		"ENXIO: vm_flush fails");
	fail_wr = 0;
	counters();
	CHECK(vm_flush(&v) == 0 && wr_ops == 1 && card[2][0] == 7,
		"ENXIO: the frame stayed dirty and is written later");

	/* vm_drop: no write; only frames wholly inside the range. */
	a = vm_get(&v, 12288, VM_RW);
	*a = 9;
	b = vm_get(&v, 13312, VM_RW);
	*b = 9;
	vm_drop(&v, 12288 + 512, 1024);	/* covers neither frame wholly */
	counters();
	vm_drop(&v, 12288, 1024);	/* frame of 12288 only */
	CHECK(vm_flush(&v) == 0 && wr_ops == 1 && card[3][0] == 0
		&& card[3][1024] == 9, "vm_drop forgets without writing");
	vm_close(&v);
}

struct rec { unsigned short id; char name[30]; };	/* 32 B */
struct odd { char b[100]; };				/* 100 B */

static void records(void)
{
	struct vm v;
	struct rec *r;
	struct odd *o;
	int i, ok = 1;

	CHECK(vm_open(&v, pool, sizeof(pool), 256 * 1024UL, 2048, 0) == 0,
		"records: open");
	if (v.fsize == 0) return;
	for (i = 0; i < 2000; i++) {
		r = VA_SET(&v, struct rec, i);
		if (r == NULL) { ok = 0; break; }
		r->id = (unsigned short) i;
		sprintf(r->name, "rec%d", i);
	}
	for (i = 0; i < 2000 && ok; i++) {
		r = VA_GET(&v, struct rec, i);
		if (r == NULL || r->id != i) ok = 0;
	}
	CHECK(ok, "VA_SET/VA_GET of 2000 records");
	/* 100 B records: 20 per 2 KB frame, none crosses one. */
	for (i = 0; i < 500; i++) {
		unsigned long off = VA_OFF(&v, sizeof(struct odd), i);
		if (off / 2048 != (off + sizeof(struct odd) - 1) / 2048) ok = 0;
		o = VA_SET(&v, struct odd, i);
		if (o == NULL) { ok = 0; break; }
		memset(o->b, i & 0xFF, sizeof(o->b));
	}
	for (i = 0; i < 500 && ok; i++) {
		o = VA_GET(&v, struct odd, i);
		if (o == NULL || (unsigned char) o->b[99] != (i & 0xFF)) ok = 0;
	}
	CHECK(ok, "100 B records never cross a frame");
	vm_close(&v);
}

static void errors(void)
{
	struct vm v;

	CHECK(vm_open(&v, pool, sizeof(pool), 4096, 512, 0) < 0
		&& errno == EINVAL, "framesize 512: EINVAL");
	CHECK(vm_open(&v, pool, 1024, 4096, 1024, 0) < 0 && errno == EINVAL,
		"pool smaller than a frame and its 8 B: EINVAL");
	CHECK(vm_open(&v, (char *) pool + 1, sizeof(pool) - 4, 4096, 1024, 0)
		< 0 && errno == EINVAL, "unaligned pool: EINVAL");
	CHECK(vm_open(&v, pool, sizeof(pool), (NBLK + 1) * 4096UL, 1024, 0) < 0
		&& errno == ENOSPC, "too large: ENOSPC");
	CHECK(vm_open(&v, pool, sizeof(pool), (NBLK + 1) * 4096UL, 1024,
		BLK_ANY) == 0 && v.size == NBLK * 4096UL, "BLK_ANY: what is free");
	vm_close(&v);
	open_ok = 0;
	CHECK(vm_open(&v, pool, sizeof(pool), 4096, 1024, 0) < 0
		&& errno == ENOSYS, "blocks off: ENOSYS");
	open_ok = 1;
}

int main(void)
{
	CHECK(sizeof(struct vm_frame) == 8, "8 B per frame");
	pattern(1024);
	pattern(2048);
	pattern(4096);
	lru();
	records();
	errors();
	printf("libblk: %d checks, %d failures\n", checks, fails);
	return fails != 0;
}
