/* Installed programs in flash (Galtzagorriak MINIX, spec Phase 2b,
 * R2b.2-R2b.5, R2b.7, R2b.8).
 *
 * The text of a program installed by root (pkg install --flash) is written
 * into internal flash once, at install time, and every exec of that file
 * runs it in place from there - no RAM for text, no run-time cache write.
 *
 * Region (hal_flash_layout(): from 0x08048000, after the shared libraries, up to
 * the run-time cache):
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
#include "gzbuild.h"		/* GZ_BUILD_EPOCH: the clock is set after it */

#if (CHIP == ARM_CM)

extern char mbuf[];			/* exec.c's buffer, free here */

#define IN_MAGIC	0x534E4947UL	/* "GINS" */
#define IN_SEED		0x424E4947UL	/* "GINB": made on the PC, base-flash */
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

FORWARD _PROTOTYPE( int in_clock_set, (void)				);

#define in_base		(in_st & ~3UL)
#define in_dir		((int) (in_st & 3))
#define SET_DIR(d)	(in_st = in_base | (d))
#define DIRADDR(d)	(in_base + (d) * PAGE)
#define RECP(d, i)	((struct inrec *) (DIRADDR(d) + HDR + (i) * REC))
/* A record is written in ascending double words, seq last but one: a power
 * cut can leave it torn (AC2b.5).  Only a slot that is erased throughout is
 * free (programming a written double word again fails); only a record
 * with its address and seq written counts; a torn one is skipped and
 * dropped by the next compaction. */
#define FREE_SLOT(r)	in_erased((unsigned long *) (r))
#define WHOLE(r)	((r)->addr != 0xFFFFFFFFUL && (r)->seq != 0xFFFFFFFFUL)
#define LIVE(r)		(WHOLE(r) && (r)->dead0 == 0xFFFFFFFFUL)
#define MAGIC(d)	(((unsigned long *) DIRADDR(d))[0])
#define GEN(d)		(((unsigned long *) DIRADDR(d))[1])
#define UNBOUND(r)	((r)->id == 0xFFFFFFFFUL && (r)->ctime == 0xFFFFFFFFUL)
#define HEADCRC		256	/* bytes of text under the record's CRC */
#define BINDING(s)	((unsigned long) (s)->st_ino << 16 \
					| ((unsigned long) (s)->st_dev & 0xFFFF))

PRIVATE int in_erased(w)	/* a whole record still erased? */
unsigned long *w;
{
  int i;

  for (i = 0; i < REC / 4; i++) if (w[i] != 0xFFFFFFFFUL) return(FALSE);
  return(TRUE);
}

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
  ok0 = (h0[0] == IN_MAGIC || h0[0] == IN_SEED) && h0[1] != 0xFFFFFFFFUL;
  ok1 = (h1[0] == IN_MAGIC || h1[0] == IN_SEED) && h1[1] != 0xFFFFFFFFUL;
  if (ok0 && ok1) SET_DIR(h1[1] > h0[1]);
  else if (ok0 || ok1) SET_DIR(ok1);
  else SET_DIR(2);			/* none yet */
}

/*===========================================================================*
 *				in_lookup				     *
 *===========================================================================*/
