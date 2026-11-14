/*	free - show free memory				(Flipper port)
 *
 *	free [-v]
 *
 * Total free RAM and the largest contiguous hole; -v lists the holes.
 * Without an MMU every program needs one contiguous block (text + data +
 * stack/heap), so the largest hole decides what can still be started.
 */
#include <lib.h>
#include <stdio.h>
#include <string.h>
#include <minix/callnr.h>

int main(int argc, char **argv)
{
	message m;
	unsigned long holes[2 * 16];
	int total, i, v = argc > 1 && strcmp(argv[1], "-v") == 0;

	memset(holes, 0, sizeof holes);
	m.m1_p1 = v ? (char *) holes : NULL;
	m.m1_i1 = v ? (int) sizeof holes : 0;
	if ((total = _syscall(MM, MEMINFO, &m)) < 0) {
		perror("free");
		return 1;
	}
	printf("free %d KB, largest hole %d KB\n", total / 1024,
							m.m2_i1 / 1024);
	for (i = 0; v && i < 16 && holes[2 * i + 1] != 0; i++)
		printf("  hole at %08lx  %5lu bytes (%lu KB)\n", holes[2 * i],
			holes[2 * i + 1], holes[2 * i + 1] / 1024);
	return 0;
}
