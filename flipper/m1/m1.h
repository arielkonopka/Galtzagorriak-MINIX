/* Milestone 1 test servers: stand-ins for MM, FS and INIT that exercise the
 * kernel (IPC, clock, TTY, privilege checks) before the real servers run. */
#define _SYSTEM 1	/* like MM and FS: negative error codes, OK */
#define _MINIX 1
#define _POSIX_SOURCE 1
#include <minix/config.h>
#include <ansi.h>
#include <sys/types.h>
#include <minix/const.h>
#include <minix/type.h>
#include <minix/com.h>
#include <minix/syslib.h>
#include <minix/callnr.h>
#include <errno.h>

#define M1_PING		1000	/* INIT -> MM: are you there? */
#define M1_UPTIME	1001	/* INIT -> MM: ask the clock for me */
#define M1_PRINT	1002	/* INIT -> FS: print ADDRESS, COUNT */
#define M1_RESULT	1003	/* INIT -> MM: number of failed checks */

int _send(int dst, message *m);
int _receive(int src, message *m);
int _sendrec(int srcdest, message *m);

int tty_write_for(int proc, char *addr, int count);
int tty_write(int me, const char *s);
int fs_print(const char *s);
void fs_print_num(unsigned long n);
void semihost_exit(int status) __attribute__((noreturn));
void exit(int status) __attribute__((noreturn));
