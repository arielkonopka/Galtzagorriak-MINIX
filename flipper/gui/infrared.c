/* SPDX-License-Identifier: GPL-3.0-or-later
 * infrared - the stock Infrared app on Galtzagorriak MINIX (spec Phase 7;
 * issue #389): Learn new remote, Saved remotes, Universal remotes, Debug,
 * as in stock, with the stock protocols (src/lib/infrared, libir) and the
 * stock .ir files under /home/flipper/infrared, the universal databases in
 * /home/flipper/infrared/assets (stock's own files: tv.ir, audio.ir, ...).
 * A database is streamed signal by signal (irfile.c), never loaded.
 *
 *	infrared		the app (from the launcher)
 *	infrared -L		the same, with the loopback device (QEMU)
 *	infrared -t FILE	self-test: every signal of FILE through
 *				encode -> /dev/ir frame -> decode, over the
 *				loopback; prints "infrared: N ok, M bad"
 *	infrared -c [FILE]	open /dev/ir as the app would; exit 0 if it
 *				opened (AC7.3: 1 for a user outside "ir");
 *				with FILE, then also -t FILE (the M2 test runs
 *				the app only twice: a third run would copy it
 *				to the flash text cache)
 *
 * /dev/ir is group "ir" (AC7.3).  MINIX 2.0 has one group per process, and
 * apps run in group "gui" (the screen), so infrared is setgid "ir": it opens
 * /dev/ir first, only when the real user is in group "ir" in /etc/group
 * (by its primary group or by name in the member list) or is root, then
 * goes back to its real group before it opens the screen.
 *
 * The device: /dev/ir (kernel/arm/flipdev.c), 32-bit durations, mark
 * first; a first word with bit 31 set gives the carrier (bits 0-19) and the
 * duty (bits 20-26, %).  A read blocks until a frame ends (30 ms without
 * carrier); Learn and Debug read in a child (this program again, as
 * `infrared -R IRFD PIPEFD`: its own image, see reader_main) that passes
 * frames up a pipe, so the buttons stay live.  -L replaces the device by a
 * loopback in this process: a sent frame is the next frame read.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <minix/ir.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gui.h"
#include "ui.h"
#include "irfile.h"

#define DIR_IR	"/home/flipper/infrared"
#define DIR_DB	DIR_IR "/assets"
#define DEV_IR	"/dev/ir"
#define IR_FREQ	0x80000000UL		/* first word: carrier, not a time */
#define MAXT	IRF_RAW_MAX		/* timings per frame (stock's limit) */
#define PATHMAX	96

/* The frame: t[0] the carrier word, t[1..] the timings.  4 KB of .bss:
 * the app's one big buffer (swapped out with it when idle). */
static uint32_t t[MAXT + 1];
static int irfd = -1;			/* /dev/ir, or -1 */
static int loopback;			/* -L, -t: no device */
static size_t lb_n;			/* loopback: timings waiting */
static InfraredDecoderHandler *dec;
static InfraredEncoderHandler *enc;
static int nsent;			/* signals sent (Universal) */

/* ---- group "ir" (AC7.3) ---- */

/* Field f (0-based) of line into out; 0, or -1 when there is none. */
static int field(const char *line, int f, char *out, int osz)
{
	int n = 0;

	while (f > 0 && *line) if (*line++ == ':') f--;
	if (f > 0) return -1;
	while (*line && *line != ':' && *line != '\n' && n < osz - 1)
		out[n++] = *line++;
	out[n] = 0;
	return 0;
}

/* Is the real user in group "ir"?  Read line by line (no getgrnam(): it
 * keeps 1.3 KB of buffers). */
static int ir_member(void)
{
	char line[128], name[16], v[96], sb[128], *p;
	FILE *fp;
	long gid = -1;
	uid_t uid = getuid();
	int in = 0;

	if (uid == 0) return 1;
	name[0] = 0;
	/* sb: a stack buffer for both files, not stdio's 1 KB from the heap
	 * (it would stay a hole below the self-test's 4 KB, #389) */
	if ((fp = fopen("/etc/passwd", "r")) != NULL) {
		setvbuf(fp, sb, _IOFBF, sizeof sb);
		while (fgets(line, sizeof line, fp))
			if (field(line, 2, v, sizeof v) == 0
				&& atol(v) == (long) uid) {
				field(line, 0, name, sizeof name);
				break;
			}
		fclose(fp);
	}
	if ((fp = fopen("/etc/group", "r")) == NULL) return 0;
	setvbuf(fp, sb, _IOFBF, sizeof sb);
	while (fgets(line, sizeof line, fp)) {
		if (field(line, 0, v, sizeof v) || strcmp(v, "ir")) continue;
		field(line, 2, v, sizeof v);
		gid = atol(v);
		field(line, 3, v, sizeof v);
		for (p = strtok(v, ", \n"); p && name[0]; p = strtok(NULL, ", \n"))
			if (!strcmp(p, name)) in = 1;
		break;
	}
	fclose(fp);
	return in || (gid >= 0 && (long) getgid() == gid);
}

