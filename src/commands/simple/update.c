/* update - do sync periodically		Author: Andy Tanenbaum */

/* Spec O.5: after each sync, update drives the FS server's idle recount of
 * the free counters (FSTATFS_RECOUNT): 8 map blocks per call, then a 20 ms
 * pause, so a foreground request waits at most one step (a few ms) and the
 * CPU idles in between.  A recount runs once after every mount. */

#include <lib.h>
#include <sys/types.h>
#include <sys/statfs.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>

_PROTOTYPE(int main, (void));
_PROTOTYPE(static int recount, (void));

static int recount()
{
  message m;

  m.m1_i1 = FSTATFS_RECOUNT;
  return(_syscall(FS, FSTATFS, &m));
}

int main()
{
  int t;

  /* Release all (?) open file descriptors. */
  close(0);
  close(1);
  close(2);

  /* Release current directory to avoid locking current device. */
  chdir("/");

  /* Flush the cache every 30 seconds. */
  while (1) {
	sync();
	/* At most 30 s of recount steps, then the next sync. */
	for (t = 0; t < 1500 && recount() > 0; t++)
		(void) poll((struct pollfd *) 0, 0, 20);
	if (t == 0) sleep(30);
  }
}
