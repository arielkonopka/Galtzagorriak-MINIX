/* iralarm - the Infrared app's reader loop on its own (#416): a blocking
 * read of /dev/ir that a 1 s alarm interrupts (EINTR) and that is started
 * again.  Prints every frame's length and first durations and how many
 * alarms came before it, to see whether an interrupted read loses the start
 * of the next frame.  For the device: copy over apps/hello.c, send the
 * hello package.
 * Usage: iralarm [FRAMES [SECONDS_OF_ALARM]]   (alarm 0: no alarm at all) */
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static unsigned long t[600];

static void on_alarm(int sig)
{
	(void) sig;
}

int main(int argc, char **argv)
{
	int frames = argc > 1 ? atoi(argv[1]) : 3, al = argc > 2 ? atoi(argv[2]) : 1;
	int fd, r, en, ints = 0;
	struct sigaction sa;

	if ((fd = open("/dev/ir", O_RDONLY)) < 0) { perror("/dev/ir"); return 1; }
	sa.sa_handler = on_alarm;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGALRM, &sa, NULL);
	while (frames > 0) {
		if (al) alarm(al);
		r = read(fd, (char *) t, sizeof t);
		en = errno;
		alarm(0);
		if (r < 0 && en == EINTR) { ints++; continue; }
		if (r < 0) { errno = en; perror("read"); return 1; }
		printf("frame: %d durations, starts %lu %lu %lu, after %d alarms\n",
			r / 4, t[0], t[1], t[2], ints);
		fflush(stdout);
		ints = 0;
		frames--;
	}
	return 0;
}