/* Open /dev/ir as the user may, then drop the "ir" group for good. */
static void ir_open(void)
{
	if (!loopback && ir_member()) irfd = open(DEV_IR, O_RDWR);
	if (getegid() != getgid() && setgid(getgid()) < 0) exit(1);
}

/* ---- the device ---- */

/* Send t[1..n] (mark first) with carrier hz and duty %. */
static int ir_send(size_t n, unsigned long hz, unsigned duty)
{
	if (n % 2 == 0) n--;			/* ends in a space: drop it */
	if (n == 0 || n > MAXT) return -1;
	if (loopback) { lb_n = n; return 0; }
	if (irfd < 0) return -1;
	t[0] = IR_FREQ | hz | (unsigned long) duty << 20;
	return write(irfd, (char *) t, (n + 1) * 4) == (int) ((n + 1) * 4) ? 0 : -1;
}

/* A frame into t[1..]; the count, or -1. */
static long ir_recv(int fd)
{
	int r;

	if (loopback) { r = (int) lb_n; lb_n = 0; return r ? r : -1; }
	r = read(fd, (char *) (t + 1), MAXT * 4);
	return r > 0 ? r / 4 : -1;	/* errno stays from the read */
}

/* Encode a parsed message as stock sends it (infrared_send: at least the
 * protocol's repeat count, the repeats chained) into t[1..].  Count or 0. */
static size_t encode(const InfraredMessage *m)
{
	size_t n = 0, rep = infrared_get_protocol_min_repeat_count(m->protocol);
	uint32_t dur;
	bool lv, last = false;
	InfraredStatus st;
	int first = 1;

	if (rep < 1) rep = 1;
	infrared_reset_encoder(enc, m);
	while (rep > 0) {
		st = infrared_encode(enc, &dur, &lv);
		if (st == InfraredStatusError) return 0;
		if (first && !lv) {		/* a leading space: not sent */
			if (st == InfraredStatusDone) rep--;
			continue;
		}
		if (first || lv != last) {
			if (++n > MAXT) return 0;
			t[n] = 0;
		}
		first = 0;
		last = lv;
		t[n] += dur;
		if (st == InfraredStatusDone) rep--;
	}
	return n;
}

static int send_sig(const struct irf_sig *s)
{
	size_t n;

	nsent++;
	if (s->type == IRF_PARSED) {
		if ((n = encode(&s->msg)) == 0) return -1;
		return ir_send(n, infrared_get_protocol_frequency(s->msg.protocol), 0);
	}
	if (s->type != IRF_RAW || s->n == 0) return -1;
	if (s->data != t + 1) memmove(t + 1, s->data, s->n * 4);
	return ir_send(s->n, s->freq, (unsigned) ((s->duty + 5000) / 10000));
}

/* Decode t[1..n] (mark first).  1 with *m, 0 when not known. */
static int decode(size_t n, InfraredMessage *m)
{
	const InfraredMessage *r = NULL;
	size_t i;

	infrared_reset_decoder(dec);
	for (i = 1; i <= n && !r; i++)
		r = infrared_decode(dec, (i & 1) != 0, t[i]);
	if (!r) r = infrared_decode(dec, false, INFRARED_RAW_RX_TIMING_DELAY_US);
	if (!r) r = infrared_check_decoder_ready(dec);
	if (!r) return 0;
	*m = *r;
	return 1;
}

static void describe(const InfraredMessage *m, char *l1, char *l2)
{
	sprintf(l1, "%s", infrared_get_protocol_name(m->protocol));
	sprintf(l2, "A:%lX C:%lX", (unsigned long) m->address,
		(unsigned long) m->command);
}

/* ---- the reader child (Learn, Debug) ---- */

static pid_t rpid = -1;
static int rfd = -1;
static const char *self = "/bin/infrared";	/* argv[0] if a path */

static void rd_alarm(int sig)
{
	(void) sig;
}

