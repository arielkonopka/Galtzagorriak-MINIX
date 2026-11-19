/* alarmrace - alarm() under load (issue #308: MM panic "alarm er").
 *
 *   alarmrace [-s SECS] [-a N] [-p N]
 *
 * For SECS seconds (default 20): N children (-a, default 3) call alarm()
 * in a tight loop (MM sends CLOCK a SET_ALARM each time); N children (-p,
 * default 2) poll() with a 1 ms timeout (FS's synchronous alarm, every
 * tick); as root, one child sets the clock 10 times a second (stime:
 * CLOCK sends INIT SIGRTC).  MM's own synchronous alarm runs while a copy
 * to the flash cache waits for an idle system: tools/alarmtest.py runs
 * this program three times from a fresh file, so its third run is queued
 * for the cache and MM steps every tick while the children keep the CPU
 * busy.
 *
 * The old fault: two synchronous alarms due in one clock pass (MM's and
 * FS's) made CLOCK block sending to SYN_AL, which was blocked sending
 * CLOCK_INT to MM; MM's next sendrec(CLOCK) then failed with ELOCKED and
 * MM panicked ("alarm er").  PASS: every child ran and ended by our
 * SIGTERM, and alarm() still answers.  A panic halts the system, which
 * the harness sees.  Writes nothing to flash itself.
 */
#include <poll.h>
#include <time.h>
#include "t.h"

#define MAXKIDS	16

static volatile int stop;

static void on_term(int sig)
{
	stop = 1;
}

static void on_alrm(int sig)
{
	signal(SIGALRM, on_alrm);
}

/* The children report how many rounds they made through a pipe. */
static void report(int fd, unsigned long n)
{
	(void) write(fd, (char *) &n, sizeof n);
	_exit(0);
}

static void alarm_kid(int fd)
{
	unsigned long n = 0;

	signal(SIGALRM, on_alrm);
	while (!stop) {
		alarm(1000);			/* SET_ALARM, an alarm to cancel */
		alarm((unsigned) (n & 1));	/* sometimes one that fires */
		n++;
	}
	alarm(0);
	report(fd, n);
}

static void poll_kid(int fd)
{
	unsigned long n = 0;

	while (!stop) {
		(void) poll((struct pollfd *) NULL, 0, 1);
		n++;
	}
	report(fd, n);
}

static void rtc_kid(int fd)
{
	unsigned long n = 0;
	time_t t;

	while (!stop) {
		t = time((time_t *) NULL);
		if (stime(&t) == 0) n++;	/* SIGRTC to INIT */
		(void) poll((struct pollfd *) NULL, 0, 100);
	}
	report(fd, n);
}

int main(int argc, char **argv)
{
	int secs = 20, na = 3, np = 2, nk = 0, i, st, fd[2], bad = 0;
	pid_t pid[MAXKIDS];
	unsigned long n, total = 0;
	int kind[MAXKIDS];

	while ((i = getopt(argc, argv, "s:a:p:")) != EOF) {
		switch (i) {
		case 's': secs = atoi(optarg); break;
		case 'a': na = atoi(optarg); break;
		case 'p': np = atoi(optarg); break;
		default:
			fprintf(stderr,
				"usage: alarmrace [-s secs] [-a n] [-p n]\n");
			return 1;
		}
	}
	if (na < 1) na = 1;
	if (na + np + 1 > MAXKIDS) np = MAXKIDS - 1 - na;
	if (pipe(fd) != 0) {
		perror("alarmrace: pipe");
		return 1;
	}
	signal(SIGTERM, on_term);
	fflush(stdout);
	for (i = 0; i < na + np + (getuid() == 0); i++) {
		kind[nk] = i < na ? 0 : i < na + np ? 1 : 2;
		if ((pid[nk] = fork()) == 0) {
			close(fd[0]);
			if (kind[nk] == 0) alarm_kid(fd[1]);
			else if (kind[nk] == 1) poll_kid(fd[1]);
			else rtc_kid(fd[1]);
		}
		if (pid[nk] < 0) {
			perror("alarmrace: fork");
			bad++;
			break;
		}
		nk++;
	}
	close(fd[1]);
	sleep((unsigned) secs);
	for (i = 0; i < nk; i++) kill(pid[i], SIGTERM);
	for (i = 0; i < nk; i++) {
		if (waitpid(pid[i], &st, 0) != pid[i] || !WIFEXITED(st)
						|| WEXITSTATUS(st) != 0) {
			printf("child %d (kind %d) ended badly: status %#x\n",
				(int) pid[i], kind[i], st);
			bad++;
		}
	}
	while (read(fd[0], (char *) &n, sizeof n) == sizeof n) total += n;
	close(fd[0]);
	printf("alarmrace: %d s, %d alarm, %d poll, %d stime children; "
		"%lu rounds\n", secs, na, np, nk - na - np, total);
	/* MM and CLOCK still answer. */
	signal(SIGALRM, on_alrm);
	alarm(5);
	i = (int) alarm(0);
	t_result("alarm() under load (#308)", bad == 0 && total > 0
						&& i >= 4 && i <= 5, NULL);
	return t_fail;
}
