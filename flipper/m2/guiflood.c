/* guiflood - ACK.3's four desktop modules (spec K.6, T23a, issue #324;
 * QEMU only, /bin/guiflood).
 *
 * Usage:	guiflood
 *
 * Forks three copies of itself, then all four write tile records to
 * /var/run/gui as fast as they can (O_NONBLOCK: a full FIFO is skipped),
 * until they are killed: tiles w0..w3, each with its own pid.  m2test
 * forks once and execs this, in a process group of its own (setsid), and
 * ends it with kill(-pgrp).  Before, the four flooders were forks of
 * m2test itself: each needed a copy of m2test's data, and every new
 * m2test check took from a margin of a few bytes.  This image is a few
 * hundred bytes, so m2test can grow without touching ACK.3.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

/* QEMU retranslates code on a page that is written: four loops whose
 * errno (EAGAIN, a full FIFO) sits next to their code would crawl.  This
 * puts the data 4 KB away from the code (text: one copy for all four). */
static const volatile char pad[4096] = { 1 };

static char *num(char *p, unsigned long v)	/* decimal, no stdio */
{
	char t[12];
	int n = 0;

	do t[n++] = (char) ('0' + v % 10); while ((v /= 10) != 0);
	while (n > 0) *p++ = t[--n];
	return p;
}

static char *cpy(char *p, const char *s)
{
	while (*s != '\0') *p++ = *s++;
	return p;
}

static int __attribute__((noinline)) flood(void)
{
	char rec[64], *p, *q;
	unsigned long k;
	int i, fd, n;

	for (i = 0; i < 3; i++)
		if (fork() == 0) break;
	i = i < 3 ? i + 1 : 0;
	if (pad[0] != 1 || (fd = open("/var/run/gui", O_WRONLY)) < 0)
		return 1;
	for (k = 0; ; k++) {
		n = sprintf(rec, "tile\tw%d\t0\t%d\tflood %ld\n", i,
			    (int) getpid(), (long) k);
		(void) write(fd, rec, (size_t) n);
	}
}

int main(void)
{
	volatile char guard[2048];	/* the loop's stack 2 KB below the top */

	guard[0] = 0;
	return flood() + guard[0];
}
