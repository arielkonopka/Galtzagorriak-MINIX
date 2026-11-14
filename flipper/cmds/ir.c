/* ir - infrared receive and send on the Flipper (Galtzagorriak MINIX)
 *
 * Usage:	ir rx [-n COUNT]	print received frames (default: one)
 *		ir tx nec ADDR CMD	send an NEC frame (ADDR 0..255, or
 *					0..65535 for extended NEC)
 *		ir tx raw US US ...	send marks and spaces, microseconds,
 *					mark first
 *
 * Frames are printed as raw timings, and decoded when they are NEC.
 * Uses /dev/ir (kernel/arm/flipdev.c): 32-bit durations in microseconds.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXD	256

static unsigned long d[MAXD];
static int nd;

static void usage(void)
{
	fprintf(stderr, "Usage: ir rx [-n COUNT]\n"
			"       ir tx nec ADDR CMD\n"
			"       ir tx raw US US ...\n");
	exit(1);
}

/* Within 25 % (and 150 us) of the nominal duration? */
static int near(unsigned long v, unsigned long nom)
{
	unsigned long tol = nom / 4 > 150 ? nom / 4 : 150;

	return v + tol >= nom && v <= nom + tol;
}

static void put(unsigned long us)
{
	if (nd < MAXD) d[nd++] = us;
}

static void nec_bits(unsigned long v, int n)
{
	while (n-- > 0) {
		put(562);
		put(v & 1 ? 1687 : 562);
		v >>= 1;
	}
}

static void decode(void)
{
	unsigned long bits = 0;
	int i;

	if (nd == 3 && near(d[0], 9000) && near(d[1], 2250)) {
		printf("NEC repeat\n");
		return;
	}
	if (nd != 67 || !near(d[0], 9000) || !near(d[1], 4500)) return;
	for (i = 0; i < 32; i++) {
		if (!near(d[2 + 2*i], 562)) return;
		if (near(d[3 + 2*i], 1687)) bits |= 1UL << i;
		else if (!near(d[3 + 2*i], 562)) return;
	}
	if (((bits >> 16) & 0xFF) != (~(bits >> 24) & 0xFF)) return;
	if ((bits & 0xFF) == (~(bits >> 8) & 0xFF))
		printf("NEC address 0x%02lx command 0x%02lx\n",
			bits & 0xFF, (bits >> 16) & 0xFF);
	else
		printf("NEC (extended) address 0x%04lx command 0x%02lx\n",
			bits & 0xFFFF, (bits >> 16) & 0xFF);
}

static int rx(int count)
{
	int fd, n, i;

	if ((fd = open("/dev/ir", O_RDONLY)) < 0) {
		perror("ir: /dev/ir");
		return 1;
	}
	while (count-- > 0) {
		if ((n = read(fd, (char *) d, sizeof(d))) < 0) {
			perror("ir");
			return 1;
		}
		nd = n / 4;
		printf("%d:", nd);
		for (i = 0; i < nd; i++) printf(" %lu", d[i]);
		printf("\n");
		decode();
		fflush(stdout);
	}
	return 0;
}

static int tx(void)
{
	int fd;

	if (nd == 0 || nd % 2 == 0) {
		fprintf(stderr, "ir: need an odd number of durations\n");
		return 1;
	}
	if ((fd = open("/dev/ir", O_WRONLY)) < 0) {
		perror("ir: /dev/ir");
		return 1;
	}
	if (write(fd, (char *) d, nd * 4) != nd * 4) {
		perror("ir");
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	unsigned long a, c;
	int i;

	if (argc >= 2 && strcmp(argv[1], "rx") == 0) {
		if (argc == 2) return rx(1);
		if (argc == 4 && strcmp(argv[2], "-n") == 0)
			return rx(atoi(argv[3]));
		usage();
	}
	if (argc == 5 && strcmp(argv[1], "tx") == 0
				&& strcmp(argv[2], "nec") == 0) {
		a = strtoul(argv[3], NULL, 0);
		c = strtoul(argv[4], NULL, 0);
		if (a > 0xFFFF || c > 0xFF) usage();
		put(9000);
		put(4500);
		if (a > 0xFF) nec_bits(a, 16);
		else nec_bits(a | (~a & 0xFF) << 8, 16);
		nec_bits(c | (~c & 0xFF) << 8, 16);
		put(562);
		return tx();
	}
	if (argc >= 4 && strcmp(argv[1], "tx") == 0
				&& strcmp(argv[2], "raw") == 0) {
		for (i = 3; i < argc; i++) put(strtoul(argv[i], NULL, 0));
		return tx();
	}
	usage();
	return 1;
}
