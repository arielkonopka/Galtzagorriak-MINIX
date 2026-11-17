/* flash - self test module (spec R7b.3.1, flash part): the 1 MB of
 * internal flash as a 16 x 16 grid of 4 KB pages.  Every program installed
 * in flash (Phase 2b) has a record with the SHA-256 of its text; the text
 * is hashed again here and compared, its pages drawn as passed or failed,
 * and a failed one is named (AC7b.3.2).  The system image, the run-time
 * cache and CPU2's area are shown, not tested (the system image has no
 * stored checksum yet).  Automatic.  Programs may read the flash (the MPU
 * makes it read-only for everyone), so no kernel or MM help is needed
 * beyond MEMINFO's region addresses.
 *
 * Cells: filled pass, cross fail, frame free, dots cache, hatched system
 * or CPU2, two dots a directory page.
 */
#include <lib.h>
#include <minix/type.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "gui.h"
#include "st.h"

#define PAGE	4096UL
#define NPAGE	256
#define IN_MAGIC 0x534E4947UL		/* "GINS" (mm/finst.c) */
#define IN_SEED	0x424E4947UL		/* "GINB" */
#define HDR	8
#define REC	64
#define NREC	((PAGE - HDR) / REC)

struct inrec {				/* as mm/finst.c */
	unsigned char sha[32];
	unsigned long addr, size, id, ctime, seq, flags, dead0, dead1;	/* timecheck: ok (MM's ftime32_t) */
};

enum { C_NONE, C_PASS, C_FAIL, C_FREE, C_CACHE, C_SYS, C_DIR };
static unsigned char st[NPAGE];
static unsigned long FLASH;	/* 0x08000000 on the Flipper; QEMU's differs */
static int gui;

/* SHA-256 (FIPS 180-4), small rather than fast. */
static const unsigned long K[64] = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
	0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
	0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
	0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
	0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
	0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
	0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
	0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
	0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n)	((x) >> (n) | (x) << (32 - (n)))

static void block(unsigned long *h, const unsigned char *p)
{
	unsigned long w[64], a, b, c, d, e, f, g, k, t1, t2;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = (unsigned long) p[4*i] << 24 | (unsigned long) p[4*i+1] << 16
			| (unsigned long) p[4*i+2] << 8 | p[4*i+3];
	for (; i < 64; i++)
		w[i] = (ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ w[i-2] >> 10)
			+ w[i-7] + (ROR(w[i-15], 7) ^ ROR(w[i-15], 18)
			^ w[i-15] >> 3) + w[i-16];
	a = h[0]; b = h[1]; c = h[2]; d = h[3];
	e = h[4]; f = h[5]; g = h[6]; k = h[7];
	for (i = 0; i < 64; i++) {
		t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25))
			+ ((e & f) ^ (~e & g)) + K[i] + w[i];
		t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22))
			+ ((a & b) ^ (a & c) ^ (b & c));
		k = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d;
	h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

static void sha256(const unsigned char *p, unsigned long n, unsigned char *out)
{
	static const unsigned long h0[8] = { 0x6a09e667, 0xbb67ae85,
		0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
		0x5be0cd19 };
	unsigned long h[8], left = n;
	unsigned char t[128];
	int i, r;

	memcpy(h, h0, sizeof h);
	for (; left >= 64; left -= 64, p += 64) block(h, p);
	r = (int) left;
	memset(t, 0, sizeof t);
	memcpy(t, p, r);
	t[r] = 0x80;
	i = r < 56 ? 64 : 128;
	t[i-4] = (unsigned char) (n >> 21);	/* bits = n * 8 (< 2^32) */
	t[i-3] = (unsigned char) (n >> 13);
	t[i-2] = (unsigned char) (n >> 5);
	t[i-1] = (unsigned char) (n << 3);
	block(h, t);
	if (i == 128) block(h, t + 64);
	for (i = 0; i < 8; i++) {
		out[4*i] = h[i] >> 24; out[4*i+1] = h[i] >> 16;
		out[4*i+2] = h[i] >> 8; out[4*i+3] = h[i];
	}
}

static void cell(int p)
{
	int x = 2 + (p % 16) * 8, y = 8 + (p / 16) * 3 + (p / 16 >= 8);

	if (!gui) return;
	switch (st[p]) {
	case C_PASS: gui_fill(x, y, x + 6, y + 1, 1); break;
	case C_FAIL: gui_fill(x, y, x + 6, y + 1, 1);
		gui_pixel(x + 2, y, 0); gui_pixel(x + 4, y + 1, 0); break;
	case C_CACHE: gui_pixel(x + 1, y, 1); gui_pixel(x + 5, y + 1, 1); break;
	case C_SYS: gui_hline(x, x + 6, y, 1); break;
	case C_DIR: gui_pixel(x + 1, y, 1); gui_pixel(x + 5, y, 1);
		gui_hline(x, x + 6, y + 1, 1); break;
	default: gui_pixel(x + 3, y + 1, 1);	/* free or unknown */
	}
}

