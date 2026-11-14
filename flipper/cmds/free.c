/*	free - show free memory				(Flipper port)
 *
 *	free [-v] [-k]
 *
 * Total free RAM and the largest contiguous hole; -v lists the holes,
 * -k the stack use (high-water mark) of the kernel tasks.
 * Without an MMU every program needs one contiguous block (text + data +
 * stack/heap), so the largest hole decides what can still be started.
 */
#include <lib.h>
#include <stdio.h>
#include <string.h>
#include <minix/callnr.h>

static void kstacks(void)
{
	message m;
	struct kstack ks[24];
	int n, i, size = 0, used = 0;

	m.m1_p1 = (char *) ks;
	m.m1_i1 = (int) sizeof ks;
	m.m1_i2 = MI_KSTACKS;
	if ((n = _syscall(MM, MEMINFO, &m)) < 0) {
		perror("free -k");
		return;
	}
	printf("kernel task stacks:\n");
	for (i = 0; i < n; i++) {
		printf("  %-8.8s %5u of %5u bytes used\n", ks[i].name,
			ks[i].used, ks[i].size);
		size += ks[i].size;
		used += ks[i].used;
	}
	printf("  total    %5d of %5d bytes used\n", used, size);
}

int main(int argc, char **argv)
{
	message m;
	unsigned long holes[2 * 16];
	int total, i, v = 0, k = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0) v = 1;
		else if (strcmp(argv[i], "-k") == 0) k = 1;
		else {
			fprintf(stderr, "Usage: free [-v] [-k]\n");
			return 1;
		}
	}
	memset(holes, 0, sizeof holes);
	m.m1_p1 = v ? (char *) holes : NULL;
	m.m1_i1 = v ? (int) sizeof holes : 0;
	m.m1_i2 = 0;
	if ((total = _syscall(MM, MEMINFO, &m)) < 0) {
		perror("free");
		return 1;
	}
	printf("free %d KB, largest hole %d KB\n", total / 1024,
							m.m2_i1 / 1024);
	for (i = 0; v && i < 16 && holes[2 * i + 1] != 0; i++)
		printf("  hole at %08lx  %5lu bytes (%lu KB)\n", holes[2 * i],
			holes[2 * i + 1], holes[2 * i + 1] / 1024);
	if (k) kstacks();
	return 0;
}
