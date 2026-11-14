/* Installed programs in flash (Galtzagorriak MINIX, spec Phase 2b,
 * R2b.2-R2b.5, R2b.7, R2b.8).
 *
 * The text of a program installed by root (pkg install --flash) is written
 * into internal flash once, at install time, and every exec of that file
 * runs it in place from there - no RAM for text, no run-time cache write.
 *
 * Region (hal_flash_layout(): from 0x08030000 up to the run-time cache):
 *   page 0, 1	directories, ping-pong like fcache.c: header (8 bytes)
 *		magic, generation; then 63 records of 64 bytes, each written
 *		once (8 double words):
 *		  sha[32]	SHA-256 of the text as written and read back
 *		  addr, size	where the text is; all ones: free slot
 *		  ino|dev, ctime  the program file it was installed for
 *		  seq, flags	insertion counter; flags unused
 *		  dead		all ones: live; programmed 0: removed
 *   page 2..	text, whole pages per program.
 *
 * exec finds a program by the binding (dev, ino, ctime) of its file and
 * then trusts the text without checking it: only root could have put it
 * there, after checking the file (owned by root or bin, writable only by
 * its owner) and the copy (SHA-256 read back).  ctime changes with every
 * write or chmod of the file, so a changed file never matches (R2b.4).
 *
 * Install order, for power cuts (AC2b.5): text pages, then the record, then
 * the old version's record is marked dead.  A cut leaves pages no record
 * claims (reused later) or both records live (the newer seq wins).
 *
 * RAM: a few words of state; the page use is worked out from the
 * directory when needed, no bitmap.  Writes: only on install, remove, and
 * a directory compaction when it is full - never by exec.
 */
#include "mm.h"
#include <sys/stat.h>
#include <minix/callnr.h>
#include <minix/com.h>
#include <a.out.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include "mproc.h"

#if (CHIP == ARM_CM)

extern char mbuf[];			/* exec.c's buffer, free here */

#define IN_MAGIC	0x534E4947UL	/* "GINS" */
#define PAGE		4096UL
#define HDR		8
#define REC		64
#define NREC		((PAGE - HDR) / REC)
#define DIRPAGES	2
#define CHUNK		256		/* programmed per kernel call */
#define BIN_UID		2

struct inrec {
  unsigned char sha[32];
  unsigned long addr, size;
  unsigned long id;		/* ino << 16 | dev */
  unsigned long ctime;
  unsigned long seq, flags;
  unsigned long dead0, dead1;
};

/* The only RAM: region base | active directory page (0, 1; 2: none yet),
 * 0: no region.  The size and the next seq are looked up when installing. */
PRIVATE unsigned long in_st;

#define in_base		(in_st & ~3UL)
#define in_dir		((int) (in_st & 3))
#define SET_DIR(d)	(in_st = in_base | (d))
#define DIRADDR(d)	(in_base + (d) * PAGE)
#define RECP(d, i)	((struct inrec *) (DIRADDR(d) + HDR + (i) * REC))
#define FREE_SLOT(r)	((r)->addr == 0xFFFFFFFFUL)
#define LIVE(r)		(!FREE_SLOT(r) && (r)->dead0 == 0xFFFFFFFFUL)
#define GEN(d)		(((unsigned long *) DIRADDR(d))[1])
#define BINDING(s)	((unsigned long) (s)->st_ino << 16 \
					| ((unsigned long) (s)->st_dev & 0xFFFF))

/*===========================================================================*
 *				SHA-256					     *
 *===========================================================================*/
/* FIPS 180-4, small rather than fast: used at install time only. */
struct sha { unsigned long h[8], n; unsigned char b[64]; };

PRIVATE const unsigned long sha_k[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
  0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n)	(((x) >> (n)) | ((x) << (32 - (n))))

PRIVATE void sha_block(s)
struct sha *s;
{
  unsigned long w[16], a[8], t1, t2, x;
  int i;

  for (i = 0; i < 16; i++)
	w[i] = (unsigned long) s->b[4*i] << 24 | (unsigned long) s->b[4*i+1]
		<< 16 | (unsigned long) s->b[4*i+2] << 8 | s->b[4*i+3];
  for (i = 0; i < 8; i++) a[i] = s->h[i];
  for (i = 0; i < 64; i++) {
	if (i >= 16) {		/* the schedule, in a 16-word ring */
		x = w[(i + 1) & 15];
		t1 = ROR(x, 7) ^ ROR(x, 18) ^ (x >> 3);
		x = w[(i + 14) & 15];
		t2 = ROR(x, 17) ^ ROR(x, 19) ^ (x >> 10);
		w[i & 15] += t1 + t2 + w[(i + 9) & 15];
	}
	t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25))
		+ ((a[4] & a[5]) ^ (~a[4] & a[6])) + sha_k[i] + w[i & 15];
	t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22))
		+ ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
	a[7] = a[6]; a[6] = a[5]; a[5] = a[4]; a[4] = a[3] + t1;
	a[3] = a[2]; a[2] = a[1]; a[1] = a[0]; a[0] = t1 + t2;
  }
  for (i = 0; i < 8; i++) s->h[i] += a[i];
}

