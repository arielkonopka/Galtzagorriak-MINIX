/* test_nat.c - host tests of nat_core.c (spec 7b.1, 7b.2): the remind
 * log reader nat_log_days(), times, day masks, the form cursor.
 *	make -C apps/native test	(also run by make -C tests/host) */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "nat.h"

static int fails, checks;

#define CHECK(c) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char path[] = "/tmp/test_natXXXXXX";

/* A log file of `s`; an fd open on it for reading. */
static int logfile(const char *s)
{
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);

	if (fd < 0 || write(fd, s, strlen(s)) != (ssize_t) strlen(s)) {
		perror(path);
		exit(2);
	}
	lseek(fd, 0, SEEK_SET);
	return fd;
}

static int days(const char *s, time_t day0, int n, int *c)
{
	int fd = logfile(s), r = nat_log_days(fd, day0, n, c);

	close(fd);
	return r;
}

/* A log of `n` lines: line i at t0 + i * step, ids "remind-<i % 7>". */
static char *biglog(time_t t0, int step, int n)
{
	char *s = malloc((size_t) n * 32 + 1), *p = s;
	int i;

	for (i = 0; i < n; i++)
		p += sprintf(p, "%lld remind-%d\n", (long long) t0 +
			     (long long) i * step, i % 7);
	*p = 0;
	return s;
}

/* The counts of biglog(t0, step, n) by brute force. */
static int expect(time_t t0, int step, int n, time_t day0, int nd, int *c)
{
	int i, d, total = 0;
	time_t t;

	for (d = 0; d < nd; d++) c[d] = 0;
	for (i = 0; i < n; i++) {
		t = t0 + (time_t) i * step;
		for (d = 0; d < nd; d++)
			if (t >= nat_midnight(day0, d - nd + 1)
			    && t < nat_midnight(day0, d - nd + 2)) {
				c[d]++;
				total++;
			}
	}
	return total;
}

static void test_log(void)
{
	time_t now = time(NULL), d0 = nat_midnight(now, 0);
	time_t noon = d0 + 12 * 3600;
	int c[8], e[8], i, sum;
	time_t t0;
	char buf[4096], *s;

	/* empty, bad arguments */
	CHECK(days("", noon, 7, c) == 0);
	for (sum = i = 0; i < 7; i++) sum += c[i];
	CHECK(sum == 0);
	CHECK(days("", noon, 0, c) == -1);
	CHECK(days("", noon, 9, c) == -1);
	CHECK(nat_log_days(-1, noon, 7, c) == -1);

	/* today, yesterday, 6 days ago; 7 days ago is outside the window */
	snprintf(buf, sizeof buf, "%lld remind-1\n%lld remind-2\n"
		 "%lld remind-1\n%lld remind-1\n%lld remind-3\n",
		 (long long) nat_midnight(noon, -7) + 60,
		 (long long) nat_midnight(noon, -6) + 60,
		 (long long) nat_midnight(noon, -1) + 3600,
		 (long long) d0 + 1, (long long) noon);
	CHECK(days(buf, noon, 7, c) == 4);
	CHECK(c[6] == 2 && c[5] == 1 && c[0] == 1);
	CHECK(c[1] == 0 && c[2] == 0 && c[3] == 0 && c[4] == 0);
	/* the window is whole local days: day0 anywhere in the day */
	CHECK(days(buf, d0, 7, c) == 4 && c[6] == 2);
	CHECK(days(buf, d0 + 86399, 7, c) == 4 && c[6] == 2);
	/* just today */
	CHECK(days(buf, noon, 1, c) == 2 && c[0] == 2);
	/* midnight belongs to the day it starts */
	snprintf(buf, sizeof buf, "%lld a\n%lld b\n", (long long) d0 - 1,
		 (long long) d0);
	CHECK(days(buf, noon, 2, c) == 2 && c[0] == 1 && c[1] == 1);
	/* lines after the window (tomorrow, clock set back) are skipped */
	snprintf(buf, sizeof buf, "%lld a\n%lld b\n", (long long) noon,
		 (long long) nat_midnight(noon, 1));
	CHECK(days(buf, noon, 7, c) == 1 && c[6] == 1);

	/* no newline at the end; junk lines; empty lines; CR */
	snprintf(buf, sizeof buf, "junk\n\n%lld remind-1\nx 1\n%lld\n"
		 "%lld remind-2", (long long) noon, (long long) noon,
		 (long long) noon);
	CHECK(days(buf, noon, 7, c) == 2 && c[6] == 2);

	/* an over-long line is skipped, the lines around it count */
	s = malloc(400);
	memset(s, 'x', 300);
	s[300] = 0;
	snprintf(buf, sizeof buf, "%lld a\n%lld %s\n%lld b\n",
		 (long long) noon, (long long) noon, s, (long long) noon);
	free(s);
	CHECK(days(buf, noon, 7, c) == 2 && c[6] == 2);

	/* lines cut by every chunk border (128 B): one line a minute,
	 * 3 days; each day's count is exact */
	s = biglog(nat_midnight(noon, -2), 60, 3 * 1440);
	CHECK(days(s, noon, 3, c) == 3 * 1440);
	CHECK(c[0] == 1440 && c[1] == 1440 && c[2] == 1440);
	CHECK(days(s, noon, 7, c) == 3 * 1440 && c[6] == 1440 && c[3] == 0);
	free(s);

	/* a long log costs no more: the reader stops at the first older
	 * line (one line every 10 minutes for 60 days and a bit);
	 * across a DST change the days are local days */
	t0 = nat_midnight(noon, -60);
	s = biglog(t0, 600, 60 * 144 + 10);
	CHECK(days(s, noon, 2, c) == expect(t0, 600, 60 * 144 + 10, noon, 2, e));
	CHECK(c[0] == e[0] && c[1] == e[1] && c[1] > 0);
	CHECK(days(s, noon, 8, c) == expect(t0, 600, 60 * 144 + 10, noon, 8, e));
	CHECK(memcmp(c, e, sizeof e) == 0);
	free(s);
}

