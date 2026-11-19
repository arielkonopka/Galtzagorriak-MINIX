/* parttest.c - PC test of the partition table parser (src/kernel/drvlib.c)
 * against hostile SD cards (security review 2026-11-16, K2).
 *
 * The kernel file is compiled unchanged with host stubs (stubs/); the
 * "card" is an image in memory.  Checked for every card:
 *   - partition() ends (a chain of extended tables may point back to
 *     itself: the disk task must not read forever);
 *   - every device lies inside the card, every subpartition and logical
 *     partition inside its primary partition, with no 32-bit wrap-around.
 * Fixed attack cards first, then random ones (deterministic seeds).
 * Exit status: the number of failures.  Build and run: make -C tests/host
 */
#include <setjmp.h>
#include <stdlib.h>
#include "kernel.h"
#include "driver.h"
#include "drvlib.h"

#define NSECT		65536u		/* a 32 MB card */
#define MAX_READS	1000		/* far more than any real card needs */
#define NR_DEV		(1 + NR_PARTITIONS)
#define NR_SUB		(NR_PARTITIONS * NR_PARTITIONS)

static uint8_t *card;
static unsigned reads;
static jmp_buf stuck;
static struct device part[NR_DEV], sub[NR_SUB];
static int fails;

int hal_disk_read_part(uint32_t sector, unsigned off, unsigned len, void *buf)
{
	if (++reads > MAX_READS) longjmp(stuck, 1);
	if (sector >= NSECT || off + len > SECTOR_SIZE) return -1;
	memcpy(buf, card + (size_t) sector * SECTOR_SIZE + off, len);
	return 0;
}

int t_quiet(const char *fmt, ...) { (void) fmt; return 0; }

static char *t_name(void) { return "sd"; }

static struct device *t_prepare(int device)	/* as arm/disk.c */
{
	if (device >= 0 && device < NR_DEV) return &part[device];
	if ((unsigned) (device - MINOR_hd1a) < NR_SUB)
		return &sub[device - MINOR_hd1a];
	return NIL_DEV;
}

static struct driver drv = { t_name, t_prepare };

static void put_entry(uint32_t sector, int i, int sysind, uint32_t lowsec,
							uint32_t size)
{
	uint8_t *s = card + (size_t) sector * SECTOR_SIZE;
	uint8_t *e = s + PART_TABLE_OFF + 16 * i;

	e[4] = sysind;
	memcpy(e + 8, &lowsec, 4);		/* little endian, as on the card */
	memcpy(e + 12, &size, 4);
	s[510] = 0x55;
	s[511] = 0xAA;
}

/* Inside [lo, hi) sectors, with the counts not wrapped (R12.39: the
 * driver's dv_base and dv_size are in sectors, DV_SECS). */
static int inside(struct device *dv, uint64_t lo, uint64_t hi)
{
	uint64_t b = dv->dv_base, n = dv->dv_size;

	if (n == 0) return 1;			/* empty: never read */
	return b >= lo && b + n <= hi;
}

static void check(const char *what)
{
	int p, s, ok = 1;

	memset(part, 0, sizeof part);
	memset(sub, 0, sizeof sub);
	part[0].dv_base = 0;
	part[0].dv_size = NSECT;
	reads = 0;
	if (setjmp(stuck)) {
		printf("FAIL %s: partition() still reading after %d sectors\n",
			what, MAX_READS);
		fails++;
		return;
	}
	partition(&drv, 0, P_PRIMARY);
	for (p = 1; p < NR_DEV; p++) {
		if (!inside(&part[p], 0, NSECT)) ok = 0;
		for (s = 0; s < NR_PARTITIONS; s++) {
			struct device *d = &sub[(p - 1) * NR_PARTITIONS + s];
			uint64_t lo = part[p].dv_base;

			if (!inside(d, lo, lo + part[p].dv_size))
				ok = 0;
		}
	}
	if (!ok) {
		printf("FAIL %s: a device outside its container:\n", what);
		for (p = 1; p < NR_DEV; p++)
			printf("  hd%d %08x+%08x\n", p,
				(unsigned) part[p].dv_base,
				(unsigned) part[p].dv_size);
		for (s = 0; s < NR_SUB; s++)
			if (sub[s].dv_size)
				printf("  hd%d%c %08x+%08x\n", s / 4 + 1,
					'a' + s % 4, (unsigned) sub[s].dv_base,
					(unsigned) sub[s].dv_size);
		fails++;
		return;
	}
	if (what[0] != '#') printf("PASS %s (%u reads)\n", what, reads);
}

static void blank(void)
{
	memset(card, 0, (size_t) NSECT * SECTOR_SIZE);
}

static uint32_t rnd(void)
{
	return (uint32_t) rand() << 16 ^ (uint32_t) rand();
}

static uint32_t rnd_sector(void)	/* mostly on the card, some anywhere */
{
	switch (rand() % 4) {
	case 0:  return rnd();
	case 1:  return 0xFFFFFFFFu - rand() % 64;
	default: return rnd() % NSECT;
	}
}

static int rnd_type(void)
{
	static const int t[] = { NO_PART, MINIX_PART, EXT_PART, EXT_PART, 0x0C };

	return t[rand() % 5];
}

