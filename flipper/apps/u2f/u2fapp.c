/* SPDX-License-Identifier: GPL-3.0-or-later
 * u2f - the stock U2F app on Galtzagorriak MINIX (issue #420), a window
 * app (spec K.8): the Flipper as a U2F security key.
 *
 *	main	the state: waiting for the computer, ready, the last login
 *	ask	"Register?" or "Log in?" with the action Confirm: a site asked,
 *		and nothing is signed until OK (U2F user presence)
 *	error	a file is missing or bad (src/lib/u2f/u2fstore.h): what to do
 *
 * The logic is libu2f (src/lib/u2f: messages, U2FHID framing, P-256 from
 * micro-ecc); the files are u2fstore's, in /ext/u2f (/home/flipper/u2f).
 * The certificate and its key are the user's own: none is shipped.
 *
 * Transport: the 64-byte U2FHID reports of a USB HID FIDO interface,
 * U2FDEV (one report per read and per write).  That interface is not
 * there yet (it comes with the USB work, like Bad USB's HID); until then
 * the open fails and the app says so: the transport stub.
 *
 *	U2F:u2f:@usb:=/usr/bin/u2f	(/etc/apps; user u2f, group usb)
 *
 * RAM: no stdio, no malloc, no signal handler: no MM gap (K.8).  The
 * U2FHID state with its message buffer (~1 KB) and the certificate
 * (768 B) are .bss; P-256 runs on the stack.
 */
#include <sys/types.h>
#include <sys/times.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "gm.h"
#include "u2f.h"
#include "u2fstore.h"
#include "p256.h"

#define DIR	"/home/flipper/u2f"		/* /ext/u2f */
#define U2FDEV	"/dev/u2fhid"			/* the USB interface */

static struct u2f u;
static struct u2fhid h;
static unsigned char cert[U2F_MAXCERT];
static int rfd = -1, dev = -1, err, asked, logins;
static char cur;				/* m(ain) a(sk) e(rror) */

static long now_ms(void)
{
	struct tms t;

	return (long) ((unsigned long) times(&t) * 1000UL / CLK_TCK);
}

static int fill(unsigned char *b, unsigned n)
{
	unsigned k = 0;
	int r;

	if (rfd < 0) return -1;
	while (k < n) {
		if ((r = (int) read(rfd, b + k, n - k)) <= 0) return -1;
		k += (unsigned) r;
	}
	return 0;
}

static void h_random(void *ctx, unsigned char *b, unsigned n)
{
	(void) ctx;
	if (fill(b, n) < 0) memset(b, 0, n);	/* then P-256 refuses 0 */
}

static void s_random(unsigned char *b, unsigned n)
{
	h_random(NULL, b, n);
}

static int h_save(void *ctx, unsigned long c)
{
	(void) ctx;
	return u2fs_counter_save(DIR, c);
}

static void h_event(void *ctx, int ev)
{
	(void) ctx;
	if ((ev == U2F_EV_REGISTER || ev == U2F_EV_AUTH) && cur != 'a')
		asked = ev;			/* shown after the answer */
	if (ev == U2F_EV_AUTH_OK) logins++;
}

static void h_send(void *ctx, const unsigned char p[U2F_PKT])
{
	(void) ctx;
	if (dev >= 0) (void) write(dev, p, U2F_PKT);
}

static const struct u2f_ops ops = { NULL, h_random, p256_pub, p256_sign,
				    h_save, h_event, h_send };

static void w_error(void)
{
	gm_win("error", "text", "U2F", 0, 0);
	gm_line(u2fs_error(err));
	if (err == U2FS_NOCERT || err == U2FS_NOKEY || err == U2FS_ENCRYPTED) {
		gm_line("Put your own cert.der and");
		gm_line("cert_key.u2f (Type: 2) in");
		gm_line("/ext/u2f/assets");
	} else if (err == U2FS_COUNTER || err == U2FS_DEVKEY) {
		gm_line("Not reset, so no counter");
		gm_line("is used twice: see the");
		gm_line("manual, Small apps, U2F");
	}
	gm_show();
	cur = 'e';
}

static void w_main(void)
{
	char l[32], t[12], *p = t + 11;
	int v = logins;

	gm_win("main", "text", "U2F", 0, 0);
	if (dev < 0) {
		gm_line("No USB U2F interface yet");
		gm_line("(planned): the keys are");
		gm_line("ready, the cable is not");
	} else {
		gm_line("Ready: plug in and log in");
		gm_line("on a site with U2F");
	}
	*p = 0;
	do *--p = (char) ('0' + v % 10); while ((v /= 10) > 0);
	strcpy(l, "Logins this time: ");
	strcat(l, p);
	gm_line(l);
	gm_show();
	cur = 'm';
}

static void w_ask(void)
{
	gm_win("ask", "confirm", asked == U2F_EV_REGISTER ? "Register"
							   : "Log in", 0, 0);
	gm_line(asked == U2F_EV_REGISTER ? "A site wants to register"
					 : "A site wants a login");
	gm_line("this Flipper. OK to allow");
	gm_action("yes", "Confirm");
	gm_show();
	cur = 'a';
}

static void redraw(void)
{
	if (cur == 'e') w_error();
	else if (cur == 'a') w_ask();
	else w_main();
}

int main(int argc, char **argv)
{
	char buf[128], ev[128], *f[GM_FIELDS];
	unsigned char pkt[U2F_PKT];
	static const char use[] = "u2f: start U2F from the launcher\n";
	struct pollfd p[2];
	int n;

	(void) argv;
	if (argc != 1 || gm_init(buf, sizeof buf) < 0) {
		(void) write(2, use, sizeof use - 1);
		return 1;
	}
	rfd = open("/dev/random", O_RDONLY);
	p256_rng(fill);
	u.ops = &ops;
	err = u2fs_load(DIR, &u, cert, s_random);
	if (err != U2FS_OK) w_error();
	else {
		u2fhid_init(&h, &u);
		dev = open(U2FDEV, O_RDWR);	/* the stub: not there yet */
		w_main();
	}
	for (;;) {
		if (!gm_pending(ev)) {
			p[0].fd = GM_EVFD;
			p[0].events = POLLIN;
			p[0].revents = 0;
			p[1].fd = dev;
			p[1].events = POLLIN;
			p[1].revents = 0;
			if (poll(p, dev >= 0 ? 2 : 1, -1) <= 0) continue;
			if (dev >= 0 && p[1].revents != 0) {
				n = (int) read(dev, pkt, sizeof pkt);
				if (n <= 0) {		/* unplugged */
					u2fhid_reset(&h);
					continue;
				}
				u2fhid_packet(&h, pkt, (unsigned) n, now_ms());
				if (asked && cur != 'a') w_ask();
				else if (cur == 'm') w_main();
			}
			if (p[0].revents == 0) continue;
		}
		switch (gm_event(ev, sizeof ev, f, GM_FIELDS)) {
		case GM_OK:
			if (cur == 'a' && f[2] != NULL
			    && strcmp(f[2], "yes") == 0) {
				u2f_present(&u);	/* the host asks again */
				asked = 0;
				w_main();
			} else
				gm_busy(0);
			break;
		case GM_BACK:
			if (cur == 'a') {		/* not allowed */
				asked = 0;
				w_main();
			} else
				return 0;
			break;
		case GM_REDRAW:
			redraw();
			break;
		case GM_CLOSE:
			return 0;
		}
	}
}