PRIVATE void sha_init(s)
struct sha *s;
{
  static const unsigned long h0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372,
	0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

  memcpy(s->h, h0, sizeof(h0));
  s->n = 0;
}

PRIVATE void sha_add(s, p, n)
struct sha *s;
const unsigned char *p;
unsigned long n;
{
  while (n-- > 0) {
	s->b[s->n++ & 63] = *p++;
	if ((s->n & 63) == 0) sha_block(s);
  }
}

PRIVATE void sha_end(s, out)
struct sha *s;
unsigned char *out;
{
  unsigned long bits = s->n << 3;
  unsigned char z = 0x80;
  int i;

  sha_add(s, &z, 1);
  z = 0;
  while ((s->n & 63) != 56) sha_add(s, &z, 1);
  for (i = 0; i < 4; i++) { z = 0; sha_add(s, &z, 1); }	/* bits 63-32 */
  for (i = 3; i >= 0; i--) { z = bits >> (8 * i); sha_add(s, &z, 1); }
  for (i = 0; i < 32; i++) out[i] = s->h[i / 4] >> (24 - 8 * (i % 4));
}

PRIVATE int in_region(sizep)	/* in_st = base | 2, *sizep = size */
unsigned long *sizep;
{
  unsigned long base;

  if (sys_flash(FL_INFO, FL_LAYOUT_INST, NULL, 0, &base, sizep) != OK
	|| base == 0) return(ENOSYS);
  if ((in_st & ~3UL) != base) in_st = base | 2;
  return(OK);
}

/*===========================================================================*
 *				in_init					     *
 *===========================================================================*/
PUBLIC void in_init()
{
/* The installed region and its active directory (nothing written: an
 * empty region gets its directory on the first install). */
  unsigned long *h0, *h1, size;
  int ok0, ok1;

  if (in_region(&size) != OK || size < (DIRPAGES + 1) * PAGE) {
	in_st = 0;
	return;
  }
  h0 = (unsigned long *) DIRADDR(0);
  h1 = (unsigned long *) DIRADDR(1);
  ok0 = h0[0] == IN_MAGIC && h0[1] != 0xFFFFFFFFUL;
  ok1 = h1[0] == IN_MAGIC && h1[1] != 0xFFFFFFFFUL;
  if (ok0 && ok1) SET_DIR(h1[1] > h0[1]);
  else if (ok0 || ok1) SET_DIR(ok1);
  else SET_DIR(2);			/* none yet */
}

/*===========================================================================*
 *				in_lookup				     *
 *===========================================================================*/
PUBLIC unsigned long in_lookup(s, size)
struct stat *s;			/* the program file */
unsigned long size;		/* its text size */
{
/* Where the installed text for this file is, or 0 (R2b.4). */
  struct inrec *r, *best = NULL;
  unsigned long id = BINDING(s);
  int i;

  if (in_st == 0 || in_dir > 1) return(0);
  for (i = 0; i < NREC; i++) {
	r = RECP(in_dir, i);
	if (FREE_SLOT(r)) break;
	if (LIVE(r) && r->id == id && r->ctime == (unsigned long) s->st_ctime
	    && r->size == size && (best == NULL || r->seq > best->seq))
		best = r;
  }
  return(best != NULL ? best->addr : 0);
}

/*===========================================================================*
 *				Writing helpers				     *
 *===========================================================================*/
PRIVATE int in_format(d, gen)
int d;
unsigned long gen;
{
  unsigned long h[2];

  if (sys_flash(FL_ERASE, DIRADDR(d), NULL, 0, NULL, NULL) != OK)
	return(EIO);
  h[0] = IN_MAGIC;
  h[1] = gen;
  return(sys_flash(FL_WRITE, DIRADDR(d), h, sizeof(h), NULL, NULL));
}

PRIVATE int in_running(addr)
unsigned long addr;
{
  struct mproc *rmp;

  for (rmp = &mproc[0]; rmp < &mproc[NR_PROCS]; rmp++)
	if ((rmp->mp_flags & IN_USE)
		&& ((unsigned long) rmp->mp_seg[T].mem_phys << CLICK_SHIFT)
								== addr)
		return(TRUE);
  return(FALSE);
}