/* dv_sector (R12.39, review of #348): the sector of a 64-bit request
 * position (HIGHPOS:POSITION) in a partition, or its size (EOF).  Bits
 * 32-40 of the position are sector bits 23-31; HIGHPOS 512 and up is
 * past 2 TiB and must be EOF, not wrap to a sector near the start. */
static void sectors(void)
{
	static const struct {
		unsigned high;
		uint32_t pos, size, want;	/* want == size: EOF */
	} t[] = {
		{ 0, 0, 100, 0 },
		{ 0, 99u * 512, 100, 99 },
		{ 0, 100u * 512, 100, 100 },		/* at the end */
		{ 1, 0, 0x01000000u, 0x00800000u },	/* 4 GiB */
		{ 1, 1024, 0x01000000u, 0x00800002u },
		{ 0x1FF, 0xFFFFFE00u, 0xFFFFFFFFu, 0xFFFFFFFFu }, /* end */
		{ 0x1FF, 0xFFFFFC00u, 0xFFFFFFFFu, 0xFFFFFFFEu },
		{ 512, 0, 0xFFFFFFFFu, 0xFFFFFFFFu },	/* 2 TiB: EOF */
		{ 512, 0, 0x80000000u, 0x80000000u },	/* was sector 0 */
		{ 513, 1024, 0xFFFFFFFFu, 0xFFFFFFFFu },/* was sector 0x800002 */
		{ 1023, 0xFFFFFC00u, 0xFFFFFFFFu, 0xFFFFFFFFu }, /* FS's max */
		{ 0xFFFF, 0, 0xFFFFFFFFu, 0xFFFFFFFFu },
	};
	struct device dv;
	unsigned i;
	uint32_t got;

	for (i = 0; i < sizeof t / sizeof t[0]; i++) {
		dv.dv_base = 1000;
		dv.dv_size = t[i].size;
		got = dv_sector(&dv, t[i].high, t[i].pos);
		if (got != t[i].want) {
			printf("FAIL dv_sector(%#x:%#x, size %#x) = %#x, "
				"not %#x\n", t[i].high, t[i].pos, t[i].size,
				got, t[i].want);
			fails++;
		}
	}
	printf("%s dv_sector: 64-bit positions, past 2 TiB is EOF\n",
		fails ? "done:" : "PASS");
}

int main(void)
{
	int i, j, k;
	char name[32];

	sectors();

	if ((card = malloc((size_t) NSECT * SECTOR_SIZE)) == NULL) return 1;

	/* An extended table whose "next" is itself. */
	blank();
	put_entry(0, 0, EXT_PART, 100, 1000);
	put_entry(100, 0, 0x0C, 1, 10);
	put_entry(100, 1, EXT_PART, 50, 100);
	put_entry(150, 1, EXT_PART, 50, 100);
	check("extended chain pointing back to itself");

	/* Two tables pointing at each other, no logical partitions. */
	blank();
	put_entry(0, 0, EXT_PART, 100, 1000);
	put_entry(100, 0, EXT_PART, 20, 10);
	put_entry(120, 0, EXT_PART, 30, 10);
	put_entry(130, 0, EXT_PART, 20, 10);
	check("extended chain cycle without logical partitions");

	/* A logical partition far outside the extended one, and wrapping. */
	blank();
	put_entry(0, 0, EXT_PART, 100, 1000);
	put_entry(100, 0, 0x0C, 0xFFFFFF00u, 0x1000);
	put_entry(100, 1, EXT_PART, 10, 10);
	put_entry(110, 0, 0x0C, 5, 0xFFFFFFF0u);
	check("logical partitions outside the extended one");

	/* Primary partitions that wrap around 32 bits. */
	blank();
	put_entry(0, 0, MINIX_PART, 0xFFFFFFF0u, 0x20);
	put_entry(0, 1, MINIX_PART, 10, 0xFFFFFFFFu);
	put_entry(0, 2, EXT_PART, 0xFFFFFE00u, 0x400);
	check("primary partitions wrapping around");

	/* Subpartitions of a MINIX partition outside it. */
	blank();
	put_entry(0, 0, MINIX_PART, 1000, 100);
	put_entry(1000, 0, MINIX_PART, 0, 0xFFFFFFFFu);
	put_entry(1000, 1, MINIX_PART, 1050, 1000);
	put_entry(1000, 2, MINIX_PART, 0xFFFFFFFFu, 2);
	check("subpartitions outside their partition");

	/* Random cards: an MBR, and tables at the sectors it points to. */
	for (i = 0; i < 20000; i++) {
		srand(i + 1);
		blank();
		for (j = 0; j < 4; j++)
			put_entry(0, j, rnd_type(), rnd_sector(), rnd_sector());
		for (k = 0; k < 6; k++) {	/* tables where entries point */
			uint32_t at = rnd() % 2048;

			for (j = 0; j < 4; j++)
				put_entry(at, j, rnd_type(), rand() % 2048,
					rand() % 2 ? rand() % 4096 : rnd());
		}
		snprintf(name, sizeof name, "#random card %d", i + 1);
		check(name);
	}
	printf("%s random cards (20000)\n", fails ? "done:" : "PASS");
	printf("%d failure(s)\n", fails);
	return fails;
}
