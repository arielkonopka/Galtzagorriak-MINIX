/* spinfifo - processes that spin on EAGAIN (issue #331; QEMU only,
 * /bin/spinfifo).
 *
 * Usage:	spinfifo [n]
 *
 * Forks n - 1 copies of itself (default n = 4: a shadow group of four,
 * as an app's forked workers are), then each fills a pipe of its own,
 * sets it O_NONBLOCK and writes to it for ever: every write fails with
 * EAGAIN at once, so each spinner is always ready and calls FS all the
 * time.  m2test runs it in a process group of its own and measures key ->
 * frame on the launcher meanwhile (the AmigaOS rule: the UI responds even
 * under load), then kills the group.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	static char buf[512];
	int p[2], i, n = argc > 1 ? atoi(argv[1]) : 4;

	for (i = 1; i < n; i++)
		if (fork() <= 0) break;
	if (pipe(p) < 0 || fcntl(p[1], F_SETFL, O_NONBLOCK) < 0) return 1;
	for (;;) (void) write(p[1], buf, sizeof buf);
}
