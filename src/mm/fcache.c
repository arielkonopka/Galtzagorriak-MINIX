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
 * are removed, oldest first.
 *
 * All writing is idle work (spec R0.8c): exec only looks programs up.  A
 * program that is not cached runs with its text in RAM, and fc_queue()
 * notes it; MM then copies that RAM text to the cache in small steps, one
 * per synchronous alarm (fc_step(): an erase of one page, or 256 bytes of
 * programming, or one directory record), so the scheduler is free in
 * between.  If the program exits first, the copy is dropped - its pages
 * are not claimed by any record and get reused.  Formatting a fresh
 * directory at boot is such a step too.
 */
#include "mm.h"
#include <minix/com.h>
#include <signal.h>
#include <string.h>
#include "mproc.h"

extern char mbuf[];		/* exec.c, free between execs */

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

#define STEP_TICKS	1		/* clock ticks between steps */
#define STEP_BYTES	256		/* programmed per step (~2.7 ms) */
#define START_TICKS	(5 * HZ)	/* first step after boot */

/* The one copy in progress, if any. */
enum { J_IDLE, J_RESERVE, J_ERASE, J_PROGRAM, J_RECORD, J_COMPACT,
	J_COMPACT_COPY, J_COMPACT_DROP };
PRIVATE struct {			/* packed: MM's static RAM (rule 0.1) */
  unsigned char state;
  unsigned char slot;		/* the process whose RAM text is copied */
  unsigned char unformatted;	/* no valid directory yet */
  unsigned char page;		/* first reserved page; compaction: next
				 * record (they are never needed together) */
  unsigned short pid, owner;	/* its pid, the file's owner (key) */
  unsigned long src, size, crc;	/* its text, key */
  unsigned long done;		/* bytes erased/programmed, records */
} job;
#define JOB_ADDR	(fc_base + (unsigned long) job.page * PAGE)
#define fc_unformatted	job.unformatted

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
FORWARD _PROTOTYPE( int fc_reserve, (void)				);
FORWARD _PROTOTYPE( int fc_compact_step, (void)				);
FORWARD _PROTOTYPE( void fc_alarm, (int ticks)				);

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
	/* No directory: format one later, as idle work (R0.8c); until
	 * then nothing is found or cached. */
	fc_unformatted = TRUE;
	fc_dir = 0;
	fc_alarm(START_TICKS);
	return;
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

  if (fc_size == 0 || size == 0 || fc_unformatted) return(0);
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
 *				fc_queue				     *
 *===========================================================================*/
PUBLIC void fc_queue(slot, src, size, crc, owner)
int slot;			/* process just exec'd, text in RAM */
unsigned long src, size, crc, owner;
{
/* Note a program to copy to the cache.  One at a time: if a copy is going
 * on, this one is cached on a later run. */
  if (fc_size == 0 || size == 0 || job.state != J_IDLE) return;
  if ((size + PAGE - 1) / PAGE > fc_size / PAGE - DIRPAGES) return;
  job.slot = slot;
  job.pid = mproc[slot].mp_pid;
  job.src = src;
  job.size = size;
  job.crc = crc;
  job.owner = owner;
  job.state = J_RESERVE;
  if (!fc_unformatted) fc_alarm(STEP_TICKS);
}


/*===========================================================================*
 *				fc_step					     *
 *===========================================================================*/
PUBLIC void fc_step()
{
/* One piece of the cache's flash work (a synchronous alarm came in). */
  struct mproc *rmp;
  unsigned long npages, n, rec[6];
  int r = OK, i;

  if (fc_size == 0) return;
  if (fc_unformatted) {
	if (fc_format(0, 1) == OK) {
		fc_unformatted = FALSE;
		fc_dir = 0;
		fc_gen = 1;
	} else {
		fc_base = fc_size = 0;		/* no cache, then */
		return;
	}
	if (job.state != J_IDLE) fc_alarm(STEP_TICKS);
	return;
  }
  if (job.state == J_IDLE) return;

  /* The program must still be there, running from that RAM text (a
   * compaction, once started, is finished regardless). */
  rmp = &mproc[job.slot];
  if (job.state != J_COMPACT && job.state != J_COMPACT_COPY
	&& job.state != J_COMPACT_DROP
	&& (!(rmp->mp_flags & IN_USE)
	|| ((unsigned long) rmp->mp_seg[T].mem_phys << CLICK_SHIFT) != job.src
	|| (unsigned short) rmp->mp_pid != job.pid))
	r = EAGAIN;
  npages = (job.size + PAGE - 1) / PAGE;

  if (r == OK) switch (job.state) {
  case J_RESERVE:
	r = fc_reserve();
	break;
  case J_COMPACT:
  case J_COMPACT_COPY:
  case J_COMPACT_DROP:
	r = fc_compact_step();
	break;
  case J_ERASE:				/* a page per step */
	r = sys_flash(FL_ERASE, JOB_ADDR + job.done, NULL, 0, NULL, NULL);
	job.done += PAGE;
	if (job.done == npages * PAGE) {
		job.state = J_PROGRAM;
		job.done = 0;
	}
	break;
  case J_PROGRAM:			/* STEP_BYTES per step, via mbuf */
	n = job.size - job.done < STEP_BYTES ? job.size - job.done
					     : STEP_BYTES;
	r = sys_copy(job.slot, T, (phys_bytes) (job.src + job.done),
		MM_PROC_NR, D, (phys_bytes) mbuf, (phys_bytes) n);
	if (r != OK) break;
	for (i = n; i < (int) ((n + 7) & ~7); i++) mbuf[i] = (char) 0xFF;
	r = sys_flash(FL_WRITE, JOB_ADDR + job.done, mbuf,
			(unsigned) ((n + 7) & ~7), NULL, NULL);
	job.done += n;
	if (job.done == job.size) job.state = J_RECORD;
	break;
  case J_RECORD:			/* now it can be found */
	for (i = 0; i < NREC; i++)
		if (FREE_SLOT(RECP(fc_dir, i))) break;
	if (i == NREC) { r = EAGAIN; break; }
	rec[0] = job.crc; rec[1] = job.size; rec[2] = JOB_ADDR;
	rec[3] = fc_seq++; rec[4] = job.owner; rec[5] = 0xFFFFFFFFUL;
	r = sys_flash(FL_WRITE, (unsigned long) RECP(fc_dir, i), rec,
						sizeof(rec), NULL, NULL);
	job.state = J_IDLE;
	return;
  }
  if (r != OK) {			/* give up; it is cached next time */
	job.state = J_IDLE;
	return;
  }
  fc_alarm(STEP_TICKS);
}


