/* SPDX-License-Identifier: GPL-3.0-or-later
 * gpioui - the stock GPIO app on Galtzagorriak MINIX (issue #413, part 2
 * of the stock app ports), a window app (spec K.8).  The header's eight
 * pins on /dev/gpio, as the gpio command: each pin's level and its mode
 * (input, pull up or down, output low or high, off).  The 5 V switch and
 * the USB-UART bridge have no kernel path yet (no new /dev paths in this
 * part): their rows say so, with stock's warning for the 5 V one.
 *
 *	GPIO:nobody:@gpio:=/bin/gpioui	(/etc/apps)
 *
 * The kernel keeps no mode to read back: a pin's mode is shown as "-"
 * until it is set here (it stays as set after the app ends, as stock).
 * RAM: no stdio, no malloc, 8 bytes of .bss beyond the gm buffers.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gm.h"

static const char *const label[8] = {
	"PA7 (2)", "PA6 (3)", "PA4 (4)", "PB3 (5)",
	"PB2 (6)", "PC3 (7)", "PC1 (15)", "PC0 (16)"
};
/* The gpio command's modes, in /dev/gpio's order: 0 1 in up down off;
 * the enum shows "-" first (not set here). */
static const char mode_opts[] = "-|Out 0|Out 1|In|In up|In down|Off";
static unsigned char mode[8];		/* 0: not set, else /dev/gpio's + 1 */
static char top = 'm';

static void w_main(void)
{
	char lv[8], k[3], lab[24];
	int fd = open("/dev/gpio", O_RDONLY), ok, i;

	ok = fd >= 0 && read(fd, lv, 8) == 8;
	if (fd >= 0) close(fd);
	gm_win_live("main", "list", "GPIO", 1);
	for (i = 0; i < 8; i++) {
		k[0] = 'p';
		k[1] = (char) ('0' + i);
		k[2] = 0;
		strcpy(lab, label[i]);
		strcat(lab, ok ? (lv[i] == '1' ? " =1" : " =0") : " =?");
		gm_enum(k, lab, mode[i], mode_opts);
	}
	gm_item("rd", "Read the levels", "");
	gm_item("v5", "5V on GPIO", "n/a");
	gm_item("uart", "USB-UART Bridge", "n/a");
	gm_show();
	top = 'm';
}

static void w_note(const char *t, const char *l1, const char *l2,
	const char *l3)
{
	gm_win("note", "text", t, 0, 0);
	gm_line(l1);
	if (l2) gm_line(l2);
	if (l3) gm_line(l3);
	gm_show();
	top = 'n';
}

static int set(int pin, int m)
{
	unsigned char c[2];
	int fd = open("/dev/gpio", O_WRONLY), r;

	if (fd < 0) return -1;
	c[0] = (unsigned char) pin;
	c[1] = (unsigned char) m;
	r = write(fd, (char *) c, 2) == 2 ? 0 : -1;
	close(fd);
	return r;
}

static void values(char **f)
{
	char *k, *v;
	int i, x;

	for (i = 2; i < GM_FIELDS && f[i] != NULL; i++) {
		k = gm_kv(f[i], &v);
		x = atoi(v);
		if (k[0] == 'p' && k[1] >= '0' && k[1] <= '7' && !k[2]
		    && x >= 1 && x <= 6 && set(k[1] - '0', x - 1) == 0)
			mode[k[1] - '0'] = (unsigned char) x;
	}
	w_main();			/* the levels after the change */
}

static void ok(const char *key)
{
	if (!strcmp(key, "rd")) w_main();
	else if (!strcmp(key, "v5"))
		w_note("5V on GPIO", "Stock: \"Careful! 5V on pin 1",
			"may damage what is connected\"", "No 5 V switch here yet.");
	else if (!strcmp(key, "uart"))
		w_note("USB-UART Bridge", "Not on this system yet:",
			"use the console on USB", NULL);
	else gm_busy(0);
}

int main(void)
{
	char buf[128], ev[128], *f[GM_FIELDS];

	if (gm_init(buf, sizeof buf) < 0) {
		(void) write(2, "gpioui: start it from the launcher\n", 35);
		return 1;
	}
	w_main();
	for (;;) {
		switch (gm_event(ev, sizeof ev, f, GM_FIELDS)) {
		case GM_OK: if (f[2]) ok(f[2]); break;
		case GM_SUBMIT: values(f); break;
		case GM_BACK: top = 'm'; break;
		case GM_PAGE:
		case GM_REDRAW: w_main(); break;
		case GM_CLOSE: return 0;
		}
	}
}
