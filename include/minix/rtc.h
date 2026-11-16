/* rtc.h - the RTC alarm and calibration, /dev/rtc (spec R6.2, R7b.1,
 * kernel/arm/flipdev.c).  Root and group timers (the node's mode).
 *
 * RTCIOCSALARM arms Alarm A at due (seconds since 1970, UTC; at most 27
 * days ahead; 0 clears it).  When it fires the kernel sends SIGRTC to
 * INIT; while the Flipper is off it powers it on, lead seconds early (the
 * boot takes that long; 0 keeps the stored lead, first 10 s), and the
 * boot finds RTC_WOKE.  RTCIOCGALARM reads it back.  RTCIOCSCAL and
 * RTCIOCGCAL: the smooth calibration in steps of 0.954 ppm (2^-20),
 * positive runs faster, -511..512.
 */
#ifndef _MINIX_RTC_H
#define _MINIX_RTC_H

#include <sys/ioctl.h>

struct rtc_alarm {
	unsigned long due;	/* 0: none */
	unsigned long lead;	/* s */
	unsigned long flags;	/* read: RTC_* */
};
#define RTC_ARMED	1	/* it will fire */
#define RTC_FIRED	2	/* it fired since it was set */
#define RTC_WOKE	4	/* this boot came from it */

#define RTCIOCSALARM	_IOW('R', 1, struct rtc_alarm)
#define RTCIOCGALARM	_IOR('R', 2, struct rtc_alarm)
#define RTCIOCSCAL	_IOW('R', 3, int)
#define RTCIOCGCAL	_IOR('R', 4, int)

#endif /* _MINIX_RTC_H */