/* The reader (infrared -R IRFD PIPEFD, started by reader_start): reads
 * frames from /dev/ir and writes each to the pipe as a count word and the
 * timings.  It is a program of its own, not a fork()ed copy: a process
 * waiting in a read of /dev/ir is pinned in memory (the DMA goes into its
 * buffer), and a pinned fork copy can never be swapped with its parent,
 * which then never runs (#416: the frozen screen, Back not seen).
 * A killed app (Back held 4 s) leaves the reader behind, and it would hold
 * /dev/ir for ever (every later read: EBUSY): a 1 s alarm ends its read,
 * and it ends itself when the app is gone. */
static int reader_main(int ifd, int pfd)
{
	struct sigaction sa;
	pid_t parent = getppid();
	long n;
	uint32_t c;
	int en;

	sa.sa_handler = rd_alarm;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGALRM, &sa, NULL);
	/* as stock's raw receiver: a remote's repeat bursts belong to the
	 * signal that Learn saves (not fatal on an older kernel) */
	en = IR_GAP_LEARN;
	(void) ioctl(ifd, IRIOCSGAP, &en);
	for (;;) {
		alarm(1);
		n = ir_recv(ifd);
		en = errno;
		alarm(0);
		if (n < 0 && en == EINTR) {
			if (getppid() != parent) return 0;
			continue;
		}
		if (n < 0) return 1;
		c = (uint32_t) n;
		if (write(pfd, (char *) &c, 4) != 4) return 0;
		if (write(pfd, (char *) (t + 1), n * 4) != n * 4) return 0;
	}
}

/* Start the reader: this program again, with /dev/ir and the pipe. */
static int reader_start(void)
{
	char a1[8], a2[8];		/* on the stack: the bss budget */
	char *av[5];
	int p[2];

	if (loopback) return 0;
	if (irfd < 0 || pipe(p) < 0) return -1;
	sprintf(a1, "%d", irfd);
	sprintf(a2, "%d", p[1]);
	av[0] = (char *) self;
	av[1] = "-R";
	av[2] = a1;
	av[3] = a2;
	av[4] = NULL;
	if ((rpid = vfork()) < 0) { close(p[0]); close(p[1]); return -1; }
	if (rpid == 0) {
		close(p[0]);
		execv(self, av);
		_exit(127);
	}
	close(p[1]);
	rfd = p[0];
	return 0;
}

static void reader_stop(void)
{
	int st;

	if (rpid > 0) {
		kill(rpid, SIGKILL);
		waitpid(rpid, &st, 0);
	}
	if (rfd >= 0) close(rfd);
	rpid = -1;
	rfd = -1;
}

/* Read all of n bytes from the pipe. */
static int readall(int fd, char *b, long n)
{
	long r;

	while (n > 0) {
		if ((r = read(fd, b, n)) <= 0) return -1;
		b += r;
		n -= r;
	}
	return 0;
}

/* Wait for a frame or a key.  The count of timings in t[1..], 0 on a key
 * (*key), -1 on an error. */
static long wait_frame(int *key)
{
	struct gui_in in;
	uint32_t c;
	int k;

	memset(&in, 0, sizeof in);
	if (loopback) { *key = gui_key() & ~KEY_LONG; return 0; }
	k = gui_wait(&in, 0, -1, rfd);
	if (k != GUI_FD) { *key = k & ~KEY_LONG; return 0; }
	if (readall(rfd, (char *) &c, 4) < 0 || c > MAXT) return -1;
	if (readall(rfd, (char *) (t + 1), (long) c * 4) < 0) return -1;
	return (long) c;
}

/* ---- remote files ---- */

static int ensure_dir(void)
{
	mkdir("/home/flipper", 0755);
	mkdir(DIR_IR, 0755);
	return access(DIR_IR, W_OK);
}

/* Append signal s to remote file path (made with its header if new). */
static int save_sig(const char *path, struct irf_sig *s)
{
	FILE *fp;
	int isnew = access(path, F_OK) != 0, r;

	if ((fp = fopen(path, "a")) == NULL) return -1;
	r = 0;
	if (isnew) r = irf_write_header(fp, IRF_SIGNALS);
	if (r == 0) r = irf_write(fp, s);
	if (fclose(fp) != 0) r = -1;
	return r;
}

/* The next free "Remote_N.ir" in DIR_IR into path. */
static void new_remote(char *path)
{
	int k;

	for (k = 1; k < 1000; k++) {
		sprintf(path, "%s/Remote_%d.ir", DIR_IR, k);
		if (access(path, F_OK) != 0) return;
	}
}

/* Count the signals in path (or -1). */
static int count_sigs(const char *path)
{
	FILE *fp = fopen(path, "r");
	struct irf f;
	struct irf_sig s;
	int n = 0, r;

	if (!fp) return -1;
	if (irf_open(&f, fp) < 0) { fclose(fp); return -1; }
	s.data = t + 1;
	s.cap = MAXT;
	while ((r = irf_next(&f, &s)) != 0) if (r > 0) n++;
	fclose(fp);
	return n;
}

