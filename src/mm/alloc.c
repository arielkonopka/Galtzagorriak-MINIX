/* This file is concerned with allocating and freeing arbitrary-size blocks of
 * physical memory on behalf of the FORK and EXEC system calls.  The key data
 * structure used is the hole table, which maintains a list of holes in memory.
 * It is kept sorted in order of increasing memory address. The addresses
 * it contains refer to physical memory, starting at absolute address 0
 * (i.e., they are not relative to the start of MM).  During system
 * initialization, that part of memory containing the interrupt vectors,
 * kernel, and MM are "allocated" to mark them as not available and to
 * remove them from the hole list.
 *
 * The entry points into this file are:
 *   alloc_mem:	allocate a given sized chunk of memory
 *   free_mem:	release a previously allocated chunk of memory
 *   mem_init:	initialize the tables when MM start up
 *   max_hole:	returns the largest hole currently available
 */

#include "mm.h"
#include <minix/com.h>
#include <signal.h>
#include "mproc.h"

#if (MACHINE == FLIPPER)
#define NR_HOLES          32	/* max # entries in hole table */
#else
#define NR_HOLES         128	/* max # entries in hole table */
#endif
#define NIL_HOLE (struct hole *) 0

PRIVATE struct hole {
  phys_clicks h_base;		/* where does the hole begin? */
  phys_clicks h_len;		/* how big is the hole? */
  struct hole *h_next;		/* pointer to next entry on the list */
} hole[NR_HOLES];


PRIVATE struct hole *hole_head;	/* pointer to first hole */
PRIVATE struct hole *free_slots;	/* ptr to list of unused table slots */

FORWARD _PROTOTYPE( void del_slot, (struct hole *prev_ptr, struct hole *hp) );
FORWARD _PROTOTYPE( void merge, (struct hole *hp)			    );


/*===========================================================================*
 *				alloc_mem				     *
 *===========================================================================*/
PUBLIC phys_clicks alloc_mem(clicks)
phys_clicks clicks;		/* amount of memory requested */
{
/* Allocate a block of memory from the free list using first fit. The block
 * consists of a sequence of contiguous bytes, whose length in clicks is
 * given by 'clicks'.  A pointer to the block is returned.  The block is
 * always on a click boundary.  This procedure is called when memory is
 * needed for FORK or EXEC.
 */

  register struct hole *hp, *prev_ptr;
  phys_clicks old_base;

  hp = hole_head;
  while (hp != NIL_HOLE) {
	if (hp->h_len >= clicks) {
		/* We found a hole that is big enough.  Use it. */
		old_base = hp->h_base;	/* remember where it started */
		hp->h_base += clicks;	/* bite a piece off */
		hp->h_len -= clicks;	/* ditto */

		/* If hole is only partly used, reduce size and return. */
		if (hp->h_len != 0) return(old_base);

		/* The entire hole has been used up.  Manipulate free list. */
		del_slot(prev_ptr, hp);
		return(old_base);
	}

	prev_ptr = hp;
	hp = hp->h_next;
  }
  return(NO_MEM);
}


/*===========================================================================*
 *				free_mem				     *
 *===========================================================================*/
PUBLIC void free_mem(base, clicks)
phys_clicks base;		/* base address of block to free */
phys_clicks clicks;		/* number of clicks to free */
{
/* Return a block of free memory to the hole list.  The parameters tell where
 * the block starts in physical memory and how big it is.  The block is added
 * to the hole list.  If it is contiguous with an existing hole on either end,
 * it is merged with the hole or holes.
 */

  register struct hole *hp, *new_ptr, *prev_ptr;

  if (clicks == 0) return;
  if ( (new_ptr = free_slots) == NIL_HOLE) panic("Hole table full", NO_NUM);
  new_ptr->h_base = base;
  new_ptr->h_len = clicks;
  free_slots = new_ptr->h_next;
  hp = hole_head;

  /* If this block's address is numerically less than the lowest hole currently
   * available, or if no holes are currently available, put this hole on the
   * front of the hole list.
   */
  if (hp == NIL_HOLE || base <= hp->h_base) {
	/* Block to be freed goes on front of the hole list. */
	new_ptr->h_next = hp;
	hole_head = new_ptr;
	merge(new_ptr);
	return;
  }

  /* Block to be returned does not go on front of hole list. */
  while (hp != NIL_HOLE && base > hp->h_base) {
	prev_ptr = hp;
	hp = hp->h_next;
  }

  /* We found where it goes.  Insert block after 'prev_ptr'. */
  new_ptr->h_next = prev_ptr->h_next;
  prev_ptr->h_next = new_ptr;
  merge(prev_ptr);		/* sequence is 'prev_ptr', 'new_ptr', 'hp' */
}


/*===========================================================================*
 *				del_slot				     *
 *===========================================================================*/
