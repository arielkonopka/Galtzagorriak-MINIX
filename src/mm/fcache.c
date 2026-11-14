/* Flash text cache (Galtzagorriak MINIX, ARM Cortex-M).
 *
 * Position-independent programs (A_SEP, see exec.c) have text without a
 * single absolute address, so it can run in place from the internal flash
 * and be shared by every process running it.  Their text is copied into a
 * region of flash the first time they are executed; later executions find
 * it there by the CRC-32 and size of the text (a_syms, a_text) and the
 * owner of the program file, and only data, bss and stack take RAM.
 *
 * The CRC comes from the file header and proves nothing (S12): a file
 * could claim another program's CRC.  With the owner in the key, a forged
 * header can only alias text among one owner's own files, who controls
 * them all anyway; and exec.c caches only files nobody but their owner can
 * write.  So the text found for /bin/su was put there from a root file.
 *
 * Layout of the region (the board decides where, kernel call FL_INFO):
 *   page 0, 1	directories; the valid one with the higher generation is
 *		active.  Header (8 bytes): magic, generation.  Then records
 *		of 32 bytes = four double words, written once each:
 *		  crc, size	- all ones: free slot
 *		  addr, seq	- where the text is, insertion counter
 *		  owner, flags	- uid of the program file; flags unused
 *		  dead		- all ones: live; programmed 0: removed
 *   page 2..	text, whole pages per program.
 * Flash bits only go from 1 to 0 without an erase, hence the dead word.
 * When the directory is full, the live records are copied to the other
 * directory page with the next generation, and the old one is erased.
 * A record is written only after its text: a run that dies half way
 * leaves pages that no record claims, and they are simply reused.
 *
 * When there is no room, records of programs that no process is running
 * are removed, oldest first.  If that is not enough the text goes to RAM
 * as before.
 */
#include "mm.h"
#include <minix/com.h>
#include <signal.h>
#include <string.h>
#include "mproc.h"

#if (CHIP == ARM_CM)

#define FC_MAGIC	0x32585447UL	/* "GTX2": 32-byte records with owner;
					 * older directories are reformatted */
#define PAGE		4096UL
#define HDR		8
#define REC		32
#define NREC		((PAGE - HDR) / REC)
#define DIRPAGES	2
#define MAXPAGES	128		/* bitmap size: up to 512 KB */

struct fcrec {
  unsigned long crc, size;
  unsigned long addr, seq;
  unsigned long owner, flags;
  unsigned long dead0, dead1;
};

PRIVATE unsigned long fc_base, fc_size;	/* region; 0: no cache */
PRIVATE int fc_dir;			/* active directory page (0, 1) */
PRIVATE unsigned long fc_gen, fc_seq;
PRIVATE unsigned char used[MAXPAGES / 8];

#define DIRADDR(d)	(fc_base + (d) * PAGE)
#define RECP(d, i)	((struct fcrec *) (DIRADDR(d) + HDR + (i) * REC))
#define FREE_SLOT(r)	((r)->crc == 0xFFFFFFFFUL && (r)->size == 0xFFFFFFFFUL)
#define LIVE(r)		(!FREE_SLOT(r) && (r)->dead0 == 0xFFFFFFFFUL)

FORWARD _PROTOTYPE( int fc_format, (int d, unsigned long gen)		);
FORWARD _PROTOTYPE( int fc_compact, (void)				);
FORWARD _PROTOTYPE( int fc_running, (unsigned long addr)		);
FORWARD _PROTOTYPE( void fc_map, (void)					);
FORWARD _PROTOTYPE( unsigned long fc_find_run, (unsigned long npages)	);

/*===========================================================================*
 *				fc_format				     *
 *===========================================================================*/
PRIVATE int fc_format(d, gen)
int d;
unsigned long gen;
{
/* Erase directory page d and give it a header. */
  unsigned long h[2];

  if (sys_flash(FL_ERASE, DIRADDR(d), NULL, 0, NULL, NULL) != OK) return(-1);
  h[0] = FC_MAGIC;
  h[1] = gen;
  return(sys_flash(FL_WRITE, DIRADDR(d), h, sizeof(h), NULL, NULL));
}

/*===========================================================================*
 *				fc_init					     *
 *===========================================================================*/
