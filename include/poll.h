/* The <poll.h> header: wait for one of several file descriptors (POSIX
 * poll(), spec R4b.6).  Pipes and FIFOs are checked by FS itself;
 * character devices are asked with DEV_SELECT (<minix/com.h>), and a
 * device that does not answer it counts as always ready.  Regular files,
 * directories and block devices are always ready.  The timeout is in
 * milliseconds (-1: none) and is rounded up to the clock tick (1/60 s).
 */

#ifndef _POLL_H
#define _POLL_H

#ifndef _ANSI_H
#include <ansi.h>
#endif

typedef unsigned int nfds_t;

struct pollfd {
  int fd;			/* the descriptor; < 0: ignored */
  short events;			/* what to wait for */
  short revents;		/* what happened (set by poll) */
};

#define POLLIN		0x0001	/* data to read */
#define POLLPRI		0x0002	/* urgent data (never set) */
#define POLLOUT		0x0004	/* writing will not block */
#define POLLERR		0x0008	/* error (always reported) */
#define POLLHUP		0x0010	/* hung up: no writer left (always reported) */
#define POLLNVAL	0x0020	/* fd not open (always reported) */
#define POLLRDNORM	0x0040	/* same as POLLIN */
#define POLLRDBAND	0x0080	/* (never set) */
#define POLLWRNORM	0x0100	/* same as POLLOUT */
#define POLLWRBAND	0x0200	/* (never set) */

#define INFTIM		(-1)	/* no timeout */

_PROTOTYPE( int poll, (struct pollfd *_fds, nfds_t _nfds, int _timeout)	);

#endif /* _POLL_H */