/* The file whose text a record holds (dev, ino), in the usual places. */
static int name_of(unsigned long id, char *out, int len)
{
	static const char *dirs[] = { "/bin", "/usr/bin", "/lib", "/usr/lib",
		"/usr/lib/selftest", "/usr/tests", "/tmp", NULL };
	static char path[64];
	struct dirent *e;
	struct stat sb;
	DIR *d;
	int i;

	for (i = 0; dirs[i] != NULL; i++) {
		if ((d = opendir(dirs[i])) == NULL) continue;
		while ((e = readdir(d)) != NULL) {
			if (strlen(dirs[i]) + strlen(e->d_name) + 2 > sizeof path)
				continue;
			sprintf(path, "%s/%s", dirs[i], e->d_name);
			if (stat(path, &sb) == 0 && ((unsigned long) sb.st_ino
				<< 16 | ((unsigned long) sb.st_dev & 0xFFFF)) == id) {
				closedir(d);
				strncpy(out, path, len - 1);
				out[len - 1] = 0;
				return 0;
			}
		}
		closedir(d);
	}
	return -1;
}

int main(int argc, char **argv)
{
	struct fcinfo fi;
	struct inrec *r;
	unsigned long dir, g0 = 0, g1 = 0, *h0, *h1, a, p;
	unsigned char sha[32];
	char why[48], first[40], line[40];
	int autom = argc > 1 && strcmp(argv[1], "-a") == 0;
	int i, ok = 0, bad = 0;
	message m;

	gui = gui_open() == 0;
	m.m1_i2 = MI_FCACHE;
	m.m1_p1 = (char *) &fi;
	if (_syscall(MM, MEMINFO, &m) < 0 || fi.inst_base == 0) {
		if (gui) gui_close();
		st_result("SKIP", "no installed region");
		return 0;
	}
	FLASH = fi.inst_base & ~0xFFFFFUL;	/* the 1 MB it lies in */
	for (p = 0; p < NPAGE; p++) {
		a = FLASH + p * PAGE;
		st[p] = a < fi.inst_base ? C_SYS
			: a < fi.inst_base + fi.inst_size ? C_FREE
			: a < fi.sfsa ? C_CACHE : C_SYS;
	}
	h0 = (unsigned long *) fi.inst_base;
	h1 = (unsigned long *) (fi.inst_base + PAGE);
	if (h0[0] == IN_MAGIC || h0[0] == IN_SEED) g0 = h0[1] + 1;
	if (h1[0] == IN_MAGIC || h1[0] == IN_SEED) g1 = h1[1] + 1;
	dir = g1 > g0 ? fi.inst_base + PAGE : fi.inst_base;
	st[(fi.inst_base - FLASH) / PAGE] = C_DIR;
	st[(fi.inst_base - FLASH) / PAGE + 1] = C_DIR;
	if (gui) {
		gui_clear();
		gui_title("Memory: flash");
		for (p = 0; p < NPAGE; p++) cell((int) p);
		gui_flush();
	}
	first[0] = 0;
	if (g0 != 0 || g1 != 0) {
		for (i = 0; i < (int) NREC; i++) {
			r = (struct inrec *) (dir + HDR + i * REC);
			if (r->addr == 0xFFFFFFFFUL && r->seq == 0xFFFFFFFFUL
			    && r->id == 0xFFFFFFFFUL) break;	/* free slot */
			if (r->addr == 0xFFFFFFFFUL || r->seq == 0xFFFFFFFFUL
			    || r->dead0 != 0xFFFFFFFFUL) continue;
			if (r->addr < fi.inst_base || r->size == 0
			    || r->addr + r->size > fi.inst_base + fi.inst_size)
				continue;
			sha256((const unsigned char *) r->addr, r->size, sha);
			if (memcmp(sha, r->sha, 32) == 0) ok++;
			else {
				bad++;
				if (first[0] == 0 && name_of(r->id, first,
							sizeof first) < 0)
					sprintf(first, "at %08lx", r->addr);
			}
			for (a = r->addr & ~(PAGE - 1); a < r->addr + r->size;
								a += PAGE) {
				p = (a - FLASH) / PAGE;
				if (p >= NPAGE) break;
				st[p] = memcmp(sha, r->sha, 32) == 0 ? C_PASS
								: C_FAIL;
				cell((int) p);
			}
			if (gui) gui_flush();
		}
	}
	sprintf(line, "%d programs ok, %d damaged", ok, bad);
	fprintf(stderr, "flash: %s\n", line);
	if (gui) {
		gui_text(2, GUI_H - 6, line, 0);
		gui_flush();
		if (!autom) (void) gui_key();
		gui_close();
	}
	if (bad) {
		sprintf(why, "%d damaged: %s", bad, first);
		st_result("FAIL", why);
		return 1;
	}
	sprintf(why, "%d programs", ok);
	st_result("PASS", why);
	return 0;
}
