/* Process shadowing: fork() without an MMU (as on the Atari ST).
 *
 * Without an MMU a child cannot be moved to new memory after fork(): its
 * stack and data hold absolute pointers.  So parent and child keep the
 * *same* addresses.  Only one member of such a group of processes can have
 * its image at those addresses at any time - the resident one.  Every other
 * member's data+stack image is kept in a private buffer of the same size,
 * its shadow (p_shadow, allocated by MM at fork time).  The text segment is
 * never copied; it is shared by the whole group.
 *
 * A shadowed process that becomes runnable is put on SHADOW_Q instead of
 * USER_Q.  The clock task calls unshadow() on it, which exchanges its image
 * with that of the resident member.  Message copies and kernel I/O to a
 * shadowed process are redirected to its shadow by cp_mess(), umap() and
 * proc_vir2phys().
 *
 * A group is identified by the click address of its data segment.
 *
 * mkshadow()  - from SYS_FORK: the child shares the parent's addresses
 *               (vfork: without a shadow, the parent waits).
 * unshadow()  - from the clock task: make a shadowed process resident,
 *               once the resident one has had its turn (#429).
 * rmshadow()  - from SYS_XIT and SYS_FRESH (exec): the process leaves its
 *               group; returns the memory MM must now free.
 */
#include "../kernel.h"
#include "../proc.h"
#include <string.h>

FORWARD _PROTOTYPE( phys_clicks image_clicks, (struct proc *p)		);
_PROTOTYPE( void fp_save, (struct proc *rp)				);
_PROTOTYPE( void flipwords, (u32_t *a, u32_t *b, size_t bytes)		);
FORWARD _PROTOTYPE( struct proc *other_member, (struct proc *p,
							int resident)	);

/*===========================================================================*
 *			copyclicks, zeroclicks, flipclicks		     *
 *===========================================================================*/
/* Click-sized memory primitives (klib68k.s on the Atari).  Clicks are word
 * aligned, so whole words are moved. */

PUBLIC void copyclicks(src, dest, nclicks)
phys_clicks src, dest, nclicks;
{
  memmove((void *) ((phys_bytes) dest << CLICK_SHIFT),
	  (void *) ((phys_bytes) src << CLICK_SHIFT),
	  (size_t) nclicks << CLICK_SHIFT);
}

PUBLIC void zeroclicks(dest, nclicks)
phys_clicks dest, nclicks;
{
  memset((void *) ((phys_bytes) dest << CLICK_SHIFT), 0,
	 (size_t) nclicks << CLICK_SHIFT);
}

PUBLIC void flipclicks(c1, c2, n)
phys_clicks c1, c2, n;
{
/* Exchange two non-overlapping click ranges (arm/flip.S: LDM/STM). */
  flipwords((u32_t *) ((phys_bytes) c1 << CLICK_SHIFT),
	    (u32_t *) ((phys_bytes) c2 << CLICK_SHIFT),
	    (size_t) n << CLICK_SHIFT);
}


/*===========================================================================*
 *				image_clicks				     *
 *===========================================================================*/
PRIVATE phys_clicks image_clicks(p)
struct proc *p;
{
/* Size of the part of p's image that is shadowed: data, gap and stack. */
  return p->p_map[S].mem_phys + p->p_map[S].mem_len - p->p_map[D].mem_phys;
}


/*===========================================================================*
 *				other_member				     *
 *===========================================================================*/
PRIVATE struct proc *other_member(p, resident)
struct proc *p;
int resident;			/* look for the resident one? */
{
/* Find another live user process at the same addresses as p: the resident
 * one, or any shadowed one.
 */
  struct proc *q;

  for (q = BEG_USER_ADDR; q < END_PROC_ADDR; q++) {
	/* a swapped process's addresses are not its own now (R12.4) */
	if (q == p || (q->p_flags & (P_SLOT_FREE | P_SWAPPED))) continue;
	if (q->p_map[D].mem_phys != p->p_map[D].mem_phys) continue;
	if (resident ? q->p_shadow == 0 : q->p_shadow != 0) return q;
  }
  return NIL_PROC;
}


/*===========================================================================*
 *				mkshadow				     *
 *===========================================================================*/
PUBLIC void mkshadow(parent, child, c2)
struct proc *parent;		/* the process that forked */
struct proc *child;		/* its new child, a copy of the parent slot */
phys_clicks c2;			/* fresh memory from MM, image_clicks() big */
{
/* Give the new child the parent's addresses.  The child runs first: it
 * takes over the resident image and the parent moves into the new memory.
 * If the parent is itself shadowed at this point (another member of its
 * group got the addresses while it waited for MM), the child starts out
 * shadowed instead, with a copy of the parent's shadow.
 */
  phys_clicks n = image_clicks(parent);

  if (c2 == 0) {		/* vfork (R2c.1): the child runs in the
				 * parent's image, the parent waits in MM; no
				 * other member (MM checked), so no swaps */
	child->p_shadow = 0;
	return;
  }
  if (parent->p_shadow == 0) {
	copyclicks(parent->p_map[D].mem_phys, c2, n);
	parent->p_shadow = c2;	/* parent is blocked in fork(): no queue */
	child->p_shadow = 0;
  } else {
	copyclicks(parent->p_shadow, c2, n);
	child->p_shadow = c2;
  }
}