PRIVATE int in_compact()
{
/* Copy the live records to the other directory page, the new active one
 * (only when the directory is full, R2b.7). */
  int d = !in_dir, i, j;
  struct inrec *r;

  if (in_format(d, GEN(in_dir) + 1) != OK) return(EIO);
  for (i = j = 0; i < NREC; i++) {
	r = RECP(in_dir, i);
	if (FREE_SLOT(r)) break;
	if (!LIVE(r) && !in_running(r->addr)) continue;
	/* dead ones still run from keep their pages until they exit */
	if (sys_flash(FL_WRITE, (unsigned long) RECP(d, j), r,
				REC, NULL, NULL) != OK) return(EIO);
	j++;
  }
  (void) sys_flash(FL_ERASE, DIRADDR(in_dir), NULL, 0, NULL, NULL);
  SET_DIR(d);
  return(OK);
}

PRIVATE int in_used(addr, n)	/* is [addr, +n) live or being run? */
unsigned long addr, n;
{
  struct inrec *r;
  unsigned long e;
  int i;

  for (i = 0; i < NREC; i++) {
	r = RECP(in_dir, i);
	if (FREE_SLOT(r)) break;
	if (!LIVE(r) && !in_running(r->addr)) continue;
	e = r->addr + (r->size + PAGE - 1) / PAGE * PAGE;
	if (r->addr < addr + n && addr < e) return(TRUE);
  }
  return(FALSE);
}

PRIVATE unsigned long in_find_run(npages)
unsigned long npages;
{
/* First fit: npages free pages in a row. */
  unsigned long a, end, size;

  if (in_region(&size) != OK) return(0);
  end = in_base + size;

  for (a = in_base + DIRPAGES * PAGE; a + npages * PAGE <= end; a += PAGE)
	if (!in_used(a, npages * PAGE)) return(a);
  return(0);
}

PRIVATE int in_kill(r)		/* mark a record dead */
struct inrec *r;
{
  unsigned long zero[2];

  zero[0] = zero[1] = 0;
  return(sys_flash(FL_WRITE, (unsigned long) &r->dead0, zero,
						sizeof(zero), NULL, NULL));
}

/*===========================================================================*
 *				in_install				     *
 *===========================================================================*/
PRIVATE int in_install(fd, s)
int fd;				/* the program, open */
struct stat *s;
{
  struct exec hdr;
  struct sha sh;
  struct inrec *r;
  unsigned char sha[32];
  unsigned long npages, addr, done, rec[14], id = BINDING(s);
  int i, n, k;

  /* Only a file nobody but root or bin could have written (R2b.5 b). */
  if ((s->st_uid != SUPER_USER && s->st_uid != BIN_UID)
	|| (s->st_mode & (S_IWGRP | S_IWOTH))) return(EPERM);
  if (read(fd, (char *) &hdr, sizeof(hdr)) < (int) A_MINHDR
	|| BADMAG(hdr) || !(hdr.a_flags & A_SEP) || hdr.a_text <= 0)
	return(ENOEXEC);

  if (in_dir > 1) {			/* first install: a directory */
	if (in_format(0, 1) != OK) return(EIO);
	SET_DIR(0);
  }
  for (i = 0; i < NREC; i++) {
	r = RECP(in_dir, i);
	if (FREE_SLOT(r)) break;
	if (!LIVE(r) || r->id != id) continue;
	if (r->ctime == (unsigned long) s->st_ctime
		&& r->size == (unsigned long) hdr.a_text) return(OK);
  }
  if (i >= NREC) {			/* directory full: compact once */
	if (in_compact() != OK) return(EIO);
	for (i = 0; i < NREC; i++)
		if (FREE_SLOT(RECP(in_dir, i))) break;
	if (i >= NREC) return(ENOSPC);
  }

  npages = ((unsigned long) hdr.a_text + PAGE - 1) / PAGE;
  if ((addr = in_find_run(npages)) == 0) return(ENOSPC);
  for (done = 0; done < npages * PAGE; done += PAGE)
	if (sys_flash(FL_ERASE, addr + done, NULL, 0, NULL, NULL) != OK)
		return(EIO);

