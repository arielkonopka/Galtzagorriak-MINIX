/* rfidemu - be a 125 kHz EM4100 tag for a reader, from the launcher.
 *
 * Edit the five-byte number with Left/Right (pick a byte) and Up/Down
 * (change it), press OK to start, hold the Flipper's back to the reader;
 * OK or Back stops.  The emulation itself is the kernel's (RFIDIOCEMU on
 * /dev/rfid): the reader's own field is the clock, so it cannot drift and
 * the CPU does nothing while it runs.  The default is 04 00 98 3D C5.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <minix/rfid.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "gui.h"

static unsigned long dur[128], pulse[128];	/* the EM4100 waveform */

/* The 64 EM4100 bits of a number into b[64] (as cmds/rfid.c em_bits). */
static void em_bits(unsigned char id[5], unsigned char b[64])
{
	int r, c, p;

	for (r = 0; r < 9; r++) b[r] = 1;
	for (r = 0; r < 10; r++) {
		int nib = id[r / 2] >> (r % 2 ? 0 : 4) & 15;

		p = 0;
		for (c = 0; c < 4; c++) {
			b[9 + r * 5 + c] = nib >> (3 - c) & 1;
			p ^= b[9 + r * 5 + c];
		}
		b[9 + r * 5 + 4] = p;
	}
	for (c = 0; c < 4; c++) {
		p = 0;
		for (r = 0; r < 10; r++) p ^= b[9 + r * 5 + c];
		b[59 + c] = p;
	}
	b[63] = 0;
}

/* Open /dev/rfid and start emulating id; returns the fd or -1. */
static int emu_start(unsigned char id[5])
{
	unsigned char b[64];
	struct rfid_emu e;
	int k, fd;

	em_bits(id, b);
	for (k = 0; k < 64; k++) {
		dur[2 * k] = dur[2 * k + 1] = 31;	/* two 32-period halves */
		pulse[2 * k] = b[k] ? 32 : 0;
		pulse[2 * k + 1] = b[k] ? 0 : 32;
	}
	e.dur = dur;
	e.pulse = pulse;
	e.n = 128;
	if ((fd = open("/dev/rfid", O_RDWR)) < 0) return -1;
	if (ioctl(fd, RFIDIOCEMU, &e) < 0) { close(fd); return -1; }
	return fd;			/* close stops the emulation */
}

int main(void)
{
	unsigned char id[5] = { 0x04, 0x00, 0x98, 0x3D, 0xC5 };
	int sel = 0, k, i, fd = -1;
	char line[40];

	if (gui_open() < 0) return 1;
	for (;;) {
		gui_clear();
		gui_title("RFID EMU");
		if (fd >= 0) {
			gui_center(16, "EMULATING", 0);
			sprintf(line, "%02X %02X %02X %02X %02X",
				id[0], id[1], id[2], id[3], id[4]);
			gui_center(30, line, 0);
			gui_center(44, "hold to a reader", 0);
			gui_center(54, "OK/BACK to stop", 0);
			gui_led(0, 40, 0);
		} else {
			gui_center(14, "EM4100 number:", 0);
			for (i = 0; i < 5; i++) {
				sprintf(line, "%02X", id[i]);
				gui_text(14 + i * 20, 28, line, i == sel);
			}
			gui_center(44, "L/R pick  U/D change", 0);
			gui_center(54, "OK emulate  BACK exit", 0);
		}
		gui_flush();
		k = gui_key() & ~KEY_LONG;
		if (fd >= 0) {			/* emulating: any OK/Back stops */
			if (k == KEY_OK || k == KEY_BACK) {
				close(fd);	/* the kernel drops the field */
				fd = -1;
				gui_led(0, 0, 0);
			}
			continue;
		}
		if (k == KEY_BACK) break;
		if (k == KEY_LEFT) sel = (sel + 4) % 5;
		if (k == KEY_RIGHT) sel = (sel + 1) % 5;
		if (k == KEY_UP) id[sel]++;
		if (k == KEY_DOWN) id[sel]--;
		if (k == KEY_OK) {
			if ((fd = emu_start(id)) < 0)
				gui_center(0, "cannot open /dev/rfid", 1);
		}
	}
	if (fd >= 0) close(fd);
	gui_led(0, 0, 0);
	return 0;
}
