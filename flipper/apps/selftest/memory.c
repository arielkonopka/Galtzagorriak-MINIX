/* memory - self test module (spec R7b.3.1, RAM part): every 4 KB block of
 * the 256 KB of SRAM (SRAM1 192 KB, SRAM2 64 KB) as a cell of an 8 x 8
 * grid.  A free block is tested by MM in place (MI_MEMTEST, March C-),
 * one block per call; the process never sees it.  Blocks in use and the
 * ones that are not user memory (kernel, servers, SRAM2: the FS cache and
 * CPU2's share) are shown, not tested.  Automatic.  Root.
 *
 * Cells: empty frame untested, filled pass, cross fail, dotted in use,
 * hatched not ours.  The flash part (CRCs, ECC counts) follows.
 */
#include <lib.h>
#include <minix/type.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "gui.h"
#include "st.h"

#define RAM	0x20000000UL
#define BLOCKS	64
#define CX(i)	(4 + ((i) % 8) * 15)
#define CY(i)	(9 + ((i) / 8) * 7)

static int gui;

static int memtest(unsigned long addr, unsigned long *bad)
{
	message m;
	int r;

	m.m1_i1 = (int) addr;
	m.m1_i2 = MI_MEMTEST;
	m.m1_p1 = NULL;
	r = _syscall(MM, MEMINFO, &m);
	*bad = (unsigned long) m.m2_i1;
	return r;
}

static void cell(int i, int st)
{
	int x = CX(i), y = CY(i), k;

	if (!gui) return;
	gui_fill(x, y, x + 12, y + 5, 0);
	switch (st) {
	case MT_PASS:
		gui_fill(x, y, x + 12, y + 5, 1);
		break;
	case MT_FAIL:
		gui_frame(x, y, x + 12, y + 5);
		for (k = 0; k < 6; k++) {
			gui_pixel(x + 2 * k + 1, y + k, 1);
			gui_pixel(x + 11 - 2 * k, y + k, 1);
		}
		break;
	case MT_INUSE:
		gui_frame(x, y, x + 12, y + 5);
		for (k = 2; k < 12; k += 3) gui_pixel(x + k, y + 2, 1);
		break;
	case MT_OWNED:
		gui_frame(x, y, x + 12, y + 5);
		for (k = 0; k < 12; k += 2) gui_pixel(x + k, y + 5 - k % 6, 1);
		break;
	default:			/* untested */
		gui_frame(x, y, x + 12, y + 5);
	}
}

int main(int argc, char **argv)
{
	unsigned long bad = 0, at = 0;
	char why[48], line[40];
	int autom = argc > 1 && strcmp(argv[1], "-a") == 0;
	int i, r, n[4] = { 0, 0, 0, 0 }, err = 0;

	gui = gui_open() == 0;
	if (gui) {
		gui_clear();
		gui_title("Memory: RAM");
		for (i = 0; i < BLOCKS; i++) cell(i, -1);
		gui_flush();
	}
	for (i = 0; i < BLOCKS; i++) {
		r = memtest(RAM + (unsigned long) i * MT_BLOCK, &bad);
		if (r < 0) {			/* not root, or an old MM */
			err = 1;
			break;
		}
		if (r <= MT_OWNED) n[r]++;
		if (r == MT_FAIL && at == 0) at = bad;
		cell(i, r);
		if (gui && (i % 8 == 7 || r == MT_FAIL)) gui_flush();
	}
	sprintf(line, "%d ok %d bad %d in use", n[MT_PASS], n[MT_FAIL],
		n[MT_INUSE]);
	fprintf(stderr, "memory: %s, %d not ours\n", line, n[MT_OWNED]);
	if (gui) {
		gui_text(4, GUI_H - 6, line, 0);
		gui_flush();
		if (!autom) (void) gui_key();
		gui_close();
	}
	if (err) {
		st_result("SKIP", "MM has no MI_MEMTEST, or not root");
		return 0;
	}
	if (n[MT_FAIL] > 0) {
		sprintf(why, "%d block(s), first at %08lx", n[MT_FAIL], at);
		st_result("FAIL", why);
		return 1;
	}
	sprintf(why, "%d blocks", n[MT_PASS]);
	st_result("PASS", why);
	return 0;
}
