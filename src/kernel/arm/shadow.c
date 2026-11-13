/* Process shadowing (fork without an MMU) - milestone 3.
 *
 * Placeholder: no process forks before MM, FS and the C library exist, so
 * reaching any of these is a bug.
 */
#include "../kernel.h"
#include "../proc.h"
#include <string.h>

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
/* Exchange two non-overlapping click ranges. */
  u32_t *a = (u32_t *) ((phys_bytes) c1 << CLICK_SHIFT);
  u32_t *b = (u32_t *) ((phys_bytes) c2 << CLICK_SHIFT);
  size_t words = ((size_t) n << CLICK_SHIFT) / sizeof(u32_t);
  u32_t t;

  while (words-- > 0) {
	t = *a;
	*a++ = *b;
	*b++ = t;
  }
}


PUBLIC void mkshadow(p, c2)
struct proc *p;
phys_clicks c2;
{
  panic("mkshadow not implemented", proc_number(p));
}

PUBLIC void rmshadow(p, basep, sizep)
struct proc *p;
phys_clicks *basep, *sizep;
{
  panic("rmshadow not implemented", proc_number(p));
}

PUBLIC void unshadow(p)
struct proc *p;
{
  panic("unshadow not implemented", proc_number(p));
}
