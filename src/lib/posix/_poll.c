#include <lib.h>
#define poll	_poll
#include <poll.h>

PUBLIC int poll(fds, nfds, timeout)
struct pollfd *fds;
nfds_t nfds;
int timeout;			/* ms; -1: none */
{
/* R4b.6.  FS rejects more than OPEN_MAX entries (a negative count too). */
  message m;

  m.m1_i1 = 0;
  m.m1_i2 = (int) nfds;
  m.m1_i3 = timeout;
  m.m1_p1 = (char *) fds;
  return(_syscall(FS, POLL, &m));
}