PRIVATE void del_slot(prev_ptr, hp)
register struct hole *prev_ptr;	/* pointer to hole entry just ahead of 'hp' */
register struct hole *hp;	/* pointer to hole entry to be removed */
{
/* Remove an entry from the hole list.  This procedure is called when a
 * request to allocate memory removes a hole in its entirety, thus reducing
 * the numbers of holes in memory, and requiring the elimination of one
 * entry in the hole list.
 */

  if (hp == hole_head)
	hole_head = hp->h_next;
  else
	prev_ptr->h_next = hp->h_next;

  hp->h_next = free_slots;
  free_slots = hp;
}


/*===========================================================================*
 *				merge					     *
 *===========================================================================*/
PRIVATE void merge(hp)
register struct hole *hp;	/* ptr to hole to merge with its successors */
{
/* Check for contiguous holes and merge any found.  Contiguous holes can occur
 * when a block of memory is freed, and it happens to abut another hole on
 * either or both ends.  The pointer 'hp' points to the first of a series of
 * three holes that can potentially all be merged together.
 */

  register struct hole *next_ptr;

  /* If 'hp' points to the last hole, no merging is possible.  If it does not,
   * try to absorb its successor into it and free the successor's table entry.
   */
  if ( (next_ptr = hp->h_next) == NIL_HOLE) return;
  if (hp->h_base + hp->h_len == next_ptr->h_base) {
	hp->h_len += next_ptr->h_len;	/* first one gets second one's mem */
	del_slot(hp, next_ptr);
  } else {
	hp = next_ptr;
  }

  /* If 'hp' now points to the last hole, return; otherwise, try to absorb its
   * successor into it.
   */
  if ( (next_ptr = hp->h_next) == NIL_HOLE) return;
  if (hp->h_base + hp->h_len == next_ptr->h_base) {
	hp->h_len += next_ptr->h_len;
	del_slot(hp, next_ptr);
  }
}


/*===========================================================================*
 *				max_hole				     *
 *===========================================================================*/
PUBLIC phys_clicks max_hole()
{
/* Scan the hole list and return the largest hole. */

  register struct hole *hp;
  register phys_clicks max;

  hp = hole_head;
  max = 0;
  while (hp != NIL_HOLE) {
	if (hp->h_len > max) max = hp->h_len;
	hp = hp->h_next;
  }
  return(max);
}


/*===========================================================================*
 *				mem_init				     *
 *===========================================================================*/
PUBLIC void mem_init(total, free)
phys_clicks *total, *free;		/* memory size summaries */
{
/* Initialize hole lists.  There are two lists: 'hole_head' points to a linked
 * list of all the holes (unused memory) in the system; 'free_slots' points to
 * a linked list of table entries that are not in use.  Initially, the former
 * list has one entry for each chunk of physical memory, and the second
 * list links together the remaining table slots.  As memory becomes more
 * fragmented in the course of time (i.e., the initial big holes break up into
 * smaller holes), new table slots are needed to represent them.  These slots
 * are taken from the list headed by 'free_slots'.
 */

  register struct hole *hp;
  phys_clicks base;		/* base address of chunk */
  phys_clicks size;		/* size of chunk */
  message mess;

  /* Put all holes on the free list. */
  for (hp = &hole[0]; hp < &hole[NR_HOLES]; hp++) hp->h_next = hp + 1;
  hole[NR_HOLES-1].h_next = NIL_HOLE;
  hole_head = NIL_HOLE;
  free_slots = &hole[0];

  /* Ask the kernel for chunks of physical memory and allocate a hole for
   * each of them.  The SYS_MEM call responds with the base and size of the
   * next chunk and the total amount of memory.
   */
  *free = 0;
  for (;;) {
	mess.m_type = SYS_MEM;
	if (sendrec(SYSTASK, &mess) != OK) panic("bad SYS_MEM?", NO_NUM);
	base = mess.m1_i1;
	size = mess.m1_i2;
	if (size == 0) break;		/* no more? */

	free_mem(base, size);
	*total = mess.m1_i3;
	*free += size;
  }
}


/*===========================================================================*
 *				alloc_mem_high				     *
 *===========================================================================*/
PUBLIC phys_clicks alloc_mem_high(clicks)
phys_clicks clicks;		/* amount of memory requested */
{
/* Like alloc_mem(), but take the block from the top end of the highest hole
 * that is big enough.  Without an MMU nothing can be moved later, so the
 * placement matters: exec puts program images at the top, where the long-
 * lived ones (launcher, shells, daemons) pile up, while fork's temporary
 * shadow copies come from the bottom (alloc_mem).  The free space then
 * stays together instead of being cut into pieces too small to run
 * anything.
 */
  register struct hole *hp, *prev_ptr, *best, *best_prev;

  best = best_prev = prev_ptr = NIL_HOLE;
  for (hp = hole_head; hp != NIL_HOLE; prev_ptr = hp, hp = hp->h_next) {
	if (hp->h_len >= clicks) {	/* the list is sorted by address */
		best = hp;
		best_prev = prev_ptr;
	}
  }
  if (best == NIL_HOLE) return(NO_MEM);
  best->h_len -= clicks;
  if (best->h_len == 0) {
	phys_clicks base = best->h_base;

	del_slot(best_prev, best);
	return(base);
  }
  return(best->h_base + best->h_len);
}