PUBLIC unsigned long in_lookup(s, size, fd, toff)
struct stat *s;			/* the program file */
unsigned long size;		/* its text size */
int fd;				/* the file, open */
unsigned long toff;		/* where its text starts in it */
{
/* Where the installed text for this file is, or 0 (R2b.4).  The binding
 * (dev, ino, ctime) and the size must match, and the CRC-32 of the first
 * HEADCRC bytes of the text (#58): a new file that got an old program's
 * inode number and ctime (a clock that was not set) is not taken for it.
 * Records made before the CRC have flags = all ones: binding only.  The
 * file position is put back for exec. */
  struct inrec *r, *best = NULL;
  unsigned long id = BINDING(s), crc, n, k;
  unsigned char b[64];		/* MM's stack: small pieces */
  int i;

  if (in_st == 0 || in_dir > 1) return(0);
  for (i = 0; i < NREC; i++) {
	r = RECP(in_dir, i);
	if (FREE_SLOT(r)) break;
	if (LIVE(r) && !UNBOUND(r) && r->id == id && r->ctime == (unsigned long) s->st_ctime
	    && r->size == size && (best == NULL || r->seq > best->seq))
		best = r;
  }
  if (best == NULL) return(0);
  if (best->flags == 0xFFFFFFFFUL) return(best->addr);
  n = size < HEADCRC ? size : HEADCRC;
  crc = 0xFFFFFFFFUL;
  if (lseek(fd, (off_t) toff, SEEK_SET) < 0) return(0);
  for (; n > 0; n -= k) {		/* the same block exec read: cached */
	k = n < sizeof(b) ? n : sizeof(b);
	if (read(fd, (char *) b, (unsigned) k) != (int) k) break;
	crc = crc32_more(crc, b, k);
  }
  (void) lseek(fd, (off_t) toff, SEEK_SET);
  return(n == 0 && ~crc == best->flags ? best->addr : 0);
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

PRIVATE int in_stale(r)		/* is r's file gone or replaced? */
struct inrec *r;
{
/* R2b.12, #58: ask FS about the inode of r's binding.  Only a definite
 * "no" counts (the device mounted, the inode free or with another ctime):
 * a device that is not mounted (another card, /usr not yet) keeps it. */
  message m;

  if (UNBOUND(r)) return(FALSE);
  m.m1_i1 = (int) (r->id & 0xFFFF);
  m.m1_i2 = (int) (r->id >> 16);
  m.m1_i3 = (int) r->ctime;
  return(_taskcall(FS_PROC_NR, INOCHK, &m) == 0);
}

PRIVATE int in_held(i)		/* a current live record for i's text? */
int i;
{
  struct inrec *r = RECP(in_dir, i), *q;
  int j;

  for (j = 0; j < NREC; j++) {
	q = RECP(in_dir, j);
	if (FREE_SLOT(q)) break;
	if (j != i && LIVE(q) && q->addr == r->addr && !in_stale(q))
		return(TRUE);
  }
  return(FALSE);
}

PRIVATE int in_covered(i)	/* another record for record i's text? */
int i;
{
  struct inrec *r = RECP(in_dir, i), *q;
  int j;

  for (j = 0; j < NREC; j++) {
	q = RECP(in_dir, j);
	if (FREE_SLOT(q)) break;
	if (j != i && WHOLE(q) && q->addr == r->addr && (LIVE(q) || j < i))
		return(TRUE);
  }
  return(FALSE);
}

PRIVATE int in_compact()
{
/* Copy the live records to the other directory page, the new active one:
 * when the directory is full (R2b.7) or was made on the PC (in_writable).
 * The records go through MM's memory (the kernel takes no flash source).
 * Double words still erased in a record (the binding of an unbound one,
 * the dead marker of a live one) stay erased, to be written later. */
  int d = !in_dir, i, j;
  struct inrec *r;
  unsigned long rec[REC / 4], to;

  /* The header last: a power cut before it leaves the old directory the
   * valid one (AC2b.5); the new page is erased again next time. */
  if (sys_flash(FL_ERASE, DIRADDR(d), NULL, 0, NULL, NULL) != OK)
	return(EIO);
  for (i = j = 0; i < NREC; i++) {
	r = RECP(in_dir, i);
	if (FREE_SLOT(r)) break;
	/* dead ones still run from keep their pages until they exit -
	 * unless another record (live, or an earlier dead one) covers the
	 * same text: an upgrade that leaves a text unchanged gives it a new
	 * record, and the dead one of every running program (sh, getty,
	 * ...) would otherwise stay until the directory is full for good */
	if (!LIVE(r) && (!in_running(r->addr) || in_covered(i))) continue;
	/* a live record whose file is gone (rm + tar x, a restored card:
	 * pkg bind gave the text a new record) is not copied either -
	 * unless a process runs from it and no current record has the
	 * text (then its pages must stay used) */
	if (LIVE(r) && in_stale(r)
		&& (!in_running(r->addr) || in_held(i))) continue;
	memcpy(rec, r, REC);
	to = (unsigned long) RECP(d, j);
	if (sys_flash(FL_WRITE, to, rec, 40, NULL, NULL) != OK
	    || (!UNBOUND(r) && sys_flash(FL_WRITE, to + 40, rec + 10, 8,
							NULL, NULL) != OK)
	    || sys_flash(FL_WRITE, to + 48, rec + 12, 8, NULL, NULL) != OK
	    || (!LIVE(r) && sys_flash(FL_WRITE, to + 56, rec + 14, 8,
							NULL, NULL) != OK))
		return(EIO);
	j++;
  }
  rec[0] = IN_MAGIC;
  rec[1] = GEN(in_dir) + 1;
  if (sys_flash(FL_WRITE, DIRADDR(d), rec, 8, NULL, NULL) != OK)
	return(EIO);
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

  unsigned long rb, rs;

  if (in_region(&size) != OK) return(0);
  if (size > 256 * PAGE) size = 256 * PAGE;	/* mjob.page is a byte */
  end = in_base + size;
  a = in_base + DIRPAGES * PAGE;
  /* not the settings image (/dev/rom, L.2) after the directories */
  if (sys_flash(FL_INFO, FL_LAYOUT_ROM, NULL, 0, &rb, &rs) == OK
						&& rb == a) a += rs;

  for (; a + npages * PAGE <= end; a += PAGE)
	if (!in_used(a, npages * PAGE)) return(a);
  return(0);
}

PRIVATE int in_writable()
{
/* Before writing to the directory: make one if there is none, and copy one
 * made on the PC to a page erased here first - DFU programmed its free
 * double words with ones, which must not be programmed again (ECC). */
  if (in_dir > 1) {
	if (in_format(0, 1) != OK) return(EIO);
	SET_DIR(0);
	return(OK);
  }
  return(MAGIC(in_dir) == IN_SEED ? in_compact() : OK);
}

PRIVATE int in_kill(r)		/* mark a record dead */
struct inrec *r;
{
  unsigned long zero[2];

  zero[0] = zero[1] = 0;
  return(sys_flash(FL_WRITE, (unsigned long) &r->dead0, zero,
						sizeof(zero), NULL, NULL));
}

PRIVATE int in_check(fd, s, hdr)
int fd;
struct stat *s;
struct exec *hdr;
{
  /* Only a file nobody but root or bin could have written (R2b.5 b). */
  if ((s->st_uid != SUPER_USER && s->st_uid != BIN_UID)
	|| (s->st_mode & (S_IWGRP | S_IWOTH))) return(EPERM);
  if (read(fd, (char *) hdr, sizeof(*hdr)) < (int) A_MINHDR
	|| BADMAG(*hdr) || !(hdr->a_flags & A_SEP) || hdr->a_text <= 0)
	return(ENOEXEC);
  return(OK);
}

PRIVATE int in_text(fd, hdr, sha)
int fd;
struct exec *hdr;
unsigned char *sha;		/* SHA-256 of the file's text */
{
  struct sha sh;
  unsigned long done;
  int n;

  sha_init(&sh);
  if (lseek(fd, (off_t) hdr->a_hdrlen, SEEK_SET) != hdr->a_hdrlen)
	return(EIO);
  for (done = 0; done < (unsigned long) hdr->a_text; done += n) {
	n = hdr->a_text - done < CHUNK ? (int) (hdr->a_text - done) : CHUNK;
	if (read(fd, mbuf, n) != n) return(EIO);
	sha_add(&sh, (unsigned char *) mbuf, (unsigned long) n);
  }
  sha_end(&sh, sha);
  return(OK);
}

PRIVATE void in_hash(out, p, n)		/* SHA-256 of memory (flash) */
unsigned char *out;
unsigned char *p;
unsigned long n;
{
  struct sha sh;

  sha_init(&sh);
  sha_add(&sh, p, n);
  sha_end(&sh, out);
}

PRIVATE int in_flash_ok(addr, size, sha)	/* does the flash hold it? */
unsigned long addr, size;
unsigned char *sha;
{
  in_hash((unsigned char *) mbuf, (unsigned char *) addr, size);
  return(memcmp(sha, mbuf, 32) == 0);
}

PRIVATE void in_old(id, keep)	/* other live records for the file: dead */
unsigned long id;
struct inrec *keep;
{
  struct inrec *r;
  int i;

  for (i = 0; i < NREC; i++) {
	r = RECP(in_dir, i);
	if (FREE_SLOT(r)) break;
	if (r != keep && LIVE(r) && !UNBOUND(r) && r->id == id)
		(void) in_kill(r);
  }
}

/*===========================================================================*
 *				Records					     *
 *===========================================================================*/
PRIVATE int in_has(s, size)	/* is this version of the file installed? */
struct stat *s;
unsigned long size;
{
  struct inrec *r;
  int i;

  for (i = 0; i < NREC && in_dir <= 1; i++) {
	r = RECP(in_dir, i);
	if (FREE_SLOT(r)) break;
	if (LIVE(r) && !UNBOUND(r) && r->id == BINDING(s)
		&& r->ctime == (unsigned long) s->st_ctime && r->size == size)
		return(TRUE);
  }
  return(FALSE);
}

#define S_LIVE		0	/* in_same: live records */
#define S_UNBOUND	1	/*   live, not bound yet */
#define S_ANY		2	/*   dead ones too: check the flash then */

PRIVATE struct inrec *in_same(size, sha, which)
unsigned long size;
unsigned char *sha;		/* NULL: any text of that size */
int which;			/* S_LIVE, S_UNBOUND, S_ANY */
{
/* A record for this text (aliases such as [ and test, cp and mv are
 * separate files with one text: one copy in flash, a record each; an
 * upgrade with an unchanged program finds the old version's text). */
  struct inrec *r;
  int i;

  for (i = 0; i < NREC && in_dir <= 1; i++) {
	r = RECP(in_dir, i);
	if (FREE_SLOT(r)) break;
	if ((which == S_ANY || LIVE(r)) && r->addr != 0xFFFFFFFFUL
		&& r->size == size
		&& (which != S_UNBOUND || UNBOUND(r))
		&& (sha == NULL || memcmp(r->sha, sha, 32) == 0)) return(r);
  }
  return(NULL);
}

PRIVATE int in_record(addr, size, sha, s)
unsigned long addr, size;
unsigned char *sha;		/* in MM's memory, not the directory */
struct stat *s;
{
/* Append the record of a text for this file; then older ones are dead. */
  struct inrec *r;
  unsigned long rec[14];
  int i;

  if (!in_clock_set()) return(EAGAIN);	/* no binding with a bad clock */
  for (i = 0; i < NREC; i++)
	if (FREE_SLOT(RECP(in_dir, i))) break;
  if (i >= NREC) {			/* directory full: compact once */
	if (in_compact() != OK) return(EIO);
	for (i = 0; i < NREC; i++)
		if (FREE_SLOT(RECP(in_dir, i))) break;
	if (i >= NREC) return(ENOSPC);
  }
  memcpy(rec, sha, 32);
  rec[8] = addr;
  rec[9] = size;
  rec[10] = BINDING(s);
  rec[11] = s->st_ctime;
  rec[12] = 0;
  for (i = 0; i < NREC; i++) {		/* the next seq */
	r = RECP(in_dir, i);
	if (FREE_SLOT(r)) break;
	if (WHOLE(r) && r->seq >= rec[12]) rec[12] = r->seq + 1;
  }
  /* flags: CRC-32 of the text's start, from the flash (in_lookup) */
  rec[13] = ~crc32_more(0xFFFFFFFFUL, (const unsigned char *) addr,
				size < HEADCRC ? size : (unsigned long) HEADCRC);
  if (rec[13] == 0xFFFFFFFFUL) rec[13] = 0xFFFFFFFEUL;	/* all ones: none */
  r = RECP(in_dir, i);
  if (sys_flash(FL_WRITE, (unsigned long) r, rec, sizeof(rec), NULL, NULL)
								!= OK)
	return(EIO);
  in_old(rec[10], r);
  return(OK);
}

/*===========================================================================*
 *				in_clock_set				     *
 *===========================================================================*/
PRIVATE int in_clock_set()
{
/* A binding is (dev, ino, ctime): with the clock not set (the RTC does not
 * run: every boot starts at 1970) ctimes repeat between boots, and a new
 * file could get an old program's binding.  So no new bindings then: the
 * programs run from RAM (spec I.1, #58).  Set: not before this commit. */
  message m;

  m.m_type = GET_TIME;
  if (sendrec(CLOCK, &m) != OK) return(FALSE);
  return((unsigned long) m.NEW_TIME >= GZ_BUILD_EPOCH);
}

/*===========================================================================*
 *				in_install				     *
 *===========================================================================*/
PRIVATE int in_install(fd, s)
int fd;				/* the program, open */
struct stat *s;
{
/* Start an install: 1 if the caller must go on with MI_INSTALL op 4 (one
 * flash step per call, in_step), else done (OK or an error).  The steps
 * keep their state in MM's flash job (fcache.c), so MM serves exec and
 * everybody else between them: an exec waits for one page erase (22 ms)
 * or 256 bytes, never for a whole install (AC2b.R2, R0.8c). */
  struct exec hdr;
  struct inrec *r;
  unsigned char sha[32];
  unsigned long npages, addr, size;
  int i;

  if ((i = in_check(fd, s, &hdr)) != OK) return(i);
  size = (unsigned long) hdr.a_text;
  if (in_has(s, size)) return(OK);		/* nothing to write */
  if (!in_clock_set()) return(EAGAIN);	/* before any page is written */
  if (in_writable() != OK) return(EIO);

  /* The same text already there (an alias): only a record. */
  if (in_text(fd, &hdr, sha) != OK) return(EIO);
  if ((r = in_same(size, sha, S_ANY)) != NULL
	&& in_flash_ok(addr = r->addr, size, sha))
	return(in_record(addr, size, sha, s));

  npages = (size + PAGE - 1) / PAGE;
  if ((addr = in_find_run(npages)) == 0) return(ENOSPC);
  if ((i = fc_lend()) != OK) return(i);
  mjob.fd = fd;				/* do_install leaves it open */
  mjob.state = J_INST_ERASE;
  mjob.page = (addr - in_base) / PAGE;
  mjob.toff = hdr.a_hdrlen;
  mjob.size = size;
  mjob.crc = (unsigned long) s->st_ctime;
  mjob.done = 0;
  mjob.owner = who;
  mjob.vcrc = (unsigned long) mp->mp_pid;
  return(1);
}

/*===========================================================================*
 *				in_step					     *
 *===========================================================================*/
PRIVATE int in_step()
{
/* One step of the caller's install: 1 for more, else OK or an error.
 * Text pages, each piece compared with the file as it is programmed; then
 * the record (the file must not have changed meanwhile: its ctime), and
 * the old version's record is marked dead. */
  struct stat s;
  unsigned char sha[32];
  unsigned long addr = in_base + (unsigned long) mjob.page * PAGE;
  unsigned long npages = (mjob.size + PAGE - 1) / PAGE;
  int r = OK, i, n, k;

  if (!J_INSTALLING(mjob.state) || mjob.owner != who
		|| mjob.vcrc != (unsigned long) mp->mp_pid) return(EINVAL);
  switch (mjob.state) {
  case J_INST_ERASE:			/* a page per call */
	if (sys_flash(FL_ERASE, addr + mjob.done, NULL, 0, NULL, NULL) != OK)
		{ r = EIO; break; }
	mjob.done += PAGE;
	if (mjob.done == npages * PAGE) {
		mjob.state = J_INST_PROGRAM;
		mjob.done = 0;
	}
	return(1);
  case J_INST_PROGRAM:			/* CHUNK bytes per call */
	n = mjob.size - mjob.done < CHUNK ? (int) (mjob.size - mjob.done)
					  : CHUNK;
	if (lseek(mjob.fd, (off_t) (mjob.toff + mjob.done), SEEK_SET) < 0
			|| read(mjob.fd, mbuf, n) != n) { r = EIO; break; }
	k = (n + 7) & ~7;
	for (i = n; i < k; i++) mbuf[i] = (char) 0xFF;
	if (sys_flash(FL_WRITE, addr + mjob.done, mbuf, (unsigned) k,
							NULL, NULL) != OK
		|| memcmp((char *) (addr + mjob.done), mbuf, n) != 0) {
		printf("install: the flash copy does not read back\n");
		r = EIO;
		break;
	}
	mjob.done += n;
	if (mjob.done == mjob.size) mjob.state = J_INST_RECORD;
	return(1);
  case J_INST_RECORD:	/* every piece matched the file as read: if the
			 * file is unchanged, the flash holds its text */
	if (fstat(mjob.fd, &s) < 0
		|| (unsigned long) s.st_ctime != mjob.crc) { r = EIO; break; }
	in_hash(&sha[0], (unsigned char *) addr, mjob.size);
	r = in_record(addr, mjob.size, sha, &s);
	break;
  }
  close(mjob.fd);			/* done, or given up: the pages stay */
  fc_return();				/* unclaimed and are reused */
  return(r);
}

/*===========================================================================*
 *				in_bind					     *
 *===========================================================================*/
PRIVATE int in_bind(fd, s)
int fd;				/* the program, open */
struct stat *s;
{
/* Programs built into the firmware (make base-flash, R2b.6) have records
 * without a binding: give this file the one whose text it holds, or a
 * record of its own for a text already bound to an alias. */
  struct exec hdr;
  struct inrec *r;
  unsigned char sha[32];
  unsigned long b[2], size, addr;
  int i;

  if ((i = in_check(fd, s, &hdr)) != OK) return(i);
  size = (unsigned long) hdr.a_text;
  if (in_has(s, size)) return(OK);
  if (in_same(size, (unsigned char *) NULL, S_LIVE) == NULL)
	return(ENOENT);			/* cheap: no hashing */
  if (in_text(fd, &hdr, sha) != OK) return(EIO);
  if ((r = in_same(size, sha, S_UNBOUND)) == NULL)
	r = in_same(size, sha, S_LIVE);
  if (r == NULL) return(ENOENT);
  if (!in_flash_ok(addr = r->addr, size, sha)) {
	printf("install: built-in text at %lx is damaged\n", addr);
	return(EIO);
  }
  if (!in_clock_set()) return(EAGAIN);	/* see in_clock_set */
  if (in_writable() != OK) return(EIO);	/* may move the records */
  if ((r = in_same(size, sha, S_UNBOUND)) == NULL)	/* an alias */
	return(in_record(addr, size, sha, s));
  b[0] = BINDING(s);
  b[1] = s->st_ctime;
  if (sys_flash(FL_WRITE, (unsigned long) &r->id, b, sizeof(b), NULL, NULL)
								!= OK)
	return(EIO);
  in_old(b[0], r);
  return(OK);
}

/*===========================================================================*
 *				do_install				     *
 *===========================================================================*/
PUBLIC int do_install()
{
/* MEMINFO with m1_i2 = MI_INSTALL (root only, R2b.2): m1_p1 = path,
 * m1_i1 = its length; m1_i3 = 0: install the program's text into the
 * flash region, 1: mark its records dead (their pages are free once no
 * process runs from them), 2: bind it to a built-in record (in_bind), 3: return
 * the number of built-in records not bound yet (no path), 4: the next step
 * of the caller's install (op 0 answered 1: call again until not 1). */
  struct stat s;
  struct inrec *r;
  int fd, i, len = mm_in.m1_i1, rv;

  if (mp->mp_effuid != SUPER_USER) return(EPERM);	/* AC2b.4 */
  if (in_st == 0) return(ENOSYS);
  if (mm_in.m1_i3 == 4) return(in_step());	/* the next step */
  if (mm_in.m1_i3 == 3) {
	for (rv = i = 0; i < NREC && in_dir <= 1; i++) {
		r = RECP(in_dir, i);
		if (FREE_SLOT(r)) break;
		if (LIVE(r) && UNBOUND(r)) rv++;
	}
	return(rv);
  }
  if (len <= 0 || len > 256) return(EINVAL);
  if (sys_copy(who, D, (phys_bytes) mm_in.m1_p1, MM_PROC_NR, D,
			(phys_bytes) (mbuf + 3584), (phys_bytes) len) != OK)
	return(EFAULT);
  mbuf[3584 + len - 1] = 0;
  tell_fs(CHDIR, who, FALSE, 0);		/* the caller's directory */

  if (mm_in.m1_i3 == 0 || mm_in.m1_i3 == 2) {	/* install, bind */
	if ((fd = allowed(mbuf + 3584, &s, X_BIT)) < 0) return(fd);
	rv = mm_in.m1_i3 == 0 ? in_install(fd, &s) : in_bind(fd, &s);
	if (rv != 1) close(fd);		/* 1: the install job keeps it */
	return(rv);
  }
  /* uninstall: every live record for this file (any version) */
  if (stat(mbuf + 3584, &s) < 0) return(-errno);
  if (in_dir > 1) return(ENOENT);
  for (rv = ENOENT, i = 0; i < NREC; i++) {	/* any?  all idle? */
	r = RECP(in_dir, i);
	if (FREE_SLOT(r)) break;
	if (!LIVE(r) || UNBOUND(r) || r->id != BINDING(&s)) continue;
	rv = OK;	/* pages still run from stay used (in_used) */
  }
  if (rv != OK) return(rv);
  if (in_writable() != OK) return(EIO);	/* may move the records */
  in_old(BINDING(&s), (struct inrec *) NULL);
  return(OK);
}

/*===========================================================================*
 *				in_info					     *
 *===========================================================================*/
PUBLIC void in_info(entries, used)
int *entries;
unsigned long *used;
{
  struct inrec *r, *q;
  int i, j;

  *entries = 0;
  *used = 0;
  if (in_st == 0 || in_dir > 1) return;
  for (i = 0; i < NREC; i++) {
	r = RECP(in_dir, i);
	if (FREE_SLOT(r)) break;
	if (!LIVE(r)) continue;
	(*entries)++;
	for (j = 0; j < i; j++) {	/* a text shared with an alias: once */
		q = RECP(in_dir, j);
		if (LIVE(q) && q->addr == r->addr) break;
	}
	if (j == i) *used += (r->size + PAGE - 1) / PAGE * PAGE;
  }
}

#endif /* CHIP == ARM_CM */
