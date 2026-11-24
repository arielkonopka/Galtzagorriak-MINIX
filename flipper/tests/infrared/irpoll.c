/* irpoll - the layout of the Infrared app's Learn on its own (#416): a child
 * waits in a blocking read of /dev/ir, the parent polls /dev/buttons and the
 * pipe from the child with a 3 s limit.  Prints how long each poll took; a
 * poll that does not come back in about 3 s is the hang.  Run as root on the
 * device (copy over apps/hello.c, send the hello package).  Press a key
 * during the run to see a key wake the poll.
 * Usage: irpoll [-n]   -n: no child (control: the poll on its own) */
#include <sys/types.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int bfd, ifd, p[2], i, r, child = !(argc > 1 && strcmp(argv[1], "-n") == 0);
	pid_t pid = -1;
	struct pollfd pf[2];
	struct tms tm;
	clock_t t0;
	char buf[256];

	if ((bfd = open("/dev/buttons", O_RDONLY)) < 0) { perror("/dev/buttons"); return 1; }
	if ((ifd = open("/dev/ir", O_RDWR)) < 0) { perror("/dev/ir"); return 1; }
	if (pipe(p) < 0) { perror("pipe"); return 1; }
	if (child && (pid = fork()) == 0) {
		close(p[0]);
		r = read(ifd, buf, 256);	/* blocks until a frame */
		if (r < 0) perror("child: read /dev/ir");
		else printf("child: read %d bytes\n", r);
		fflush(stdout);
		write(p[1], (char *) &r, sizeof r);
		_exit(0);
	}
	if (child) close(p[1]);
	printf("child %d; polling buttons + pipe, 3 s each\n", (int) pid);
	fflush(stdout);
	for (i = 0; i < 4; i++) {
		pf[0].fd = bfd; pf[0].events = POLLIN; pf[0].revents = 0;
		pf[1].fd = p[0]; pf[1].events = POLLIN; pf[1].revents = 0;
		t0 = times(&tm);
		r = poll(pf, 2, 3000);
		printf("poll %d: r=%d took %ld ms (buttons 0x%x pipe 0x%x)\n", i, r,
			(long) ((times(&tm) - t0) * 1000 / CLK_TCK), pf[0].revents, pf[1].revents);
		fflush(stdout);
		if (r > 0 && (pf[0].revents & POLLIN)) read(bfd, buf, 2);
	}
	if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); }
	printf("done\n");
	return 0;
}