/*===========================================================================*
 *				do_meminfo				     *
 *===========================================================================*/
PUBLIC int do_meminfo()
{
/* Report free memory: the reply is the total free in bytes, reply_i1 the
 * largest hole in bytes.  Without an MMU
 * a program needs one contiguous hole, so the largest one is what decides
 * whether it can be started.
 */
  register struct hole *hp;
  phys_clicks total = 0, max = 0;

  phys_bytes list[2 * 16];		/* (base, len) pairs, in bytes */
  int n = 0;

#if (CHIP == ARM_CM)
  /* m1_i2 = MI_KSTACKS: the kernel task stack table instead. */
  if (mm_in.m1_i2 == MI_KSTACKS)
	return(sys_kstacks(who, mm_in.m1_p1, mm_in.m1_i1));
  /* m1_i2 = MI_FCACHE: the flash text cache summary to m1_p1. */
  if (mm_in.m1_i2 == MI_FCACHE) {
	struct fcinfo fi;
	fc_info(&fi);
	return(sys_copy(MM_PROC_NR, D, (phys_bytes) &fi, who, D,
		(phys_bytes) mm_in.m1_p1, (phys_bytes) sizeof(fi)));
  }
  /* m1_i2 = MI_TRACE: switch the memory use trace on or off (root). */
  if (mm_in.m1_i2 == MI_TRACE) {
	if (mp->mp_effuid != SUPER_USER) return(EPERM);
	mem_trace = mm_in.m1_i1;
	return(OK);
  }
#endif

  for (hp = hole_head; hp != NIL_HOLE; hp = hp->h_next) {
	total += hp->h_len;
	if (hp->h_len > max) max = hp->h_len;
	if (n < 16) {
		list[2 * n] = (phys_bytes) hp->h_base << CLICK_SHIFT;
		list[2 * n + 1] = (phys_bytes) hp->h_len << CLICK_SHIFT;
		n++;
	}
  }
  /* Optionally the holes themselves: m1_p1 = buffer, m1_i1 = its size. */
  if (mm_in.m1_p1 != NULL && mm_in.m1_i1 >= (int) sizeof(list[0]) * 2) {
	if (n * 2 * (int) sizeof(list[0]) > mm_in.m1_i1)
		n = mm_in.m1_i1 / (2 * (int) sizeof(list[0]));
	if (n < 16) list[2 * n] = list[2 * n + 1] = 0;	/* end mark */
	sys_copy(MM_PROC_NR, D, (phys_bytes) list, who, D,
		(phys_bytes) mm_in.m1_p1,
		(phys_bytes) (n < 16 ? n + 1 : n) * 2 * sizeof(list[0]));
  }
  result2 = (int) ((phys_bytes) max << CLICK_SHIFT);
  return((int) ((phys_bytes) total << CLICK_SHIFT));
}


#if (CHIP == ARM_CM)
/* Memory use trace ("free -t on"): exec fills the gap and stack of a new
 * image with a pattern, exit reports how much of it was ever touched.
 * The largest untouched run between the heap and the stack is what chmem
 * could have done without.  MM runs privileged with a flat map, so it
 * reads and writes the image directly.
 */
#define PAINT	0xDEADBEEFUL

/*===========================================================================*
 *				mem_paint				     *
 *===========================================================================*/
PUBLIC void mem_paint(rmp)
struct mproc *rmp;
{
  unsigned long *w, *end;

  w = (unsigned long *) ((rmp->mp_seg[D].mem_phys + rmp->mp_seg[D].mem_len)
							<< CLICK_SHIFT);
  end = (unsigned long *) ((rmp->mp_seg[S].mem_phys + rmp->mp_seg[S].mem_len)
							<< CLICK_SHIFT);
  while (w < end) *w++ = PAINT;
}

/*===========================================================================*
 *				mem_report				     *
 *===========================================================================*/
PUBLIC void mem_report(rmp)
struct mproc *rmp;
{
  unsigned long *w, *end, *run;
  unsigned long best = 0, total;

  w = (unsigned long *) ((rmp->mp_seg[D].mem_phys + rmp->mp_seg[D].mem_len)
							<< CLICK_SHIFT);
  end = (unsigned long *) ((rmp->mp_seg[S].mem_phys + rmp->mp_seg[S].mem_len)
							<< CLICK_SHIFT);
  total = (end - w) * sizeof(*w);
  while (w < end) {
	if (*w != PAINT) { w++; continue; }
	run = w;
	while (w < end && *w == PAINT) w++;
	if ((unsigned long) (w - run) > best) best = w - run;
  }
  best *= sizeof(*w);
  /* Heap given out by brk() is below the D end now and counts as used;
   * the untouched run is what chmem could be lowered by.
   */
  printf("memuse: %-8.8s %5lu bytes never used (gap+stack now %lu)\n",
	rmp->mp_name, best, total);
}
#endif
