/* nat_core.c - the native apps' shared helpers without the GUI (spec
 * 7b.1, 7b.2); host-tested by test_nat.c (make test).  See nat.h. */
#include <sys/types.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __minix
#include <sys/ioctl.h>
#include <minix/fg.h>
#endif
#include "nat.h"

char *nat_hm(char *b, int m)
{
	if (m >= 1440) {
		strcpy(b, "24:00");
		return b;
	}
	b[0] = '0' + m / 600; b[1] = '0' + m / 60 % 10; b[2] = ':';
	b[3] = '0' + m % 60 / 10; b[4] = '0' + m % 10; b[5] = 0;
	return b;
}

int nat_daybit(int pos)
{
	return 1 << (pos + 1) % 7;
}

char *nat_days(char *b, int mask)
{
	static const char l[] = "MTWTFSS";
	int i;

	for (i = 0; i < 7; i++) b[i] = mask & nat_daybit(i) ? l[i] : '-';
	b[7] = 0;
	return b;
}

time_t nat_midnight(time_t t, int off)
{
	struct tm tm = *localtime(&t);

	tm.tm_mday += off;
	tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
	tm.tm_isdst = -1;
	return mktime(&tm);
}

char *nat_num(char *b, unsigned v)
{
	char t[12], *p = t + sizeof t;

	*--p = 0;
	do *--p = '0' + v % 10; while ((v /= 10) != 0);
	strcpy(b, p);
	return b;
}

/* The uid and gid of user `name` in /etc/passwd ("name:pw:uid:gid:..."),
 * read here in small pieces: getpwnam()'s buffers would stay in the
 * app's RAM (0.8 KB) for good. */
static int passwd(const char *name, uid_t *uid, gid_t *gid)
{
	char line[96], buf[64], c = 0, *p, *q;
	int fd, n = 0, l = strlen(name), r = -1, have = 0, at = 0, got;

	if ((fd = open("/etc/passwd", O_RDONLY)) < 0) return -1;
	for (;;) {
		if (at == have) {
			have = read(fd, buf, sizeof buf);
			at = 0;
		}
		got = have > 0;
		if (got) c = buf[at++];
		if (got && c != '\n') {
			if (n < (int) sizeof line - 1) line[n++] = c;
			continue;
		}
		line[n] = 0;
		if (n > l && strncmp(line, name, l) == 0 && line[l] == ':'
		    && (p = strchr(line + l + 1, ':')) != NULL
		    && (q = strchr(p + 1, ':')) != NULL) {
			*uid = atoi(p + 1);
			*gid = atoi(q + 1);
			r = 0;
			break;
		}
		if (!got) break;
		n = 0;
	}
	close(fd);
	return r;
}

#ifdef __minix
static int fg_fd = -1;		/* /dev/lcd, kept for nat_background() */
#endif

int nat_foreground(void)
{
#ifdef __minix
	int me = getpid(), r;

	if (fg_fd < 0 && (fg_fd = open("/dev/lcd", O_RDONLY)) < 0) return -1;
	r = ioctl(fg_fd, FGIOCSET, &me);
	if (r < 0) {
		close(fg_fd);
		fg_fd = -1;
	}
	return r;
#else
	return 0;
#endif
}

void nat_background(void)
{
#ifdef __minix
	int none = 0;

	if (fg_fd < 0) return;
	(void) ioctl(fg_fd, FGIOCSET, &none);
	close(fg_fd);
	fg_fd = -1;
#endif
}

int nat_become(const char *name)
{
	uid_t uid;
	gid_t gid;

	if (geteuid() != 0) return 0;		/* already someone */
	if (passwd(name, &uid, &gid) < 0 || uid == 0) return -1;
	if (setgid(gid) < 0 || setuid(uid) < 0 || getuid() != uid
	    || geteuid() != uid) return -1;
	return 0;
}

void nat_form_move(struct nat_form *f, int d)
{
	f->cur = (f->cur + d + f->n) % f->n;
	if (f->cur < f->top) f->top = f->cur;
	if (f->cur >= f->top + NAT_ROWS) f->top = f->cur - NAT_ROWS + 1;
}

