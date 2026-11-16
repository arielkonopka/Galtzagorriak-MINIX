/* rtc - the RTC alarm and calibration (Galtzagorriak MINIX, spec R6.2,
 * R7b.1; /dev/rtc, <minix/rtc.h>).
 *
 * Usage:	rtc			show the alarm, the boot lead, whether
 *					this boot came from the alarm, and the
 *					calibration
 *		rtc alarm +SECONDS	arm Alarm A that many seconds from now
 *					(at most 27 days); INIT gets SIGRTC,
 *					and a switched-off Flipper starts
 *		rtc alarm off		clear it
 *		rtc lead SECONDS	how much earlier the alarm starts a
 *					switched-off Flipper (the boot time)
 *		rtc cal STEPS		smooth calibration, -511..512 steps of
 *					0.954 ppm (2.5 s a month); + is faster
 *
 * Alarms for people come from timed and the clock app (spec F.2); this is
 * the tool underneath, for tests and the clock settings.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <minix/rtc.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void usage(void)
{
	fprintf(stderr, "usage: rtc [alarm +SECONDS|off] [lead SECONDS] [cal STEPS]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	struct rtc_alarm a;
	int fd, cal, set;
	time_t now = time((time_t *) 0);

	if ((fd = open("/dev/rtc", O_RDONLY)) < 0) {
		perror("rtc: /dev/rtc");
		return 1;
	}
	if (ioctl(fd, RTCIOCGALARM, &a) < 0 || ioctl(fd, RTCIOCGCAL, &cal) < 0) {
		perror("rtc");
		return 1;
	}
	if (argc == 1) {
		if (a.flags & RTC_ARMED)
			printf("alarm in %ld s (at %lu), lead %lu s\n",
				(long) ((time_t) a.due - now), a.due, a.lead);
		else
			printf("alarm none%s, lead %lu s\n",
				a.flags & RTC_FIRED ? " (it fired)" : "", a.lead);
		printf("this boot: %s\n", a.flags & RTC_WOKE ? "by the alarm"
								: "by hand");
		printf("cal %d (%s%d.%d ppm)\n", cal, cal < 0 ? "-" : "+",
			abs(cal) * 954 / 1000, abs(cal) * 954 / 100 % 10);
		return 0;
	}
	if (argc != 3) usage();
	if (strcmp(argv[1], "alarm") == 0) {
		if (strcmp(argv[2], "off") == 0) a.due = 0;
		else if (argv[2][0] == '+') a.due = (unsigned long) now
							+ atol(argv[2] + 1);
		else usage();
		a.lead = 0;			/* keep it */
		set = RTCIOCSALARM;
	} else if (strcmp(argv[1], "lead") == 0) {
		a.lead = atol(argv[2]);
		if (a.lead == 0 || a.lead > 600) usage();
		if (!(a.flags & RTC_ARMED)) a.due = 0;
		set = RTCIOCSALARM;
	} else if (strcmp(argv[1], "cal") == 0) {
		cal = atoi(argv[2]);
		set = RTCIOCSCAL;
	} else usage();
	if (ioctl(fd, set, set == RTCIOCSCAL ? (void *) &cal : (void *) &a) < 0) {
		perror("rtc");
		return 1;
	}
	return 0;
}