/* Signal i (0-based, good ones only) of path into *s (timings in t). */
static int get_sig(const char *path, int i, struct irf_sig *s)
{
	FILE *fp = fopen(path, "r");
	struct irf f;
	int r, k = 0;

	if (!fp) return -1;
	if (irf_open(&f, fp) < 0) { fclose(fp); return -1; }
	s->data = t + 1;
	s->cap = MAXT;
	while ((r = irf_next(&f, s)) != 0)
		if (r > 0 && k++ == i) { fclose(fp); return 0; }
	fclose(fp);
	return -1;
}

/* ---- Learn ---- */

/* Does remote file path already have a button called name? */
static int has_sig(const char *path, const char *name)
{
	FILE *fp = fopen(path, "r");
	struct irf f;
	struct irf_sig s;
	int r = 0;

	if (!fp) return 0;
	if (irf_open(&f, fp) == 0) {
		s.data = t + 1;
		s.cap = MAXT;
		r = irf_find(&f, name, &s);
	}
	fclose(fp);
	return r;
}

/* Does remote file path already have a parsed button with msg's command? */
static int has_cmd(const char *path, const InfraredMessage *m)
{
	FILE *fp = fopen(path, "r");
	struct irf f;
	struct irf_sig s;
	int r, found = 0;

	if (!fp) return 0;
	if (irf_open(&f, fp) == 0) {
		s.data = t + 1;
		s.cap = MAXT;
		while (!found && (r = irf_next(&f, &s)) != 0)
			found = r > 0 && s.type == IRF_PARSED
			    && s.msg.protocol == m->protocol
			    && s.msg.address == m->address
			    && s.msg.command == m->command;
	}
	fclose(fp);
	return found;
}

static void offer_library(const char *path, const InfraredMessage *lm, int nl);

#define LIB_AFTER	4	/* parsed buttons learned before the library is asked */

/* Learn buttons into remote file path (new or existing). */
static void learn_into(const char *path)
{
	char l1[24], l2[32];
	InfraredMessage lm[LIB_AFTER];
	int nl = 0, asked = 0;
	struct irf_sig s;
	InfraredMessage m;
	long n;
	int key, nb = 0;

	if (irfd < 0 && !loopback) { ui_note("cannot open /dev/ir", "group ir?"); return; }
	if (reader_start() < 0) { ui_note("cannot start", "the reader"); return; }
	for (;;) {
		ui_msg("Point the remote", "at the IR window, press");
		gui_led(40, 0, 0);
		n = wait_frame(&key);
		gui_led(0, 0, 0);
		if (n < 0) { ui_note("read error", NULL); break; }
		if (n == 0) { if (key == KEY_BACK) break; continue; }
		memset(&s, 0, sizeof s);
		if (decode((size_t) n, &m)) {
			s.type = IRF_PARSED;
			s.msg = m;
			s.msg.repeat = false;
			describe(&m, l1, l2);
		} else {
			s.type = IRF_RAW;
			s.data = t + 1;
			s.n = (size_t) n;
			s.freq = INFRARED_COMMON_CARRIER_FREQUENCY;
			s.duty = 330000;
			sprintf(l1, "RAW");
			sprintf(l2, "%ld timings", n);
		}
		gui_clear();
		gui_title("LEARNED");
		gui_center(22, l1, 0);
		gui_center(32, l2, 0);
		gui_center(52, "OK save  BACK skip", 0);
		gui_flush();
		key = gui_key() & ~KEY_LONG;
		if (key != KEY_OK) continue;
		sprintf(s.name, "Button_%d", nb + 1);
		if (!ui_text("BUTTON NAME", s.name, 21)) continue;
		if (has_sig(path, s.name)) { ui_note("name already used", s.name); continue; }
		if (save_sig(path, &s) < 0) { ui_note("could not save", NULL); continue; }
		nb++;
		if (!asked && s.type == IRF_PARSED && nl < LIB_AFTER)
			lm[nl++] = s.msg;
		if (!asked && nl == LIB_AFTER) {
			asked = 1;
			reader_stop();
			offer_library(path, lm, nl);
			if (reader_start() < 0) break;
		}
	}
	reader_stop();
	if (nb > 0) {
		sprintf(l2, "%d buttons", nb);
		ui_note(strrchr(path, '/') + 1, l2);
	}
}

