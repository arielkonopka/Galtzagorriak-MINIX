/* fg.h - the foreground app (spec R4b.8, kernel/arm/flipdev.c)
 *
 * ioctl(fd, FGIOCSET, &pid) on /dev/lcd or /dev/buttons hands the screen
 * and the buttons to process pid (0: nobody, everyone may use them).  Only
 * the foreground process may change it, or anybody while there is none or
 * it has exited.  While a foreground app is set, other writes to /dev/lcd
 * are dropped and other reads of /dev/buttons wait; holding Back for a
 * second sends it SIGTERM, and SIGKILL 3 s later if it is still the
 * foreground.  The servers and init can never be the foreground (EPERM).
 */
#ifndef _MINIX_FG_H
#define _MINIX_FG_H

#include <sys/ioctl.h>

#define FGIOCSET	_IOW('F', 1, int)
/* R6.10: the foreground app turns the button gestures off (0) or on (1)
 * for itself (games using diagonals); cleared by the next FGIOCSET. */
#define BTNIOCGEST	_IOW('F', 2, int)
/* R7b.2: an alert takes the screen and the buttons from whoever has them
 * (FGIOCPREEMPT; EBUSY while another alert has them: try again later)
 * and gives them back (FGIOCRELEASE, or by exiting).  The preempted app
 * keeps running; its frames are dropped and its button reads wait; a read
 * that waits when the alert comes, or is parked meanwhile, ends with 0
 * bytes (redraw, read again).  The alert restores the screen's picture. */
#define FGIOCPREEMPT	_IO('F', 3)
#define FGIOCRELEASE	_IO('F', 4)

#endif /* _MINIX_FG_H */
