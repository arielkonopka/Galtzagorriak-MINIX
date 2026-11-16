/* utime(2) for POSIX		Authors: Terrence W. Holm & Edwin L. Froese */

#include <lib.h>
#define utime	_utime
#include <string.h>
#include <utime.h>

PUBLIC int utime(name, timp)
_CONST char *name;
_CONST struct utimbuf *timp;
{
  message m;

  if (timp == NULL) {
	m.m2_i1 = 0;		/* name size 0 means NULL `timp' */
	m.m2_i2 = strlen(name) + 1;	/* actual size here */
  } else {
	/* Stored as unsigned 32 bits (R2d.2, R2d.3): 1970 to 2106-02-07
	 * 06:28:14 UTC, the all-ones value is kept for "none". */
	if (timp->actime < 0 || timp->actime > (time_t) 0xFFFFFFFEUL
	    || timp->modtime < 0 || timp->modtime > (time_t) 0xFFFFFFFEUL) {
		errno = EINVAL;
		return(-1);
	}
	m.m2_l1 = (long) (unsigned long) timp->actime;
	m.m2_l2 = (long) (unsigned long) timp->modtime;
	m.m2_i1 = strlen(name) + 1;
  }
  m.m2_p1 = (char *) name;
  return(_syscall(FS, UTIME, &m));
}
