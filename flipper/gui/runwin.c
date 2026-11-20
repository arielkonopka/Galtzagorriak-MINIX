/* SPDX-License-Identifier: GPL-3.0-or-later
 * runwin - a command's output in a window app (issue #413), see runwin.h.
 * No stdio, no malloc: the caller's struct run (about 260 B) holds all.
 */
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include "gm.h"
#include "runwin.h"

static void put_line(struct run *r)
{
	if (r->len == 0) return;
	r->l[r->n < RUN_LINES ? r->n : RUN_LINES - 1][r->len] = 0;
	r->len = 0;
	if (r->n < RUN_LINES) r->n++;
}

/* A new line goes at index n (scrolling the oldest out when full). */
static char *cur(struct run *r)
{
	if (r->n == RUN_LINES && r->len == 0) {
		memmove(r->l[0], r->l[1], (RUN_LINES - 1) * RUN_W);
		r->n--;
	}
	return r->l[r->n];
}

static void say(struct run *r, const char *s)
{
	char *p = cur(r);

	strncpy(p, s, RUN_W - 1);
	r->len = strlen(s) < RUN_W - 1 ? (int) strlen(s) : RUN_W - 1;
	put_line(r);
}

int run_start(struct run *r, const char *path, char *const *argv)
{
	int p[2], nul;

	r->pid = 0;
	r->fd = -1;
	r->status = -1;
	r->n = r->len = 0;
	if (pipe(p) < 0) { say(r, "no pipe"); return -1; }
	if ((r->pid = fork()) == 0) {
		nul = open("/dev/null", O_RDONLY);
		if (nul >= 0) dup2(nul, 0);
		dup2(p[1], 1);
		dup2(p[1], 2);
		close(GM_EVFD);			/* the desktop's pipes stay ours */
		close(GM_RECFD);
		close(p[0]);
		close(p[1]);
		execv(path, argv);
		(void) write(2, "not installed: ", 15);
		(void) write(2, path, strlen(path));
		(void) write(2, "\n", 1);
		_exit(127);
	}
	close(p[1]);
	if (r->pid < 0) {
		r->pid = 0;
		close(p[0]);
		say(r, "cannot start it (no memory?)");
		return -1;
	}
	r->fd = p[0];
	return 0;
}

static void reap(struct run *r)
{
	int st;

	if (r->fd >= 0) { close(r->fd); r->fd = -1; }
	if (r->pid > 0 && waitpid(r->pid, &st, 0) == r->pid)
		r->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
	r->pid = 0;
}

int run_read(struct run *r)
{
	char b[64];
	int n, i, changed = 0;

	if (r->fd < 0) return 0;
	n = read(r->fd, b, sizeof b);
	if (n < 0 && errno == EINTR) return 0;
	if (n <= 0) {
		put_line(r);
		reap(r);
		return 1;
	}
	for (i = 0; i < n; i++) {
		char *p;

		if (b[i] == '\r') continue;
		if (b[i] == '\n') { put_line(r); changed = 1; continue; }
		p = cur(r);
		if (r->len < RUN_W - 1) p[r->len++] = b[i] == '\t' ? ' ' : b[i];
	}
	return changed;
}

void run_stop(struct run *r)
{
	int i;

	if (r->pid <= 0) { reap(r); return; }
	(void) kill(r->pid, SIGINT);
	for (i = 0; i < 20 && r->fd >= 0; i++) {	/* ~2 s for its last words */
		struct pollfd p;

		p.fd = r->fd;
		p.events = POLLIN;
		p.revents = 0;
		if (poll(&p, 1, 100) > 0) (void) run_read(r);
	}
	if (r->pid > 0) (void) kill(r->pid, SIGTERM);
	reap(r);
}

void run_lines(const struct run *r)
{
	int i;

	for (i = 0; i < r->n; i++) gm_line(r->l[i][0] ? r->l[i] : " ");
}

const char *run_last(const struct run *r)
{
	return r->n > 0 ? r->l[r->n - 1] : "";
}

int run_sync(struct run *r, const char *path, char *const *argv)
{
	if (run_start(r, path, argv) < 0) return -1;
	while (r->fd >= 0) (void) run_read(r);
	return r->status;
}

int run_event(struct run *r, char *ev, int size, char **f, int nf,
	void (*live)(void))
{
	struct pollfd p[2];

	while (r->fd >= 0 && !gm_pending(ev)) {
		p[0].fd = GM_EVFD;
		p[0].events = POLLIN;
		p[0].revents = 0;
		p[1].fd = r->fd;
		p[1].events = POLLIN;
		p[1].revents = 0;
		if (poll(p, 2, -1) <= 0) continue;
		if (p[1].revents != 0 && run_read(r) && live != NULL) live();
		if (p[0].revents != 0) break;
	}
	return gm_event(ev, size, f, nf);
}
