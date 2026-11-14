/*
 * (c) copyright 1987 by the Vrije Universiteit, Amsterdam, The Netherlands.
 * See the copyright notice in the ACK home directory, in the file "Copyright".
 */
/* $Header: errlist.c,v 1.1 89/05/10 16:22:20 eck Exp $ */

#include	<errno.h>

/* The messages as one string, and where each starts: no table of
 * pointers, which a position-independent program would have to keep in
 * RAM and relocate (spec R2a.1).  strerror() is the interface.
 */
const char _sys_errstr[] =
	"Error 0\0"
	"Not owner\0"
	"No such file or directory\0"
	"No such process\0"
	"Interrupted system call\0"
	"I/O error\0"
	"No such device or address\0"
	"Arg list too long\0"
	"Exec format error\0"
	"Bad file number\0"
	"No children\0"
	"Resource temporarily unavailable\0"
	"Not enough core\0"
	"Permission denied\0"
	"Bad address\0"
	"Block device required\0"
	"Resource busy\0"
	"File exists\0"
	"Cross-device link\0"
	"No such device\0"
	"Not a directory\0"
	"Is a directory\0"
	"Invalid argument\0"
	"File table overflow\0"
	"Too many open files\0"
	"Not a typewriter\0"
	"Text file busy\0"
	"File too large\0"
	"No space left on device\0"
	"Illegal seek\0"
	"Read-only file system\0"
	"Too many links\0"
	"Broken pipe\0"
	"Math argument\0"
	"Result too large\0"
	"Resource deadlock avoided\0"
	"File name too long\0"
	"No locks available\0"
	"Function not implemented\0"
	"Directory not empty\0"
	"Unknown error\0"
	"Invalid packet size\0"
	"Not enough buffers left\0"
	"Illegal ioctl for device\0"
	"Bad mode for ioctl\0"
	"Would block\0"
	"Bad destination address\0"
	"Destination not reachable\0"
	"Already connected\0"
	"Address in use\0"
	"Connection refused\0"
	"Connection reset\0"
	"Connection timed out\0"
	"Urgent data present\0"
	"No urgent data present\0"
	"No connection\0"
	"Already shutdown\0"
	"No such connection\0";

const unsigned short _sys_erroff[] = {
	   0,	/* EGENERIC */
	   8,	/* EPERM */
	  18,	/* ENOENT */
	  44,	/* ESRCH */
	  60,	/* EINTR */
	  84,	/* EIO */
	  94,	/* ENXIO */
	 120,	/* E2BIG */
	 138,	/* ENOEXEC */
	 156,	/* EBADF */
	 172,	/* ECHILD */
	 184,	/* EAGAIN */
	 217,	/* ENOMEM */
	 233,	/* EACCES */
	 251,	/* EFAULT */
	 263,	/* ENOTBLK */
	 285,	/* EBUSY */
	 299,	/* EEXIST */
	 311,	/* EXDEV */
	 329,	/* ENODEV */
	 344,	/* ENOTDIR */
	 360,	/* EISDIR */
	 375,	/* EINVAL */
	 392,	/* ENFILE */
	 412,	/* EMFILE */
	 432,	/* ENOTTY */
	 449,	/* ETXTBSY */
	 464,	/* EFBIG */
	 479,	/* ENOSPC */
	 503,	/* ESPIPE */
	 516,	/* EROFS */
	 538,	/* EMLINK */
	 553,	/* EPIPE */
	 565,	/* EDOM */
	 579,	/* ERANGE */
	 596,	/* EDEADLK */
	 622,	/* ENAMETOOLONG */
	 641,	/* ENOLCK */
	 660,	/* ENOSYS */
	 685,	/* ENOTEMPTY */
	 705,	/* 40 */
	 705,	/* 41 */
	 705,	/* 42 */
	 705,	/* 43 */
	 705,	/* 44 */
	 705,	/* 45 */
	 705,	/* 46 */
	 705,	/* 47 */
	 705,	/* 48 */
	 705,	/* 49 */
	 719,	/* EPACKSIZE */
	 739,	/* EOUTOFBUFS */
	 763,	/* EBADIOCTL */
	 788,	/* EBADMODE */
	 807,	/* EWOULDBLOCK */
	 819,	/* EBADDEST */
	 843,	/* EDSTNOTRCH */
	 869,	/* EISCONN */
	 887,	/* EADDRINUSE */
	 902,	/* ECONNREFUSED */
	 921,	/* ECONNRESET */
	 938,	/* ETIMEDOUT */
	 959,	/* EURG */
	 979,	/* ENOURG */
	1002,	/* ENOTCONN */
	1016,	/* ESHUTDOWN */
	1033,	/* ENOCONN */
};

const int _sys_nerr = sizeof(_sys_erroff) / sizeof(_sys_erroff[0]);
