/* Latency trace (spec R0.8e).  Compiled to nothing unless LATTRACE: the
 * release kernel has no ring and no hooks (Phase 1 RAM budget 0 B). */
#include "../kernel.h"
#include "../proc.h"
#include <minix/com.h>
#include <minix/lat.h>
#include "hal.h"

#if LATTRACE
#define CYCCNT	(*(volatile unsigned long *) 0xE0001004)

PUBLIC struct latinfo lat;
PRIVATE unsigned long lock_cyc;
PRIVATE int rev_proc = -1000;
PRIVATE unsigned long rev_cyc, rev_rep;	/* REVIVE seen; FS replied */
PRIVATE unsigned long rev_fl;		/* flash_n at the reply */
PRIVATE int last_run = -1000;		/* the process entered last */
PRIVATE unsigned long last_cyc, win[4];	/* when; the open window's parts */
PRIVATE unsigned long win_big;		/* its longest slice */
PRIVATE int win_big_proc;

PRIVATE unsigned long long lcd_wanted;	/* procs whose next frame counts */
PRIVATE int lcd_after_btn;		/* the next frame after a button */

PUBLIC void lat_event(ev, arg)
int ev, arg;
{
  struct latevent *e;
  unsigned long pm;
  unsigned long long bit = 1ULL << ((arg + NR_TASKS) & 63);

  /* Only the frames the budgets need: a process's first after exec or
   * exit of another (L6, L7) and the first after a button (L1); an app
   * redrawing all the time would flush everything else out of the ring. */
  if (ev == LE_EXEC || ev == LE_EXIT) lcd_wanted = ~0ULL;
  if (ev == LE_BTN) lcd_after_btn = TRUE;
  if (ev == LE_LCD) {
	if (!(lcd_wanted & bit) && !lcd_after_btn) return;
	lcd_wanted &= ~bit;
	lcd_after_btn = FALSE;
  }

  /* Interrupts may already be off here: save and restore PRIMASK. */
  __asm volatile ("mrs %0, primask\n\tcpsid i" : "=r" (pm) : : "memory");
  e = &lat.ring[lat.head % LAT_RING];
  e->cyc = CYCCNT;
  e->ev = ev;
  e->arg = arg;
  lat.head++;
  if (ev == LE_LCD && lat.boot_lcd_ms == 0)
	lat.boot_lcd_ms = get_uptime() * 1000 / HZ;
  __asm volatile ("msr primask, %0" : : "r" (pm) : "memory");
}

PUBLIC void lat_lock()			/* interrupts just masked */
{
  lock_cyc = CYCCNT;
}

PUBLIC void lat_unlock()		/* about to unmask */
{
  unsigned long d = CYCCNT - lock_cyc;

  if (d > lat.mask_max) lat.mask_max = d;
}

PUBLIC void lat_revive(proc_nr)		/* a task sent REVIVE for proc_nr */
int proc_nr;
{
  rev_proc = proc_nr;
  rev_cyc = CYCCNT;
  rev_rep = 0;
}

PUBLIC void lat_reply(rp)		/* FS sends to rp */
struct proc *rp;
{
  unsigned long d;

  if (proc_number(rp) != rev_proc || rev_rep != 0) return;
  rev_rep = CYCCNT;
  rev_fl = lat.flash_n;
  win[0] = win[1] = win[2] = win[3] = 0;
  win_big = 0;
  d = rev_rep - rev_cyc;
  if (d > lat.rev_fs_max) lat.rev_fs_max = d;
  if (rp->p_shadow != 0) lat.rev_shadow_n++;
}

PUBLIC void lat_flip(end, bytes)	/* an image swap starts/ends */
int end;
unsigned long bytes;
{
  static unsigned long t0;
  unsigned long d;

  if (!end) { t0 = CYCCNT; return; }
  d = CYCCNT - t0;
  lat.flip_n++;
  if (d > lat.flip_max) { lat.flip_max = d; lat.flip_bytes = bytes; }
}

PUBLIC void lat_run(rp)			/* restart is entering rp */
struct proc *rp;
{
  unsigned long d, q, now = CYCCNT;
  int prev = last_run, c;

  /* the time since the last entry went to the process entered then */
  if (rev_rep != 0) {
	c = prev < 0 ? 0 : prev == MM_PROC_NR ? 1 : prev == FS_PROC_NR ? 2 : 3;
	d = now - (last_cyc - rev_rep < now - rev_rep ? last_cyc : rev_rep);
	win[c] += d;
	if (d > win_big) { win_big = d; win_big_proc = prev; }
  }
  last_cyc = now;
  last_run = proc_number(rp);
  if (proc_number(rp) != rev_proc) return;
  rev_proc = -1000;
  q = CYCCNT - rev_rep;
  if (rev_rep != 0 && q > lat.hz / 10000) {	/* over 100 us */
	lat.rev_q_slow++;
	if (lat.flash_n != rev_fl) lat.rev_q_slow_flash++;
  }
  if (rev_rep != 0 && q > lat.rev_q_max) {
	lat.rev_q_max = q;
	lat.rev_q_prev = prev;
	lat.rev_q_flash = lat.flash_n - rev_fl;
	for (c = 0; c < 4; c++) lat.rev_q_who[c] = win[c];
	lat.rev_q_big = win_big;
	lat.rev_q_big_proc = win_big_proc;
  }
  rev_rep = 0;
  d = CYCCNT - rev_cyc;
  if (d > lat.rev_max) lat.rev_max = d;
  lat.rev_sum += d;
  lat.rev_n++;
}

PUBLIC void lat_flash(end)		/* a flash operation starts/ends */
int end;
{
  static unsigned long t0;
  unsigned long d;

  if (!end) { t0 = CYCCNT; return; }
  d = CYCCNT - t0;
  if (d > lat.flash_max) lat.flash_max = d;
  lat.flash_n++;
}

PUBLIC int lat_read(user, count)	/* /dev/lat */
phys_bytes user;
int count;
{
  lat.hz = hal_cpu_hz();
  if (count > sizeof(lat)) count = sizeof(lat);
  phys_copy(vir2phys(&lat), user, (phys_bytes) count);
  return(count);
}

PUBLIC void lat_clear()
{
  lat.mask_max = lat.rev_max = lat.rev_sum = lat.rev_n = 0;
  lat.flash_max = lat.flash_n = 0;
  lat.rev_fs_max = lat.rev_q_max = lat.rev_shadow_n = 0;
  lat.flip_max = lat.flip_n = lat.flip_bytes = 0;
  lat.rev_q_flash = lat.rev_q_slow = lat.rev_q_slow_flash = 0;
  lat.rev_q_prev = 0;
  lat.hz = hal_cpu_hz();
}
#endif /* LATTRACE */
