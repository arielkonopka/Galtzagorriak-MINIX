/* peek - show memory through /dev/mem (Galtzagorriak MINIX; a tool for
 * bring-up, e.g. CPU2's mailbox tables at 0x20030000: T30b).
 *
 * Usage:	peek ADDR [BYTES]	ADDR in hex, BYTES in decimal (64)
 *
 * Prints 32-bit little-endian words, 4 a line, with the address.  Root
 * only (the device file's mode); only where the board has memory: RAM,
 * flash and ROM (a peripheral's address is an I/O error, not a fault).
 * No stdio: 0.5 KB of buffers on the stack.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void out(const char *s)
{
	(void) write(1, s, strlen(s));
}

static char *hex8(char *b, unsigned long v)
{
	int i;

	for (i = 7; i >= 0; i--, v >>= 4) b[i] = "0123456789abcdef"[v & 15];
	b[8] = 0;
	return b;
}

int main(int argc, char **argv)
{
	unsigned char buf[256];
	char b[12];
	unsigned long addr;
	long n = 64, got, i;
	int fd;

	if (argc < 2 || argc > 3) {
		out("usage: peek ADDR(hex) [BYTES]\n");
		return 2;
	}
	addr = strtoul(argv[1], (char **) 0, 16) & ~3UL;
	if (argc == 3) n = atol(argv[2]);
	if (n < 4 || n > (long) sizeof buf) n = n < 4 ? 4 : (long) sizeof buf;
	n &= ~3L;
	if ((fd = open("/dev/mem", O_RDONLY)) < 0
	    || lseek(fd, (off_t) addr, SEEK_SET) == (off_t) -1
	    || (got = read(fd, buf, (size_t) n)) < 4) {
		out("peek: cannot read there\n");
		return 1;
	}
	for (i = 0; i + 4 <= got; i += 4) {
		if (i % 16 == 0) {
			out(hex8(b, addr + (unsigned long) i));
			out(":");
		}
		out(" ");
		out(hex8(b, buf[i] | buf[i + 1] << 8 | (unsigned long) buf[i + 2] << 16
				| (unsigned long) buf[i + 3] << 24));
		if (i % 16 == 12 || i + 8 > got) out("\n");
	}
	return 0;
}
