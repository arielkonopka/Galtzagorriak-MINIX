/* gpwatch - device test of the EXTI edges on header pins 2 and 4 (R6.4, #216):
 * watches one pin by poll() and prints every edge with the time it took the
 * program to wake.  The pin gets a pull-down; touch it to 3V3 (header pin 9)
 * with a jumper wire.  Not built by the Makefile: for the device, copy it
 * over apps/hello.c and send the hello package.
 * Usage: gpwatch [PIN [SECONDS]]   PIN 0 = header 2 (default), 2 = header 4 */
#include <sys/types.h>
#include <sys/times.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	unsigned char op[2];
	char v[16];
	struct pollfd p;
	struct tms tm;
	int g, pin = argc > 1 ? atoi(argv[1]) : 0;
	int secs = argc > 2 ? atoi(argv[2]) : 20, n = 0;
	clock_t t0, t;

	if ((g = open("/dev/gpio", O_RDWR)) < 0) {
		perror("/dev/gpio");
		return 1;
	}
	op[0] = pin; op[1] = 4;			/* pull-down */
	write(g, (char *) op, 2);
	op[1] = 6;				/* watch */
	if (write(g, (char *) op, 2) != 2) {
		perror("watch");
		return 1;
	}
	printf("watching pin %d for %d s: touch it to 3V3 (header 9)\n", pin, secs);
	fflush(stdout);
	t0 = times(&tm);
	for (;;) {
		t = times(&tm);
		if ((t - t0) / CLK_TCK >= (unsigned) secs) break;
		p.fd = g; p.events = POLLIN; p.revents = 0;
		if (poll(&p, 1, 500) == 1 && read(g, v, 16) == 16) {
			n++;
			printf("%3ld.%02ld s: level %c, changed %c\n",
				(long) ((times(&tm) - t0) / CLK_TCK),
				(long) ((times(&tm) - t0) % CLK_TCK * 100 / CLK_TCK),
				v[pin], v[8 + pin]);
			fflush(stdout);
		}
	}
	op[1] = 5;				/* off again */
	write(g, (char *) op, 2);
	close(g);
	printf("%d edge reports\n", n);
	return 0;
}
