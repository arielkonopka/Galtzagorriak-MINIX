/* The error path of vfork() (src/lib/arm/rts/_vfork.S): errno, -1.  An MM
 * without VFORK (an older system image: no_sys says EINVAL) gets a fork
 * instead, so new programs still run there and no ABI bump is needed.
 * Called by a tail call after the trap, with no child yet: an ordinary
 * call to fork() is safe here. */
#include <lib.h>
#include <errno.h>
#include <unistd.h>

int __vfork_err(int r)
{
  if (r == -EINVAL) return(fork());
  errno = -r;
  return(-1);
}