/* Learn New Remote: ask for the remote's name (Back: cancel). */
static void learn(void)
{
	char name[24], path[PATHMAX];

	if (ensure_dir() < 0) { ui_note("cannot write", DIR_IR); return; }
	new_remote(path);
	strcpy(name, strrchr(path, '/') + 1);
	name[strlen(name) - 3] = 0;			/* without ".ir" */
	if (!ui_text("REMOTE NAME", name, 21) || !name[0]) return;
	sprintf(path, "%s/%.21s.ir", DIR_IR, name);
	learn_into(path);
}

/* Rewrite remote path without button i (nn == NULL) or with it renamed
 * to nn, through path.tmp and rename(): a power cut keeps the old file. */
static int edit_sig(const char *path, int i, const char *nn)
{
	char tmp[PATHMAX + 8];
	FILE *in, *out;
	struct irf f;
	struct irf_sig s;
	int r, k = 0, bad = 0;

	sprintf(tmp, "%s.tmp", path);
	if ((in = fopen(path, "r")) == NULL) return -1;
	if (irf_open(&f, in) < 0) { fclose(in); return -1; }
	if ((out = fopen(tmp, "w")) == NULL) { fclose(in); return -1; }
	if (irf_write_header(out, IRF_SIGNALS) < 0) bad = 1;
	s.data = t + 1;
	s.cap = MAXT;
	while (!bad && (r = irf_next(&f, &s)) != 0) {
		if (r < 0) continue;
		if (k++ == i) {
			if (!nn) continue;
			strncpy(s.name, nn, IRF_NAME_MAX);
			s.name[IRF_NAME_MAX] = 0;
		}
		if (irf_write(out, &s) < 0) bad = 1;
	}
	fclose(in);
	if (fclose(out) != 0) bad = 1;
	if (bad || rename(tmp, path) < 0) { unlink(tmp); return -1; }
	return 0;
}

/* ---- Saved remotes ---- */

struct dirlist { DIR *d; int pos; };

/* Entry i of DIR_IR that ends in ".ir" (the name without it). */
static int remote_item(void *ctx, int i, char *buf)
{
	struct dirlist *dl = ctx;
	struct dirent *e;
	size_t l;

	if (i < dl->pos) { rewinddir(dl->d); dl->pos = 0; }
	while ((e = readdir(dl->d)) != NULL) {
		l = strlen(e->d_name);
		if (l < 4 || strcmp(e->d_name + l - 3, ".ir")) continue;
		if (dl->pos++ == i) {
			l -= 3;
			if (l > UI_ITEM - 1) l = UI_ITEM - 1;
			memcpy(buf, e->d_name, l);
			buf[l] = 0;
			return 0;
		}
	}
	return -1;
}

static int count_remotes(DIR *d)
{
	struct dirent *e;
	size_t l;
	int n = 0;

	rewinddir(d);
	while ((e = readdir(d)) != NULL) {
		l = strlen(e->d_name);
		if (l >= 4 && !strcmp(e->d_name + l - 3, ".ir")) n++;
	}
	rewinddir(d);
	return n;
}

struct sigctx { const char *path; };

static int sig_item(void *ctx, int i, char *buf)
{
	struct irf_sig s;

	if (get_sig(((struct sigctx *) ctx)->path, i, &s) < 0) return -1;
	strncpy(buf, s.name, UI_ITEM - 1);
	buf[UI_ITEM - 1] = 0;
	return 0;
}

/* One remote: OK sends the selected button; Right: Rename, Delete,
 * Add button (learn into this remote). */
static void remote(const char *path, const char *title)
{
	static const char *const acts[] = { "Rename", "Delete", "Add button" };
	struct sigctx c;
	struct irf_sig s;
	char nn[24];
	int n, sel = 0, key, a;

	c.path = path;
	for (;;) {
		if ((n = count_sigs(path)) <= 0) {
			ui_note("no buttons in", title);
			if (ui_menu("EMPTY REMOTE", &acts[2], 1) == 0) {
				learn_into(path);
				continue;
			}
			return;
		}
		sel = ui_list(title, sig_item, &c, n, sel, 1, &key);
		if (sel < 0) return;
		if (key == KEY_RIGHT) {
			a = ui_menu("BUTTON", acts, 3);
			if (a == 2) learn_into(path);
			else if (a == 0 && get_sig(path, sel, &s) == 0) {
				strcpy(nn, s.name);
				if (!ui_text("BUTTON NAME", nn, 21)) continue;
				if (strcmp(nn, s.name) && has_sig(path, nn))
					ui_note("name already used", nn);
				else if (edit_sig(path, sel, nn) < 0)
					ui_note("could not save", NULL);
			} else if (a == 1) {
				a = ui_menu("DELETE BUTTON?", &acts[1], 1);
				if (a == 0 && edit_sig(path, sel, NULL) < 0)
					ui_note("could not save", NULL);
				if (sel > 0) sel--;
			}
			continue;
		}
		if (get_sig(path, sel, &s) < 0) continue;
		gui_led(0, 0, 40);
		if (send_sig(&s) < 0) ui_note("not sent", irfd < 0 && !loopback
			? "cannot open /dev/ir" : s.name);
		gui_led(0, 0, 0);
	}
}