  /* The text, hashed as it is written ... */
  sha_init(&sh);
  if (lseek(fd, (off_t) hdr.a_hdrlen, SEEK_SET) != hdr.a_hdrlen)
	return(EIO);
  for (done = 0; done < (unsigned long) hdr.a_text; done += n) {
	n = hdr.a_text - done < CHUNK ? (int) (hdr.a_text - done) : CHUNK;
	if (read(fd, mbuf, n) != n) return(EIO);
	sha_add(&sh, (unsigned char *) mbuf, (unsigned long) n);
	k = (n + 7) & ~7;
	for (i = n; i < k; i++) mbuf[i] = (char) 0xFF;
	if (sys_flash(FL_WRITE, addr + done, mbuf, (unsigned) k, NULL, NULL)
								!= OK)
		return(EIO);
  }
  sha_end(&sh, sha);

  /* ... and read back (R2b.5): the flash must hold exactly that. */
  sha_init(&sh);
  sha_add(&sh, (unsigned char *) addr, (unsigned long) hdr.a_text);
  sha_end(&sh, (unsigned char *) mbuf);
  if (memcmp(sha, mbuf, 32) != 0) {
	printf("install: the flash copy does not read back\n");
	return(EIO);
  }

  /* The record: now exec finds it.  Then the old version goes. */
  memcpy(rec, sha, 32);
  rec[8] = addr;
  rec[9] = hdr.a_text;
  rec[10] = id;
  rec[11] = s->st_ctime;
  rec[12] = 0;
  for (i = 0; i < NREC; i++) {		/* the next seq */
	r = RECP(in_dir, i);
	if (FREE_SLOT(r)) break;
	if (r->seq >= rec[12]) rec[12] = r->seq + 1;
  }
  rec[13] = 0xFFFFFFFFUL;
  for (i = 0; i < NREC; i++)
	if (FREE_SLOT(RECP(in_dir, i))) break;
  if (sys_flash(FL_WRITE, (unsigned long) RECP(in_dir, i), rec,
					sizeof(rec), NULL, NULL) != OK)
	return(EIO);
  for (i = 0; i < NREC; i++) {		/* older versions of the file */
	r = RECP(in_dir, i);
	if (FREE_SLOT(r)) break;
	if (LIVE(r) && r->id == id && r->seq != rec[12]) (void) in_kill(r);
  }
  return(OK);
}

/*===========================================================================*
 *				do_install				     *
 *===========================================================================*/
PUBLIC int do_install()
{
/* MEMINFO with m1_i2 = MI_INSTALL (root only, R2b.2): m1_p1 = path,
 * m1_i1 = its length; m1_i3 = 0: install the program's text into the
 * flash region, 1: mark its records dead (pages free once no process
 * runs from them). */
  struct stat s;
  struct inrec *r;
  int fd, i, len = mm_in.m1_i1, rv;

  if (mp->mp_effuid != SUPER_USER) return(EPERM);	/* AC2b.4 */
  if (in_st == 0) return(ENOSYS);
  if (len <= 0 || len > 256) return(EINVAL);
  if (sys_copy(who, D, (phys_bytes) mm_in.m1_p1, MM_PROC_NR, D,
			(phys_bytes) (mbuf + 3584), (phys_bytes) len) != OK)
	return(EFAULT);
  mbuf[3584 + len - 1] = 0;
  tell_fs(CHDIR, who, FALSE, 0);		/* the caller's directory */

  if (mm_in.m1_i3 == 0) {			/* install */
	if ((fd = allowed(mbuf + 3584, &s, X_BIT)) < 0) return(fd);
	rv = in_install(fd, &s);
	close(fd);
	return(rv);
  }
  /* uninstall: every live record for this file (any version) */
  if (stat(mbuf + 3584, &s) < 0) return(-errno);
  if (in_dir > 1) return(ENOENT);
  rv = ENOENT;
  for (i = 0; i < NREC; i++) {
	r = RECP(in_dir, i);
	if (FREE_SLOT(r)) break;
	if (!LIVE(r) || r->id != BINDING(&s)) continue;
	if (in_running(r->addr)) return(EBUSY);
	rv = in_kill(r);
  }
  return(rv);
}

/*===========================================================================*
 *				in_info					     *
 *===========================================================================*/
PUBLIC void in_info(entries, used)
int *entries;
unsigned long *used;
{
  struct inrec *r;
  int i;

  *entries = 0;
  *used = 0;
  if (in_st == 0 || in_dir > 1) return;
  for (i = 0; i < NREC; i++) {
	r = RECP(in_dir, i);
	if (FREE_SLOT(r)) break;
	if (!LIVE(r)) continue;
	(*entries)++;
	*used += (r->size + PAGE - 1) / PAGE * PAGE;
  }
}

#endif /* CHIP == ARM_CM */
