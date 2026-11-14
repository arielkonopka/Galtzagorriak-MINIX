/* lat.h - latency trace (spec R0.8e), only in a LATTRACE=1 build
 *
 * The kernel keeps a ring of the last 32 events, each with the DWT cycle
 * counter, and a few running maxima; /dev/lat (Flipper device task, minor
 * LAT_MINOR) reads them as struct latinfo, a write clears the maxima.
 * The user command lat(1) turns them into the budgets L1-L10.
 */
#ifndef _MINIX_LAT_H
#define _MINIX_LAT_H

#define LAT_RING	32		/* events kept (a power of 2) */
#define LAT_MINOR	15		/* /dev/lat: major 6, minor 15 */

/* Events */
#define LE_BTN		1	/* a button went down (arg: key) */
#define LE_BREPLY	2	/* /dev/buttons read answered (arg: proc) */
#define LE_LCD		3	/* a frame written to /dev/lcd (arg: proc) */
#define LE_EXEC		4	/* exec done (arg: proc) */
#define LE_EXIT		5	/* process gone (arg: proc) */

struct latevent {
  unsigned long cyc;		/* DWT cycle counter */
  unsigned short ev, arg;
};

struct latinfo {
  unsigned long hz;		/* cycles per second */
  unsigned long head;		/* events ever recorded; ring[head % RING] next */
  unsigned long mask_max;	/* longest lock()..unlock(), cycles */
  unsigned long rev_max, rev_sum, rev_n;	/* REVIVE -> process runs */
  unsigned long boot_lcd_ms;	/* boot to the first frame from a process */
  struct latevent ring[LAT_RING];
};

#endif /* _MINIX_LAT_H */