static void saved(void)
{
	struct dirlist dl;
	char name[UI_ITEM], path[PATHMAX];
	int n, sel = 0;

	if ((dl.d = opendir(DIR_IR)) == NULL) { ui_note("no saved remotes", NULL); return; }
	for (;;) {
		if ((n = count_remotes(dl.d)) == 0) { ui_note("no saved remotes", NULL); break; }
		dl.pos = 0;
		sel = ui_list("SAVED REMOTES", remote_item, &dl, n, sel, 0, NULL);
		if (sel < 0) break;
		dl.pos = 0;
		rewinddir(dl.d);
		if (remote_item(&dl, sel, name) < 0) continue;
		sprintf(path, "%s/%.40s.ir", DIR_IR, name);
		remote(path, name);
		rewinddir(dl.d);
	}
	closedir(dl.d);
}

/* ---- Universal remotes ---- */

struct uni {
	const char *label, *file;
	const char *buttons[9];			/* NULL-terminated */
};

/* The stock universal remotes and their button names (the stock scenes
 * infrared_scene_universal_*.c). */
static const struct uni unis[] = {
	{ "TVs", "tv.ir", { "Power", "Mute", "Vol_up", "Vol_dn", "Ch_next",
		"Ch_prev", NULL } },
	{ "Audio Players", "audio.ir", { "Power", "Mute", "Play", "Pause",
		"Vol_up", "Vol_dn", "Next", "Prev", NULL } },
	{ "Projectors", "projectors.ir", { "Power", "Mute", "Vol_up",
		"Vol_dn", "Play", "Pause", NULL } },
	{ "Air Conditioners", "ac.ir", { "Off", "Dh", "Cool_hi", "Cool_lo",
		"Heat_hi", "Heat_lo", NULL } },
	{ "Fans", "fans.ir", { "Power", "Mode", "Speed_up", "Speed_dn",
		"Rotate", "Timer", NULL } },
	{ "LEDs", "leds.ir", { "Power_on", "Power_off", "Brightness_up",
		"Brightness_dn", "Red", "Green", "Blue", "White", NULL } },
};
#define NUNI	(sizeof unis / sizeof unis[0])

/* ---- Recognise a remote from the library ---- */

/* Stream database db: how many of the nl learned messages it has (same
 * protocol, address and command), and in *addr the message of the first
 * match, whose protocol and address the library remote is known by. */
static int lib_score(const char *db, const InfraredMessage *lm, int nl,
	InfraredMessage *hit)
{
	FILE *fp = fopen(db, "r");
	struct irf f;
	struct irf_sig s;
	int r, i, got = 0, seen = 0;

	if (!fp) return 0;
	if (irf_open(&f, fp) < 0) { fclose(fp); return 0; }
	s.data = t + 1;
	s.cap = MAXT;
	while ((r = irf_next(&f, &s)) != 0) {
		if (r < 0 || s.type != IRF_PARSED) continue;
		for (i = 0; i < nl; i++)
			if (!(got & 1 << i) && s.msg.protocol == lm[i].protocol
			    && s.msg.address == lm[i].address
			    && s.msg.command == lm[i].command) {
				got |= 1 << i;
				if (!seen++) *hit = s.msg;
			}
	}
	fclose(fp);
	for (i = r = 0; i < nl; i++) r += got >> i & 1;
	return r;
}

/* Add every signal of db with msg's protocol and address to remote path
 * (names the remote has already are kept as they are).  Count added. */
static int lib_copy(const char *db, const char *path, const InfraredMessage *hit)
{
	FILE *fp = fopen(db, "r");
	struct irf f;
	struct irf_sig s;
	int r, n = 0;

	if (!fp) return 0;
	if (irf_open(&f, fp) < 0) { fclose(fp); return 0; }
	s.data = t + 1;
	s.cap = MAXT;
	while ((r = irf_next(&f, &s)) != 0) {
		if (r < 0 || s.type != IRF_PARSED || s.msg.protocol != hit->protocol
		    || s.msg.address != hit->address) continue;
		/* same command already learned under its own name? */
		if (has_cmd(path, &s.msg)) continue;
		if (has_sig(path, s.name)) continue;
		if (save_sig(path, &s) == 0) n++;
	}
	fclose(fp);
	return n;
}

