/* board - self test module (spec R7b.3.4): RTC, SD card, battery gauge
 * and charger, the LED chip, USB.  Automatic (runs in All tests).
 *
 * RTC: the clock advances across a 1.2 s wait, calibration value.  SD: size,
 * 1 MB read with its speed, and a write-read-compare of one scratch
 * sector in the gap before the first partition (the original is put back;
 * skipped if there is no gap).  Gauge: voltage and temperature in range.
 * LED: the driver takes "off".  USB: shown, never a failure.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/times.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <minix/partition.h>
#include <minix/rtc.h>
#include "gui.h"
#include "st.h"

static int row, gui, fails;
static char why[40];

static void line(const char *what, const char *val, int ok)
{
	char b[40];

	sprintf(b, "%-7s%s", what, val);
	if (gui) {
		gui_text(2, 8 + row * 7, b, 0);
		gui_text(GUI_W - 4 * 4, 8 + row * 7, ok ? "ok" : "FAIL", !ok);
		gui_flush();
	}
	fprintf(stderr, "board: %s %s\n", b, ok ? "ok" : "FAIL");
	row++;
	if (!ok) {
		fails++;
		if (why[0] == 0) strncpy(why, what, sizeof why - 1);
	}
}

static void rtc(void)
{
	char b[32];
	time_t t0 = time((time_t *) 0);
	int fd, cal = 0, ok;

	(void) poll((struct pollfd *) NULL, 0, 1200);
	ok = time((time_t *) 0) > t0;	/* timecheck: ok (a comparison) */
	if ((fd = open("/dev/rtc", O_RDONLY)) >= 0) {
		if (ioctl(fd, RTCIOCGCAL, &cal) != 0) cal = 0;
		close(fd);
	}
	sprintf(b, "runs, cal %d", cal);
	line("RTC", ok ? b : "stopped", ok);
}

static void sd(void)
{
	static char buf[512], save[512];
	struct partition p;
	struct tms tm;
	clock_t t0, t;
	unsigned long start, sec;
	long kb;
	char b[32];
	int fd, i, ok;

	if ((fd = open("/dev/hd0", O_RDWR)) < 0
	    || ioctl(fd, DIOCGETP, &p) != 0) {
		if (fd >= 0) close(fd);
		line("SD", "no card", 0);
		return;
	}
	sprintf(b, "%lu MB", (unsigned long) (p.size >> 20));
	line("SD", b, 1);

	/* 1 MB from the start of the card, through the FS cache (which is
	 * small next to 1 MB, so this is mostly the card) */
	lseek(fd, 0L, SEEK_SET);
	t0 = times(&tm);
	for (i = 0, ok = 1; i < 2048 && ok; i++)
		ok = read(fd, buf, sizeof buf) == sizeof buf;
	t = times(&tm) - t0;
	kb = t > 0 ? 1024L * CLK_TCK / t : 0;
	sprintf(b, "1 MB %ld KB/s", kb);
	line("read", ok ? b : "error", ok);

	/* The scratch sector: in the gap before the lowest partition, and
	 * so that its whole 1 KB FS block lies in the gap (FS writes the
	 * block, both sectors).  The read-back goes to the card, not FS's
	 * cache: closing an unmounted block device syncs and invalidates
	 * its cache (fs/open.c), so it is closed and opened again. */
	lseek(fd, 0L, SEEK_SET);
	if (read(fd, buf, 512) != 512 || (unsigned char) buf[510] != 0x55
				|| (unsigned char) buf[511] != 0xAA) {
		line("write", "no MBR", 0);
		close(fd);
		return;
	}
	start = 0;
	for (i = 0; i < 4; i++) {
		unsigned char *e = (unsigned char *) buf + 446 + 16 * i;
		unsigned long s0 = e[8] | e[9] << 8
			| (unsigned long) e[10] << 16 | (unsigned long) e[11] << 24;

		if (e[4] != 0 && s0 != 0 && (start == 0 || s0 < start))
			start = s0;
	}
	sec = (start & ~1UL) - 1;		/* odd: block (sec-1, sec) */
	if (start < 4 || sec < 3) {
		line("write", "no gap: skipped", 1);
		close(fd);
		return;
	}
	lseek(fd, (long) sec * 512, SEEK_SET);
	ok = read(fd, save, 512) == 512;
	for (i = 0; i < 512; i++) buf[i] = (char) (i * 7 + 0x5A);
	lseek(fd, (long) sec * 512, SEEK_SET);
	ok = ok && write(fd, buf, 512) == 512;
	close(fd);				/* sync + invalidate */
	if ((fd = open("/dev/hd0", O_RDWR)) < 0) {
		line("write", "cannot open again", 0);
		return;
	}
	memset(buf, 0, sizeof buf);
	lseek(fd, (long) sec * 512, SEEK_SET);
	ok = ok && read(fd, buf, 512) == 512;	/* from the card */
	for (i = 0; ok && i < 512; i++)
		if (buf[i] != (char) (i * 7 + 0x5A)) ok = 0;
	lseek(fd, (long) sec * 512, SEEK_SET);
	if (write(fd, save, 512) != 512) ok = 0;	/* put it back */
	close(fd);
	sprintf(b, "sector %lu", sec);
	line("write", ok ? b : "compare failed", ok);
}

static void gauge(void)
{
	char l[128], b[32];
	int fd, n, soc = -1, mv = 0, ma, avg, sby, temp = 0, usb = 0, chg = -1;
	unsigned flags;

	if ((fd = open("/dev/power", O_RDONLY)) < 0) {
		line("gauge", "no /dev/power", 0);
		return;
	}
	n = read(fd, l, sizeof l - 1);
	close(fd);
	if (n > 0) {
		l[n] = 0;
		(void) sscanf(l, "soc=%d mv=%d ma=%d avg=%d sby=%d temp=%d "
			"flags=0x%x usb=%d chg=%d", &soc, &mv, &ma, &avg,
			&sby, &temp, &flags, &usb, &chg);
	}
	sprintf(b, "%d%% %d mV %d.%d C", soc, mv, temp / 10,
		(temp < 0 ? -temp : temp) % 10);
	line("gauge", b, mv >= 3000 && mv <= 4500 && temp > -200
		&& temp < 600 && soc >= 0 && soc <= 100);
	sprintf(b, "%s", chg < 0 ? "?" : chg == 0 ? "not charging"
		: chg == 3 ? "charged" : "charging");
	line("charge", b, chg >= 0);
	line("USB", usb ? "connected" : "not connected", 1);
}

static void led(void)
{
	int fd, ok;

	ok = (fd = open("/dev/led", O_WRONLY)) >= 0
		&& write(fd, "off\n", 4) == 4;
	if (fd >= 0) close(fd);
	line("LED", ok ? "LP5562 answers" : "no answer", ok);
}

int main(int argc, char **argv)
{
	int autom = argc > 1 && strcmp(argv[1], "-a") == 0;

	gui = gui_open() == 0;
	if (gui) {
		gui_clear();
		gui_title("Board");
		gui_flush();
	}
	rtc();
	sd();
	gauge();
	led();
	if (gui && !autom) {
		gui_text(2, GUI_H - 6, "any key", 0);
		gui_flush();
		(void) gui_key();
	}
	if (gui) gui_close();
	st_result(fails ? "FAIL" : "PASS", why);
	return fails != 0;
}
