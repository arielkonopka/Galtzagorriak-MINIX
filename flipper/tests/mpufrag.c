/* mpufrag - S3/R1.6: a process whose memory the MPU cannot describe with
 * its 7 regions must not get access to all of RAM (the old fallback).
 * Copies of this program with many different chmem sizes are run; each
 * tries to read kernel RAM.  Allowed outcomes: SIGSEGV (isolated), SIGKILL
 * (fail closed), or exec refused (ENOMEM).  Not allowed: the read works.
 */
#include "t.h"

#define PROG	"/tmp/mpufrag.x"
#define KERNEL_RAM	0x20000100L

static char *self;

static void run(void)
{
	execl(PROG, "mpufrag", "probe", (char *) 0);
	_exit(errno == ENOMEM || errno == EAGAIN ? 3 : 2);
}

int main(int argc, char **argv)
{
	struct t_hdr h;
	long total, base;
	int st, k, segv = 0, killed = 0, refused = 0, bad = 0, other = 0;
	char buf[80];

	if (argc == 2 && strcmp(argv[1], "probe") == 0) {
		volatile long v = *(volatile long *) KERNEL_RAM;

		(void) v;
		_exit(1);			/* the read worked: not isolated */
	}
	self = "/usr/tests/mpufrag";
	if (t_header(self, &h) < 0) {
		t_result("read own header", 0, self);
		return 1;
	}
	base = h.data + h.bss;
	for (k = 0; k < 48; k++) {
		/* odd sizes: 1.25 KB steps from 2 KB over the data */
		total = base + 2048 + k * 1280 + (k & 3) * 256;
		if (t_copy(self, PROG, 0775) < 0
		    || t_patch(PROG, 24, &total, 4) < 0) {
			t_result("make the test program", 0, NULL);
			return 1;
		}
		st = t_child(run);
		if (st != -1 && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV) segv++;
		else if (st != -1 && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL)
			killed++;
		else if (st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 3)
			refused++;
		else if (st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 1)
			bad++;
		else other++;
	}
	unlink(PROG);
	sprintf(buf, "%d isolated, %d killed, %d refused, %d read RAM, "
		"%d other", segv, killed, refused, bad, other);
	t_result("no layout gets all of RAM", bad == 0 && other == 0, buf);
	t_result("system still running", 1, NULL);
	return t_fail;
}