PUBLIC void fc_init()
{
/* Find the cache region and its active directory; start one if none. */
  unsigned long *h0, *h1, base, size;
  struct fcrec *r;
  int i, ok0, ok1;

  if (sys_flash(FL_INFO, 0, NULL, 0, &base, &size) != OK) return;
  {	/* the layout, once (R2b.1) */
	unsigned long sfsa, ib, is, z;

	if (sys_flash(FL_INFO, FL_LAYOUT_SFSA, NULL, 0, &sfsa, &z) == OK
	    && sys_flash(FL_INFO, FL_LAYOUT_INST, NULL, 0, &ib, &is) == OK)
		printf("flash: CPU2 from 0x%lx; programs 0x%lx %lu KB, "
			"cache 0x%lx %lu KB\n", sfsa, ib, is >> 10,
			base, size >> 10);
  }
  if (size < (DIRPAGES + 1) * PAGE) return;
  if (size > MAXPAGES * PAGE) size = MAXPAGES * PAGE;
  fc_base = base;
  fc_size = size;

  h0 = (unsigned long *) DIRADDR(0);
  h1 = (unsigned long *) DIRADDR(1);
  ok0 = h0[0] == FC_MAGIC && h0[1] != 0xFFFFFFFFUL;
  ok1 = h1[0] == FC_MAGIC && h1[1] != 0xFFFFFFFFUL;
  if (ok0 && ok1) fc_dir = h1[1] > h0[1];
  else if (ok0 || ok1) fc_dir = ok1;
  else {
	if (fc_format(0, 1) != OK) { fc_base = fc_size = 0; return; }
	fc_dir = 0;
  }
  fc_gen = ((unsigned long *) DIRADDR(fc_dir))[1];
  for (i = 0; i < NREC; i++) {
	r = RECP(fc_dir, i);
	if (!FREE_SLOT(r) && r->seq != 0xFFFFFFFFUL && r->seq >= fc_seq)
		fc_seq = r->seq + 1;
  }
}

/*===========================================================================*
 *				fc_lookup				     *
 *===========================================================================*/
PUBLIC unsigned long fc_lookup(crc, size, owner)
unsigned long crc, size;
unsigned long owner;		/* uid of the program file */
{
/* The flash address of a cached text, or 0. */
  struct fcrec *r;
  int i;

  if (fc_size == 0 || size == 0) return(0);
  for (i = 0; i < NREC; i++) {
	r = RECP(fc_dir, i);
	if (FREE_SLOT(r)) break;	/* slots are used in order */
	if (LIVE(r) && r->crc == crc && r->size == size && r->owner == owner)
		return(r->addr);
  }
  return(0);
}

/*===========================================================================*
 *				fc_running				     *
 *===========================================================================*/
PRIVATE int fc_running(addr)
unsigned long addr;
{
/* Is a process running (or shadowed, or about to run) from this text? */
  struct mproc *rmp;

  for (rmp = &mproc[0]; rmp < &mproc[NR_PROCS]; rmp++) {
	if (!(rmp->mp_flags & IN_USE)) continue;
	if (((unsigned long) rmp->mp_seg[T].mem_phys << CLICK_SHIFT) == addr)
		return(TRUE);
  }
  return(FALSE);
}

/*===========================================================================*
 *				fc_map					     *
 *===========================================================================*/
PRIVATE void fc_map()
{
/* Mark the text pages that live records occupy. */
  struct fcrec *r;
  unsigned long p, n;
  int i;

  memset(used, 0, sizeof(used));
  for (p = 0; p < DIRPAGES; p++) used[p / 8] |= 1 << (p % 8);
  for (i = 0; i < NREC; i++) {
	r = RECP(fc_dir, i);
	if (FREE_SLOT(r)) break;
	if (!LIVE(r)) continue;
	p = (r->addr - fc_base) / PAGE;
	for (n = (r->size + PAGE - 1) / PAGE; n > 0; n--, p++)
		if (p < MAXPAGES) used[p / 8] |= 1 << (p % 8);
  }
}

/*===========================================================================*
 *				fc_find_run				     *
 *===========================================================================*/
PRIVATE unsigned long fc_find_run(npages)
unsigned long npages;
{
/* First fit: the address of npages free pages in a row, or 0. */
  unsigned long p, run = 0, total = fc_size / PAGE;

  for (p = DIRPAGES; p < total; p++) {
	if (used[p / 8] & (1 << (p % 8))) { run = 0; continue; }
	if (++run == npages) return(fc_base + (p + 1 - npages) * PAGE);
  }
  return(0);
}

/*===========================================================================*
 *				fc_compact				     *
 *===========================================================================*/
PRIVATE int fc_compact()
{
/* Copy the live records to the other directory page; it becomes active. */
  int i, j, d = !fc_dir;
  struct fcrec *r;
  unsigned long rec[6];

  if (fc_format(d, fc_gen + 1) != OK) return(-1);
  for (i = j = 0; i < NREC; i++) {
	r = RECP(fc_dir, i);
	if (FREE_SLOT(r)) break;
	if (!LIVE(r)) continue;
	rec[0] = r->crc; rec[1] = r->size; rec[2] = r->addr; rec[3] = r->seq;
	rec[4] = r->owner; rec[5] = r->flags;
	if (sys_flash(FL_WRITE, (unsigned long) RECP(d, j), rec, sizeof(rec),
						NULL, NULL) != OK) return(-1);
	j++;
  }
  (void) sys_flash(FL_ERASE, DIRADDR(fc_dir), NULL, 0, NULL, NULL);
  fc_dir = d;
  fc_gen++;
  return(OK);
}