/* One log line (s, n bytes, no newline) into the counts. */
static int log_line(const char *s, int n, const time_t *start, int days,
								int *counts)
{
	time_t t = 0;
	int i, d = 0;

	for (i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++, d++)
		t = t * 10 + s[i] - '0';
	if (d == 0 || i >= n || s[i] != ' ') return 0;	/* not a line */
	if (t < start[0]) return -1;		/* older: stop here */
	if (t >= start[days]) return 0;		/* after the window */
	for (i = days - 1; t < start[i]; i--) ;
	counts[i]++;
	return 1;
}

#define LOG_CHUNK	128
#define LOG_LINE	64

int nat_log_days(int fd, time_t day0, int days, int *counts)
{
	char buf[LOG_CHUNK + LOG_LINE];
	time_t start[9];
	off_t pos;
	int carry = 0, skip = 0, n, i, e, r, total = 0;

	if (days < 1 || days > 8) return -1;
	for (i = 0; i < days; i++) {
		counts[i] = 0;
		start[i] = nat_midnight(day0, i - days + 1);
	}
	start[days] = nat_midnight(day0, 1);
	if ((pos = lseek(fd, (off_t) 0, SEEK_END)) < 0) return -1;
	while (pos > 0) {
		n = pos > LOG_CHUNK ? LOG_CHUNK : (int) pos;
		pos -= n;
		/* buf: this chunk, then the part of a line it cut off */
		memmove(buf + n, buf, carry);
		if (lseek(fd, pos, SEEK_SET) < 0 || read(fd, buf, n) != n)
			return -1;
		n += carry;
		/* complete lines, from the end */
		for (e = n; ; ) {
			for (i = e - 1; i >= 0 && buf[i] != '\n'; i--) ;
			if (i < 0 && pos > 0) break;	/* cut: the next chunk */
			if (skip)		/* the head of a long line */
				skip = 0;
			else if (e - i - 1 > 0) {
				if ((r = log_line(buf + i + 1, e - i - 1, start,
						days, counts)) < 0) return total;
				total += r;
			}
			if (i < 0) break;
			e = i;
		}
		carry = e;		/* buf[0, e): the start of a line */
		if (carry > LOG_LINE) {		/* too long: not ours */
			carry = 0;
			skip = 1;	/* nor is the rest, in the next chunk */
		}
	}
	return total;
}

/* The log's lines newest first: lines skip .. skip + max - 1 (only "TIME
 * ID" lines count) go to fn, if fn is not NULL.  Reads from the end in
 * chunks, only as far back as skip + max lines: a long log costs no more
 * than its last lines (spec K.8: the app's RAM does not grow with the
 * data).  The lines counted (at most skip + max), or -1. */
int nat_log_back(int fd, int skip, int max, void (*fn)(time_t t,
					const char *id, void *arg), void *arg)
{
	char buf[LOG_CHUNK + LOG_LINE], id[LOG_LINE];
	off_t pos;
	time_t t;
	int carry = 0, cut = 0, n, i, e, j, k, seen = 0;

	if ((pos = lseek(fd, (off_t) 0, SEEK_END)) < 0) return -1;
	while (pos > 0 && seen < skip + max) {
		n = pos > LOG_CHUNK ? LOG_CHUNK : (int) pos;
		pos -= n;
		memmove(buf + n, buf, carry);
		if (lseek(fd, pos, SEEK_SET) < 0 || read(fd, buf, n) != n)
			return -1;
		n += carry;
		for (e = n; seen < skip + max; ) {
			for (i = e - 1; i >= 0 && buf[i] != '\n'; i--) ;
			if (i < 0 && pos > 0) break;
			if (cut)
				cut = 0;
			else if (e - i - 1 > 0) {
				/* "TIME ID": digits, one space, 1-14 chars */
				t = 0;
				for (j = i + 1; j < e && buf[j] >= '0'
						&& buf[j] <= '9'; j++)
					t = t * 10 + buf[j] - '0';
				k = e - j - 1;
				if (j > i + 1 && j < e && buf[j] == ' '
				    && k > 0 && k < LOG_LINE) {
					if (fn != NULL && seen >= skip) {
						memcpy(id, buf + j + 1, k);
						id[k] = 0;
						fn(t, id, arg);
					}
					seen++;
				}
			}
			if (i < 0) break;
			e = i;
		}
		carry = e;
		if (carry > LOG_LINE) {
			carry = 0;
			cut = 1;
		}
	}
	return seen;
}
