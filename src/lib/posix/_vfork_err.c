/* The error path of vfork() (src/lib/arm/rts/_vfork.S): errno, -1. */
#include <lib.h>
#include <errno.h>

int __vfork_err(int r)
{
  errno = -r;
  return(-1);
}
