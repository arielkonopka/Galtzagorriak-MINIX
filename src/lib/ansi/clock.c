/*
 * clock - determine the processor time used
 */
/* $Header: clock.c,v 1.3 90/01/22 13:08:11 eck Exp $ */

#include	<time.h>

#if	defined(__BSD4_2)

struct timeval {
	long	tv_sec;			/* seconds */
	long	tv_usec;		/* and microseconds */
};

#define	RUSAGE_SELF	0
#define	RUSAGE_CHILDREN	-1

struct rusage {
	struct timeval ru_utime;	/* user time used */
	struct timeval ru_stime;	/* system time used */
	long	ru_maxrss;
	long	ru_ixrss;		/* integral shared memory size */
	long	ru_idrss;		/* integral unshared data size */
	long	ru_isrss;		/* integral unshared stack size */
	long	ru_minflt;		/* page reclaims */
	long	ru_majflt;		/* page faults */
	long	ru_nswap;		/* swaps */
	long	ru_inblock;		/* block input operations */
	long	ru_oublock;		/* block output operations */
	long	ru_msgsnd;		/* messages sent */
	long	ru_msgrcv;		/* messages received */
	long	ru_nsignals;		/* signals received */
	long	ru_nvcsw;		/* voluntary context switches */
	long	ru_nivcsw;		/* involuntary context switches */
};

void _getrusage(int who, struct rusage *rusage);

#elif	defined(_POSIX_SOURCE) || defined(__USG)

struct tms {			/* as <sys/times.h>: clock_t, not time_t */
	clock_t	tms_utime;		/* user time */
	clock_t	tms_stime;		/* system time */
	clock_t	tms_cutime;		/* user time, children */
	clock_t	tms_cstime;		/* system time, children */
};

clock_t _times(struct tms *buffer);

#else					/* Version 7 UNIX */

struct tbuffer {
	long proc_user_time;	/* timecheck: ok (ticks) */
	long proc_system_time;	/* timecheck: ok (ticks) */
	long child_user_time;	/* timecheck: ok (ticks) */
	long child_system_time;	/* timecheck: ok (ticks) */
};

clock_t _times(struct tbuffer *buffer);

#endif

clock_t
clock(void)
{
#if	defined(__BSD4_2)
	struct rusage rusage;

	_getrusage(RUSAGE_SELF, &rusage);

	return (((unsigned long)rusage.ru_utime.tv_sec * CLOCKS_PER_SEC)
		+ rusage.ru_utime.tv_usec);
#elif	defined(_POSIX_SOURCE) || defined(__USG)
	struct tms tms;

	_times(&tms);
	return tms.tms_utime;
#else
	struct tbuffer tbuffer;

	_times(&tbuffer);
	return tbuffer.proc_user_time;
#endif
}
