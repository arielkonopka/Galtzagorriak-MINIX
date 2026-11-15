/* fccheck - every live record of the flash text cache names a text that
 * is really there: CRC-32 of [addr, addr + size) equals the record's key
 * (src/mm/fcache.c; the flash is readable by every process).  A wrong
 * one would run some other program's code under this program's name.
 */
#include <lib.h>
#include <minix/callnr.h>
#include "t.h"

#define FC_MAGIC	0x32585447UL
#define PAGE		4096UL
#define HDR		8
#define NREC		((PAGE - HDR) / 32)

struct fcrec { unsigned long crc, size, addr, seq, owner, flags, dead0, dead1; };

static unsigned long crc32(const unsigned char *p, unsigned long n)
{
	unsigned long c = 0xFFFFFFFFUL;
	int k;

	while (n-- > 0) {
		c ^= *p++;
		for (k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320UL : c >> 1;
	}
	return ~c;
}

int main(void)
{
	message m;
	struct fcinfo fi;
	unsigned long *h0, *h1, base;
	struct fcrec *r;
	int d, i, live = 0, bad = 0;
	char how[80];

	m.m1_p1 = (char *) &fi;
	m.m1_i2 = MI_FCACHE;
	if (_syscall(MM, MEMINFO, &m) != 0 || fi.size == 0) {
		printf("fccheck: no flash text cache\n");
		return 0;
	}
	base = fi.cache_base;
	h0 = (unsigned long *) base;
	h1 = (unsigned long *) (base + PAGE);
	if (h0[0] != FC_MAGIC && h1[0] != FC_MAGIC) {
		printf("fccheck: no cache directory yet\n");
		return 0;
	}
	d = h1[0] == FC_MAGIC && (h0[0] != FC_MAGIC || h1[1] > h0[1]);
	for (i = 0; i < NREC; i++) {
		r = (struct fcrec *) (base + d * PAGE + HDR + i * 32);
		if (r->crc == 0xFFFFFFFFUL && r->size == 0xFFFFFFFFUL) break;
		if (r->dead0 != 0xFFFFFFFFUL) continue;
		live++;
		if (crc32((const unsigned char *) r->addr, r->size) != r->crc) {
			bad++;
			sprintf(how, "record %d: %lu bytes at %08lx, key %08lx",
						i, r->size, r->addr, r->crc);
			t_result("cache text matches its key", 0, how);
		}
	}
	sprintf(how, "%d live records, %d bad", live, bad);
	if (bad == 0) t_result("cache texts match their keys", 1, how);
	return t_fail;
}
