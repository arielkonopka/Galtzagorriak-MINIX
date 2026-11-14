/*	free - show free memory				(Flipper port)
 *
 *	free [-v] [-k] [-t on|off]
 *
 * Total free RAM and the largest contiguous hole; -v lists the holes,
 * -k the stack use (high-water mark) of the kernel tasks.  -t on makes MM
 * print, for every program that exits, how much of its chmem (gap + stack)
 * was never touched (root only; slows exec down a little).
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

	if (argc == 3 && strcmp(argv[1], "-t") == 0) {
		m.m1_i1 = strcmp(argv[2], "on") == 0;
		m.m1_i2 = MI_TRACE;
		if (_syscall(MM, MEMINFO, &m) < 0) {
			perror("free -t");
			return 1;
		}
		return 0;
	}

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0) v = 1;
		else if (strcmp(argv[i], "-k") == 0) k = 1;
		else {
			fprintf(stderr, "Usage: free [-v] [-k] | free -t on|off\n");
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
	if (v) {
		struct fcinfo fi;

		m.m1_p1 = (char *) &fi;
		m.m1_i2 = MI_FCACHE;
		if (_syscall(MM, MEMINFO, &m) == 0) {
			if (fi.size != 0)
				printf("flash text cache: %d programs (%d "
					"running), %lu of %lu KB at %08lx\n",
					fi.entries, fi.running, fi.used / 1024,
					fi.size / 1024, fi.cache_base);
			printf("installed programs: %lu KB at %08lx; "
				"CPU2 flash from %08lx\n", fi.inst_size / 1024,
				fi.inst_base, fi.sfsa);
		}
	}
	if (k) kstacks();
	return 0;
}