/*===========================================================================*
 *				fc_reserve				     *
 *===========================================================================*/
PRIVATE int fc_reserve()
{
/* Find erased-to-be pages for the job, or make room: each call writes at
 * most one thing (a "dead" mark, or starts a compaction). */
  unsigned long npages = (job.size + PAGE - 1) / PAGE, zero[2], addr;
  struct fcrec *r, *old;
  int i;

  if (!FREE_SLOT(RECP(fc_dir, NREC - 1))) {	/* directory full */
	job.state = J_COMPACT;
	return(OK);
  }
  fc_map();
  if ((addr = fc_find_run(npages)) != 0) {
	job.page = (addr - fc_base) / PAGE;
	job.state = J_ERASE;
	job.done = 0;
	return(OK);
  }
  /* Remove the oldest record that no process uses. */
  old = NULL;
  for (i = 0; i < NREC; i++) {
	r = RECP(fc_dir, i);
	if (FREE_SLOT(r)) break;
	if (!LIVE(r) || fc_running(r->addr)) continue;
	if (old == NULL || r->seq < old->seq) old = r;
  }
  if (old == NULL) return(ENOSPC);
  zero[0] = zero[1] = 0;
  return(sys_flash(FL_WRITE, (unsigned long) &old->dead0, zero,
						sizeof(zero), NULL, NULL));
}


/*===========================================================================*
 *				fc_compact_step				     *
 *===========================================================================*/
PRIVATE int fc_compact_step()
{
/* Copy the live records to the other directory page, which becomes the
 * active one: erase it (a step), copy 8 records per step, erase the old. */
  int d = !fc_dir, k;
  struct fcrec *r;
  unsigned long rec[6];

  switch (job.state) {
  case J_COMPACT:
	if (fc_format(d, fc_gen + 1) != OK) return(EIO);
	job.page = 0;
	job.done = 0;			/* records written so far */
	job.state = J_COMPACT_COPY;
	return(OK);
  case J_COMPACT_COPY:
	for (k = 0; k < 8 && job.page < NREC; job.page++) {
		r = RECP(fc_dir, job.page);
		if (FREE_SLOT(r)) { job.page = NREC; break; }
		if (!LIVE(r)) continue;
		rec[0] = r->crc; rec[1] = r->size; rec[2] = r->addr;
		rec[3] = r->seq; rec[4] = r->owner; rec[5] = r->flags;
		if (sys_flash(FL_WRITE, (unsigned long) RECP(d, job.done),
				rec, sizeof(rec), NULL, NULL) != OK)
			return(EIO);
		job.done++;
		k++;
	}
	if (job.page >= NREC) job.state = J_COMPACT_DROP;
	return(OK);
  case J_COMPACT_DROP:
	(void) sys_flash(FL_ERASE, DIRADDR(fc_dir), NULL, 0, NULL, NULL);
	fc_dir = d;
	fc_gen++;
	job.state = J_RESERVE;
	return(OK);
  }
  return(EIO);
}


/*===========================================================================*
 *				fc_alarm				     *
 *===========================================================================*/
PRIVATE void fc_alarm(ticks)
int ticks;
{
/* Have SYN_ALRM_TASK send MM a CLOCK_INT in that many ticks (fc_step). */
  message m;

  m.m_type = SET_SYNC_AL;
  m.CLOCK_PROC_NR = MM_PROC_NR;
  m.DELTA_TICKS = (long) ticks;
  (void) sendrec(CLOCK, &m);
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

  memset(fi, 0, sizeof(*fi));
  if (fc_size == 0 || fc_unformatted) return;
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
