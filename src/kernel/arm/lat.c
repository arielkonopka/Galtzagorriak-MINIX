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
PRIVATE unsigned long rev_cyc;

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
}

PUBLIC void lat_run(rp)			/* restart is entering rp */
struct proc *rp;
{
  unsigned long d;

  if (proc_number(rp) != rev_proc) return;
  rev_proc = -1000;
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
}
#endif /* LATTRACE */