/*===========================================================================*
 *				fc_reserve				     *
 *===========================================================================*/
PUBLIC unsigned long fc_reserve(size)
unsigned long size;
{
/* Find erased pages for a text of size bytes, making room by removing
 * records nobody runs, oldest first.  Returns the address, or 0 (then the
 * text goes to RAM).
 */
  unsigned long npages = (size + PAGE - 1) / PAGE, addr, page, zero[2];
  struct fcrec *r, *old;
  int i;

  if (fc_size == 0 || size == 0 || npages > fc_size / PAGE - DIRPAGES)
	return(0);

  for (;;) {
	fc_map();
	if ((addr = fc_find_run(npages)) != 0) break;
	/* Remove the oldest record that no process uses. */
	old = NULL;
	for (i = 0; i < NREC; i++) {
		r = RECP(fc_dir, i);
		if (FREE_SLOT(r)) break;
		if (!LIVE(r) || fc_running(r->addr)) continue;
		if (old == NULL || r->seq < old->seq) old = r;
	}
	if (old == NULL) return(0);
	zero[0] = zero[1] = 0;
	if (sys_flash(FL_WRITE, (unsigned long) &old->dead0, zero,
					sizeof(zero), NULL, NULL) != OK)
		return(0);
  }

  /* A free directory slot is needed too. */
  if (!FREE_SLOT(RECP(fc_dir, NREC - 1))) {
	if (fc_compact() != OK || !FREE_SLOT(RECP(fc_dir, NREC - 1)))
		return(0);
  }

  for (page = 0; page < npages; page++) {
	if (sys_flash(FL_ERASE, addr + page * PAGE, NULL, 0, NULL, NULL)
								!= OK)
		return(0);
  }
  return(addr);
}

/*===========================================================================*
 *				fc_fill					     *
 *===========================================================================*/
PUBLIC int fc_fill(fd, addr, size, crc, owner, buf, bufsize)
int fd;				/* program file, at the start of the text */
unsigned long addr, size, crc;	/* where the text goes, its size and CRC */
unsigned long owner;		/* uid of the program file */
char *buf;			/* MM buffer to read through */
int bufsize;			/* its size, a multiple of 8 */
{
/* Copy the text from the file to the reserved pages and record it. */
  unsigned long done = 0, rec[6];
  int n, k, i;

  while (done < size) {
	n = size - done < (unsigned long) bufsize ? (int) (size - done) : bufsize;
	if (read(fd, buf, n) != n) return(-1);
	k = (n + 7) & ~7;
	for (i = n; i < k; i++) buf[i] = (char) 0xFF;
	if (sys_flash(FL_WRITE, addr + done, buf, (unsigned) k, NULL, NULL)
								!= OK)
		return(-1);
	done += n;
  }
  for (i = 0; i < NREC; i++) {
	if (FREE_SLOT(RECP(fc_dir, i))) break;
  }
  if (i == NREC) return(-1);
  rec[0] = crc; rec[1] = size; rec[2] = addr; rec[3] = fc_seq++;
  rec[4] = owner; rec[5] = 0xFFFFFFFFUL;
  return(sys_flash(FL_WRITE, (unsigned long) RECP(fc_dir, i), rec,
						sizeof(rec), NULL, NULL));
}

/*===========================================================================*
 *				fc_info					     *
 *===========================================================================*/
PUBLIC void fc_info(fi)
struct fcinfo *fi;
{
/* Summary for MEMINFO (free -v). */
  struct fcrec *r;
  int i;

  unsigned long b, n;

  memset(fi, 0, sizeof(*fi));
  if (sys_flash(FL_INFO, FL_LAYOUT_INST, NULL, 0, &b, &n) == OK) {
	fi->inst_base = b;
	fi->inst_size = n;
  }
  if (sys_flash(FL_INFO, FL_LAYOUT_SFSA, NULL, 0, &b, &n) == OK)
	fi->sfsa = b;
  fi->cache_base = fc_base;
  if (fc_size == 0) return;
  fi->size = fc_size - DIRPAGES * PAGE;
  for (i = 0; i < NREC; i++) {
	r = RECP(fc_dir, i);
	if (FREE_SLOT(r)) break;
	if (!LIVE(r)) continue;
	fi->entries++;
	fi->used += (r->size + PAGE - 1) / PAGE * PAGE;
	if (fc_running(r->addr)) fi->running++;
  }
}

#endif /* CHIP == ARM_CM */
