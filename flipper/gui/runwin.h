/* SPDX-License-Identifier: GPL-3.0-or-later
 * runwin - a command's output in a window app (spec K.8; issue #413).
 * The stock app ports of part 2 (Sub-GHz, iButton, GPIO) are front ends
 * of the existing commands (subghz, ikey, gpio): the app starts one with
 * its stdout and stderr on a pipe and shows the last lines live, so the
 * command's own messages (a region's refusal, "no key") reach the screen.
 * See runwin.c. */
#ifndef GUI_RUNWIN_H
#define GUI_RUNWIN_H

#define RUN_LINES	6		/* lines kept (the last ones) */
#define RUN_W		40		/* longest line kept */

struct run {
	int pid;			/* the child, 0 when none */
	int fd;				/* its output, -1 when closed */
	int status;			/* exit status once it ended, -1 before */
	int n;				/* lines in l[] (at most RUN_LINES) */
	int len;			/* characters of the line being read */
	char l[RUN_LINES][RUN_W];
};

/* Start path with argv (NULL-terminated), its stdin /dev/null.  0, or -1
 * with a line in r saying why. */
int run_start(struct run *r, const char *path, char *const *argv);
/* Read what the child wrote; 1 when the lines changed. When the pipe
 * closes the child is reaped (r->status). */
int run_read(struct run *r);
void run_stop(struct run *r);		/* SIGINT, then SIGTERM; reaped */
void run_lines(const struct run *r);	/* gm_line() each kept line */
const char *run_last(const struct run *r);	/* the last line, or "" */
/* Run to the end and return the exit status (-1: did not start); the
 * lines are in r.  For short commands (gpio, subghz region). */
int run_sync(struct run *r, const char *path, char *const *argv);
/* gm_event() that also reads r's pipe while waiting: calls live() when
 * the lines changed (live may be NULL).  ev and f as gm_event's. */
int run_event(struct run *r, char *ev, int size, char **f, int nf,
	void (*live)(void));

#endif /* GUI_RUNWIN_H */