/* After LIB_AFTER learned buttons: if three of them are in a library
 * remote, offer to take the whole remote from the library. */
static void offer_library(const char *path, const InfraredMessage *lm, int nl)
{
	static const char *const yn[] = { "Use library", "Keep mine" };
	InfraredMessage hit;
	char db[PATHMAX], l1[32];
	unsigned k;
	int n;

	ui_msg("Looking in the", "library...");
	for (k = 0; k < NUNI; k++) {
		sprintf(db, "%s/%s", DIR_DB, unis[k].file);
		if (lib_score(db, lm, nl, &hit) < nl - 1) continue;
		sprintf(l1, "In library: %s", unis[k].label);
		if (ui_menu(l1, yn, 2) != 0) return;
		ui_msg("Adding buttons", NULL);
		n = lib_copy(db, path, &hit);
		sprintf(l1, "%d buttons added", n);
		ui_note(l1, NULL);
		return;
	}
}

/* Send every signal called button in db, as stock's brute force does,
 * until the end or Back.  Shows the progress.  The count sent, or -1. */
static int brute(const char *db, const char *button)
{
	FILE *fp = fopen(db, "r");
	struct irf f;
	struct irf_sig s;
	char line[24];
	int n = 0;

	if (!fp) return -1;
	if (irf_open(&f, fp) < 0) { fclose(fp); return -1; }
	s.data = t + 1;
	s.cap = MAXT;
	gui_led(0, 0, 40);
	while (irf_find(&f, button, &s)) {
		sprintf(line, "%d sent", ++n);
		ui_msg(button, line);
		send_sig(&s);
		if (gui_key_nowait() == KEY_BACK) break;
	}
	gui_led(0, 0, 0);
	fclose(fp);
	return n;
}

static void universal(void)
{
	const char *labels[NUNI];
	char db[PATHMAX], line[24];
	int i, u, b, nb;

	for (i = 0; i < (int) NUNI; i++) labels[i] = unis[i].label;
	for (;;) {
		if ((u = ui_menu("UNIVERSAL", labels, NUNI)) < 0) return;
		sprintf(db, "%s/%s", DIR_DB, unis[u].file);
		if (access(db, R_OK) != 0) {
			ui_note("no database:", unis[u].file);
			continue;
		}
		for (nb = 0; unis[u].buttons[nb]; nb++) ;
		for (;;) {
			if ((b = ui_menu(unis[u].label, unis[u].buttons, nb)) < 0) break;
			if ((i = brute(db, unis[u].buttons[b])) < 0) {
				ui_note("cannot read", unis[u].file);
				continue;
			}
			sprintf(line, "%d signals sent", i);
			ui_note(unis[u].buttons[b], line);
		}
	}
}

/* ---- Debug ---- */

static void debug(void)
{
	char l1[24], l2[24], l3[40];
	InfraredMessage m;
	long n;
	int key;

	if (irfd < 0 && !loopback) { ui_note("cannot open /dev/ir", "group ir?"); return; }
	if (reader_start() < 0) { ui_note("cannot start", "the reader"); return; }
	ui_msg("Debug: waiting", "BACK to exit");
	for (;;) {
		n = wait_frame(&key);
		if (n < 0) break;
		if (n == 0) { if (key == KEY_BACK) break; continue; }
		if (decode((size_t) n, &m)) describe(&m, l1, l2);
		else { strcpy(l1, "unknown"); l2[0] = 0; }
		sprintf(l3, "%ld: %lu %lu %lu %lu", n, (unsigned long) t[1],
			(unsigned long) (n > 1 ? t[2] : 0),
			(unsigned long) (n > 2 ? t[3] : 0),
			(unsigned long) (n > 3 ? t[4] : 0));
		gui_clear();
		gui_title("DEBUG");
		gui_center(20, l1, 0);
		gui_center(30, l2, 0);
		gui_text(2, 44, l3, 0);
		gui_flush();
	}
	reader_stop();
}

/* ---- self-test (-t, QEMU) ---- */

/* Every signal of path: send over the loopback, receive, decode; a parsed
 * one must come back as the same message, a raw one with its timings. */