/*===========================================================================*
 *				unshadow				     *
 *===========================================================================*/
PUBLIC void unshadow(p, ran)
struct proc *p;			/* a runnable, shadowed process */
struct proc *ran;		/* whose quantum just ended (NIL_PROC: none) */
{
/* Swap p's image in, and the image of the resident member of its group
 * out into p's shadow buffer.  Called by the clock task.
 */
  struct proc *r;
  int p_ready, r_ready;

  if (p->p_shadow == 0) return;
  if ((r = other_member(p, TRUE)) == NIL_PROC)
	panic("unshadow: group has no resident process", proc_number(p));
  if (p->p_physio || r->p_physio) return;	/* try again later */

  /* #429: a ready resident member keeps the addresses until it has had a
   * quantum of its own.  Swapped out before it ran (it is at the head of
   * USER_Q once sched() passed a runaway process on), p would take its
   * place at the tail, behind that runaway; at the next quantum the two
   * would trade places again, and neither would ever run: the shell and
   * its fork child (a background command not yet exec'd) next to a busy
   * loop hung the console.  Swapping only after r's own quantum (r ==
   * ran, now at the tail) gives each member every other turn of the
   * group; a blocked r is swapped out at once as before.
   */
  if (r->p_flags == 0 && r != ran) return;	/* after r's turn */

  /* Both change queues, so take them off their current queues first. */
  p_ready = (p->p_flags == 0);
  r_ready = (r->p_flags == 0);
  if (p_ready) lock_unready(p);
  if (r_ready) lock_unready(r);

  /* The FP registers of the last user process run stay in the FPU until
   * another one is entered (R5.7, fp_flush); they are saved to its stack.
   * If that is r or p, save them now, while the stack at their addresses
   * is r's: after the flip it is p's (#331: fptest's time-sliced pair). */
  if ((mpu_proc == r || mpu_proc == p)
		&& (mpu_proc->p_reg.control & CONTROL_FPLIVE)) {
	lock();
	fp_save(mpu_proc);
	unlock();
  }

#if LATTRACE
  lat_flip(0, 0UL);
#endif
  flipclicks(p->p_map[D].mem_phys, p->p_shadow, image_clicks(p));
#if LATTRACE
  lat_flip(1, (unsigned long) image_clicks(p) << CLICK_SHIFT);
#endif
  r->p_shadow = p->p_shadow;
  p->p_shadow = 0;
  p->p_nflips++;

  if (r_ready) {
	/* r was preempted: it waits behind the other ready members */
	lock_ready(r);
	lock();
	if (rdy_head[SHADOW_Q] == r && r->p_nextready != NIL_PROC) {
		rdy_head[SHADOW_Q] = r->p_nextready;
		rdy_tail[SHADOW_Q]->p_nextready = r;
		rdy_tail[SHADOW_Q] = r;
		r->p_nextready = NIL_PROC;
	}
	unlock();
  }
  if (p_ready) {
	/* #331: p takes r's turn at the tail of USER_Q.  ready() puts a user
	 * process at the head, so every process that waited there (the
	 * launcher with a key to draw, a parent back from fork()) would wait
	 * again. */
	lock_ready(p);
	lock_sched();
  }
}


/*===========================================================================*
 *				rmshadow				     *
 *===========================================================================*/
PUBLIC void rmshadow(p, basep, sizep)
struct proc *p;			/* process that exits or execs */
phys_clicks *basep, *sizep;	/* memory for MM to free */
{
/* Process p gives up its image.  If it is shadowed, its shadow buffer is
 * freed.  If it is resident and the group has other members, one of them
 * moves into the addresses and its shadow buffer is freed.  If it was the
 * last member, the whole image is freed, including the text if it lies
 * just below the data (as exec lays it out).
 */
  struct proc *q;
  phys_clicks n = image_clicks(p);
  int q_ready;

  *basep = *sizep = 0;
  if (p->p_shadow != 0) {
	*basep = p->p_shadow;
	*sizep = n;
	p->p_shadow = 0;
	return;
  }

  if ((q = other_member(p, FALSE)) != NIL_PROC) {
	q_ready = (q->p_flags == 0);
	if (q_ready) lock_unready(q);
	copyclicks(q->p_shadow, p->p_map[D].mem_phys, n);
	*basep = q->p_shadow;
	*sizep = n;
	q->p_shadow = 0;
	if (q_ready) lock_ready(q);
	return;
  }

  /* Last member of the group. */
  if (p->p_map[T].mem_len != 0 && p->p_map[T].mem_phys
		+ p->p_map[T].mem_len == p->p_map[D].mem_phys) {
	*basep = p->p_map[T].mem_phys;
	*sizep = p->p_map[T].mem_len + n;
  } else {
	*basep = p->p_map[D].mem_phys;
	*sizep = n;
  }
}