/* nat_log_back(): newest first, skip and max, junk, chunk borders */
static char got[64][24];
static long long gott[64];
static int ngot;

static void take(time_t t, const char *id, void *arg)
{
	(void) arg;
	if (ngot < 64) {
		gott[ngot] = (long long) t;
		snprintf(got[ngot++], sizeof got[0], "%s", id);
	}
}

static int back(const char *s, int skip, int max)
{
	int fd = logfile(s), r;

	ngot = 0;
	r = nat_log_back(fd, skip, max, take, NULL);
	close(fd);
	return r;
}

static void test_back(void)
{
	char buf[256], *s;
	int i, ok;

	CHECK(back("", 0, 8) == 0 && ngot == 0);
	CHECK(nat_log_back(-1, 0, 8, take, NULL) == -1);
	CHECK(back("10 a\n20 b\n30 c\n", 0, 8) == 3 && ngot == 3);
	CHECK(strcmp(got[0], "c") == 0 && gott[0] == 30);
	CHECK(strcmp(got[2], "a") == 0 && gott[2] == 10);
	/* a page: skip 1, take 1; the count stops at skip + max */
	CHECK(back("10 a\n20 b\n30 c\n", 1, 1) == 2 && ngot == 1
	      && strcmp(got[0], "b") == 0);
	/* counting only (fn NULL) */
	{
		int fd = logfile("10 a\n20 b\n30 c\n");

		CHECK(nat_log_back(fd, 0, 2, NULL, NULL) == 2);
		close(fd);
	}
	/* junk, empty lines, no newline at the end */
	CHECK(back("junk\n\n10 a\nx 1\n15\n20 b", 0, 8) == 2
	      && strcmp(got[0], "b") == 0 && strcmp(got[1], "a") == 0);
	/* an over-long line is skipped */
	s = malloc(400);
	memset(s, 'x', 300);
	s[300] = 0;
	snprintf(buf, sizeof buf, "10 a\n20 %.200s\n30 b\n", s);
	free(s);
	CHECK(back(buf, 0, 8) == 2 && strcmp(got[0], "b") == 0
	      && strcmp(got[1], "a") == 0);
	/* lines cut by chunk borders: the last 50 of 3 days, in order */
	s = biglog(1000000, 60, 3 * 1440);
	CHECK(back(s, 10, 50) == 60 && ngot == 50);
	for (ok = 1, i = 0; i < 50; i++)
		if (gott[i] != 1000000 + 60LL * (3 * 1440 - 11 - i)) ok = 0;
	CHECK(ok);
	free(s);
}

static void test_misc(void)
{
	char b[16];
	struct nat_form f;
	time_t t = time(NULL);

	CHECK(strcmp(nat_hm(b, 0), "00:00") == 0);
	CHECK(strcmp(nat_hm(b, 7 * 60 + 5), "07:05") == 0);
	CHECK(strcmp(nat_hm(b, 1439), "23:59") == 0);
	CHECK(strcmp(nat_hm(b, 1440), "24:00") == 0);
	CHECK(nat_daybit(0) == 2 && nat_daybit(6) == 1);	/* Mo, Su */
	CHECK(strcmp(nat_days(b, 0x3e), "MTWTF--") == 0);
	CHECK(strcmp(nat_days(b, 0x41), "-----SS") == 0);
	CHECK(strcmp(nat_days(b, 0), "-------") == 0);
	CHECK(strcmp(nat_num(b, 0), "0") == 0);
	CHECK(strcmp(nat_num(b, 4294967295u), "4294967295") == 0);
	CHECK(nat_midnight(t, 0) <= t && t - nat_midnight(t, 0) < 86400 + 3600);
	CHECK(nat_midnight(nat_midnight(t, 0), 0) == nat_midnight(t, 0));
	CHECK(nat_midnight(t, 1) > nat_midnight(t, 0));

	memset(&f, 0, sizeof f);
	f.n = 8;
	nat_form_move(&f, -1);			/* wraps to the last */
	CHECK(f.cur == 7 && f.top == 7 - NAT_ROWS + 1);
	nat_form_move(&f, 1);			/* wraps to the first */
	CHECK(f.cur == 0 && f.top == 0);
	for (int i = 0; i < NAT_ROWS; i++) nat_form_move(&f, 1);
	CHECK(f.cur == NAT_ROWS && f.top == 1);
}

int main(void)
{
	int fd = mkstemp(path);

	if (fd < 0) {
		perror("mkstemp");
		return 2;
	}
	close(fd);
	/* a zone with DST: the windows are local days, not 86400 s */
	setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
	tzset();
	test_log();
	test_misc();
	test_back();
	setenv("TZ", "UTC0", 1);
	tzset();
	test_log();
	unlink(path);
	printf("test_nat: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
