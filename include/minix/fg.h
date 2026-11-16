/* fg.h - the foreground app (spec R4b.8, kernel/arm/flipdev.c)
 *
 * ioctl(fd, FGIOCSET, &pid) on /dev/lcd or /dev/buttons hands the screen
 * and the buttons to process pid (0: nobody, everyone may use them).  Only
 * the foreground process may change it, or anybody while there is none or
 * it has exited.  While a foreground app is set, other writes to /dev/lcd
 * are dropped and other reads of /dev/buttons wait; holding Back for a
 * second sends it SIGTERM, and SIGKILL 3 s later if it is still the
 * foreground.
 */
#ifndef _MINIX_FG_H
#define _MINIX_FG_H

#include <sys/ioctl.h>

#define FGIOCSET	_IOW('F', 1, int)

#endif /* _MINIX_FG_H */
