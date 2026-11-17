/* gpio - self test module (spec R7b.3.5): GPIO loopback.  The user puts
 * jumpers on the header pin pairs 2-3, 4-5, 6-7 and 15-16; each pin of a
 * pair drives high and low while the other reads, once with a pull-down
 * and once with a pull-up on the reading pin (without a jumper the pull
 * wins and the pair fails).  Then the 5 V output on pin 1 is switched on
 * and read back from the charger, and off again.  All pins end "off".
 * Interactive (it needs the jumpers).  Root (the 5 V output).
 *
 * /dev/gpio: pins 0..7 are header pins 2 3 4 5 6 7 15 16; a write is
 * pairs of (pin, op): 0 low, 1 high, 2 input, 3 pull-up, 4 pull-down,
 * 5 off; a read gives one '0'/'1' per pin.
 */
#include <sys/types.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "gui.h"
#include "st.h"

static const int hdr[8] = { 2, 3, 4, 5, 6, 7, 15, 16 };
static int fd;

static void op(int pin, int o)
{
	unsigned char b[2];

	b[0] = pin; b[1] = o;
	(void) write(fd, b, 2);
}

static int level(int pin)
{
	char v[8];

	(void) poll((struct pollfd *) NULL, 0, 2);	/* settle */
	lseek(fd, 0L, SEEK_SET);
	if (read(fd, v, 8) != 8) return -1;
	return v[pin] == '1';
}

/* drive -> sense with each pull: 1 if the sense pin follows both ways */
static int follows(int drive, int sense)
{
	int ok = 1;

	op(sense, 4);				/* pull-down: high must win */
	op(drive, 1);
	ok &= level(sense) == 1;
	op(drive, 0);
	ok &= level(sense) == 0;
	op(sense, 3);				/* pull-up: low must win */
	op(drive, 0);
	ok &= level(sense) == 0;
	op(drive, 1);
	ok &= level(sense) == 1;
	op(drive, 5);
	op(sense, 5);
	return ok;
}

/* The charger's VBUS_STAT ("vbus=" in /dev/power): 7 = the boost (OTG)
 * makes the 5 V, 0 = nothing on VBUS, else a USB host or charger. */
static int vbus(void)
{
	char l[160], *p;
	int f, n;

	if ((f = open("/dev/power", O_RDONLY)) < 0) return -1;
	n = read(f, l, sizeof l - 1);
	close(f);
	if (n <= 0) return -1;
	l[n] = 0;
	p = strstr(l, "vbus=");
	return p == NULL ? -1 : p[5] - '0';
}

static void otg(int on)
{
	int f;

	if ((f = open("/dev/power", O_WRONLY)) < 0) return;
	(void) write(f, on ? "otg on\n" : "otg off\n", on ? 7 : 8);
	close(f);
}

/* 1 the 5 V output works, 0 it does not, -1 not testable (USB powers
 * VBUS already) */
static int five_volt(void)
{
	int v;

	if (vbus() != 0) return -1;
	otg(1);
	(void) poll((struct pollfd *) NULL, 0, 100);	/* the boost starts */
	v = vbus();
	otg(0);
	return v == 7;
}

int main(int argc, char **argv)
{
	char why[48], b[32];
	int i, k, bad = 0, r;

	if (argc > 1 && strcmp(argv[1], "-a") == 0) {
		st_result("SKIP", "interactive (jumpers)");
		return 0;
	}
	if (gui_open() < 0 || (fd = open("/dev/gpio", O_RDWR)) < 0) {
		st_result("SKIP", "no screen or /dev/gpio");
		return 0;
	}
	gui_clear();
	gui_title("GPIO loopback");
	gui_center(16, "Put jumpers on the pins", 0);
	gui_center(24, "2-3  4-5  6-7  15-16", 0);
	gui_center(52, "OK: test   Back: skip", 0);
	gui_flush();
	do k = gui_key(); while (k != KEY_OK && k != KEY_BACK);
	if (k == KEY_BACK) {
		gui_close();
		st_result("SKIP", "no jumpers");
		return 0;
	}
	gui_clear();
	gui_title("GPIO loopback");
	strcpy(why, "pairs");
	for (i = 0; i < 8; i += 2) {
		r = follows(i, i + 1) && follows(i + 1, i);
		sprintf(b, "pins %d-%d", hdr[i], hdr[i + 1]);
		gui_text(4, 10 + (i / 2) * 7, b, 0);
		gui_text(GUI_W - 20, 10 + (i / 2) * 7, r ? "ok" : "FAIL", !r);
		gui_flush();
		fprintf(stderr, "gpio: %s %s\n", b, r ? "ok" : "FAIL");
		if (!r) {
			bad++;
			if (strlen(why) + 7 < sizeof why) {
				sprintf(b, " %d-%d", hdr[i], hdr[i + 1]);
				strcat(why, b);
			}
		}
	}
	for (i = 0; i < 8; i++) op(i, 5);
	r = five_volt();
	gui_text(4, 40, "5 V on pin 1", 0);
	gui_text(GUI_W - 32, 40, r == 1 ? "ok" : r < 0 ? "USB: -" : "FAIL",
								r == 0);
	gui_text(4, GUI_H - 6, "any key", 0);
	gui_flush();
	fprintf(stderr, "gpio: 5 V output %s\n", r == 1 ? "ok"
		: r < 0 ? "not tested (USB powers VBUS)" : "FAIL");
	(void) gui_key();
	gui_close();
	close(fd);
	if (bad || r == 0) {
		if (r == 0) strcat(why, bad ? ", 5 V" : " ok; 5 V output");
		st_result("FAIL", why);
		return 1;
	}
	st_result("PASS", r < 0 ? "5 V not tested (USB)" : NULL);
	return 0;
}
