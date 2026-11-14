/* t.h - helpers for the security tests (Galtzagorriak MINIX, spec rule 0.4)
 *
 * Each test tries an attack in a child process and reports PASS when the
 * system refuses it and keeps running, FAIL otherwise.  Exit status: the
 * number of failures.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int t_fail;

static void t_result(char *what, int ok, char *how)
{
	printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, how ? ": " : "",
		how ? how : "");
	fflush(stdout);
	if (!ok) t_fail++;
}

/* Run fn() in a child; returns its wait status, or -1. */
static int t_child(void (*fn)(void))
{
	int pid, st;

	fflush(stdout);
	if ((pid = fork()) < 0) return -1;
	if (pid == 0) {
		fn();
		_exit(0);
	}
	if (waitpid(pid, &st, 0) != pid) return -1;
	return st;
}

static char *t_status(int st)
{
	static char buf[40];

	if (st == -1) strcpy(buf, "fork/wait failed");
	else if (WIFSIGNALED(st)) sprintf(buf, "killed by signal %d",
							WTERMSIG(st));
	else sprintf(buf, "exit %d", WEXITSTATUS(st));
	return buf;
}

/* Copy a file; mode as given. */
static int t_copy(char *from, char *to, int mode)
{
	char buf[512];
	int in, out, n;

	unlink(to);
	if ((in = open(from, O_RDONLY)) < 0) return -1;
	if ((out = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode)) < 0) {
		close(in);
		return -1;
	}
	while ((n = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, n) != n) { n = -1; break; }
	close(in);
	close(out);
	chmod(to, mode);
	return n < 0 ? -1 : 0;
}

/* a.out header fields (include/a.out.h), read straight from a file. */
struct t_hdr { unsigned char magic[2], flags, cpu, hdrlen, unused;
	       unsigned short version;
	       long text, data, bss, entry, total, syms; };

static int t_header(char *file, struct t_hdr *h)
{
	int fd, n;

	if ((fd = open(file, O_RDONLY)) < 0) return -1;
	n = read(fd, (char *) h, sizeof(*h));
	close(fd);
	return n == sizeof(*h) ? 0 : -1;
}

static int t_patch(char *file, long off, void *data, int len)
{
	int fd, n;

	if ((fd = open(file, O_WRONLY)) < 0) return -1;
	if (lseek(fd, off, SEEK_SET) != off) { close(fd); return -1; }
	n = write(fd, data, len);
	close(fd);
	return n == len ? 0 : -1;
}