static int selftest(const char *path, uint32_t *keep)	/* keep: 4 KB */
{
	FILE *fp = fopen(path, "r");
	char fb[256];		/* the file's buffer: stack, not 1 KB of heap */
	struct irf f;
	struct irf_sig s;
	InfraredMessage m;
	long n;
	int r, ok = 0, bad = 0;

	if (fp) setvbuf(fp, fb, _IOFBF, sizeof fb);
	if (!keep || !fp || irf_open(&f, fp) < 0) {
		fprintf(stderr, "infrared: %s: not an .ir file\n", path);
		if (fp) fclose(fp);
		return 2;
	}
	s.data = keep;
	s.cap = MAXT;
	while ((r = irf_next(&f, &s)) != 0) {
		if (r < 0) { bad++; continue; }
		if (send_sig(&s) < 0 || (n = ir_recv(-1)) <= 0) { bad++; continue; }
		if (s.type == IRF_PARSED) {
			size_t k;

			if (!decode((size_t) n, &m)) k = 0;
			else if (m.protocol == s.msg.protocol
				&& m.address == s.msg.address
				&& m.command == s.msg.command) k = 1;
			else {	/* another name for the same frame (an NECext
				 * code that is valid NEC): it must send the
				 * same timings */
				memcpy(keep, t + 1, n * 4);
				k = encode(&m);
				k = k == (size_t) n && !memcmp(keep, t + 1, n * 4);
			}
			if (k) ok++;
			else {
				bad++;
				fprintf(stderr, "infrared: %s: decoded differently\n",
					s.name);
			}
		} else {
			size_t want = s.n % 2 ? s.n : s.n - 1;

			if ((size_t) n == want && !memcmp(t + 1, keep, want * 4)) ok++;
			else bad++;
		}
	}
	fclose(fp);
	printf("infrared: %d ok, %d bad\n", ok, bad);
	return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
	static const char *const items[] = { "Learn New Remote",
		"Saved Remotes", "Universal Remotes", "Debug" };
	int sel = 0;
	uint32_t *keep = NULL;	/* -t, -c FILE: the self-test's 4 KB */

	if (argc > 0 && argv[0][0] == '/') self = argv[0];
	if (argc == 4 && !strcmp(argv[1], "-R")) {	/* the reader child */
		if (getegid() != getgid() && setgid(getgid()) < 0) return 1;
		return reader_main(atoi(argv[2]), atoi(argv[3]));
	}

	/* -c and -t: a few lines of output, unbuffered (no 1 KB stdout
	 * buffer: the self-test's heap is its 4 KB and the codecs) */
	if (argc > 1 && (!strcmp(argv[1], "-c") || !strcmp(argv[1], "-t")))
		setvbuf(stdout, NULL, _IONBF, 0);
	/* The self-test's 4 KB first, while the heap is empty and the stack
	 * shallow: later, stdio's buffers and _doprnt's 1 KB of stack would
	 * leave it no room in the 8 KB reserve (#389, M2) */
	if (argc > 2 && (!strcmp(argv[1], "-c") || !strcmp(argv[1], "-t")))
		keep = malloc(MAXT * 4);
	if (argc > 1 && !strcmp(argv[1], "-c")) {
		ir_open();
		/* fputs, not printf: _doprnt's 1 KB stack buffer would take
		 * the room of the self-test's 4 KB (MM keeps a margin below
		 * the deepest sp) */
		fputs(irfd >= 0 ? "infrared: /dev/ir opened\n"
			: "infrared: /dev/ir refused\n", stdout);
		if (irfd < 0) return 1;
		if (argc < 3) return 0;
		close(irfd);		/* -c FILE: and the self-test */
		irfd = -1;
		loopback = 1;
		dec = infrared_alloc_decoder();
		enc = infrared_alloc_encoder();
		return selftest(argv[2], keep);
	}
	if (argc > 1 && (!strcmp(argv[1], "-L") || !strcmp(argv[1], "-t")))
		loopback = 1;
	ir_open();
	dec = infrared_alloc_decoder();
	enc = infrared_alloc_encoder();
	if (argc > 2 && !strcmp(argv[1], "-t")) return selftest(argv[2], keep);
	if (argc > 1 && argv[1][0] == '/') {	/* Archive's Open (#413) */
		const char *b = strrchr(argv[1], '/') + 1;

		if (gui_open() < 0) return 1;
		remote(argv[1], b);
		gui_led(0, 0, 0);
		if (irfd >= 0) close(irfd);
		return 0;
	}
	if (argc > 1 && !loopback) {
		fprintf(stderr, "Usage: infrared [-L | -t FILE | -c [FILE] | FILE]\n");
		return 2;
	}
	if (gui_open() < 0) return 1;
	for (;;) {
		sel = ui_menu("INFRARED", items, 4);
		if (sel < 0) break;
		if (sel == 0) learn();
		else if (sel == 1) saved();
		else if (sel == 2) universal();
		else debug();
	}
	gui_led(0, 0, 0);
	if (irfd >= 0) close(irfd);
	return 0;
}
