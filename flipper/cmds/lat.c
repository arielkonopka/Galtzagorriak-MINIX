/* lat - latency budgets from the kernel trace (spec R0.8e)
 *
 * Usage:	lat [-c] [-w SECONDS]
 *
 * Reads /dev/lat (only in a kernel built with "make latcheck") and prints
 * min / median / p99 / max of
 *	L1  button down -> next frame to the LCD	(first changed pixel)
 *	L2  button down -> /dev/buttons read answered
 *	L4  longest stretch with interrupts masked (lock .. unlock)
 *	L5  REVIVE -> the revived process runs
 *	L6  exec -> the new program's first frame	(app launch)
 *	L7  exit of a drawing app -> next frame by another (back to menu)
 *	L9  one flash operation (an erase or 256 bytes programmed)
 *	L10 boot -> first frame by a process		(the launcher)
 * -w SECONDS: keep collecting for that long (press buttons, start apps),
 * reading the kernel's 32-event ring every second.  -c: clear the kernel's
 * maxima (L4, L5) first.
 */
#include <sys/types.h>
#include <sys/times.h>
#include <time.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <minix/lat.h>

#define MAXEV	1024
#define MAXS	256

static struct latinfo li;
static struct latevent ev[MAXEV];
static int nev, lost;
static unsigned long seen;		/* events taken so far (li.head) */

static void collect(int fd)
{
	unsigned long h;

	lseek(fd, 0L, SEEK_SET);
	if (read(fd, (char *) &li, sizeof(li)) != sizeof(li)) {
		perror("lat: /dev/lat");
		exit(1);
	}
	if (seen == 0 && nev == 0) {
		/* first read: whatever the ring still holds */
		seen = li.head > LAT_RING ? li.head - LAT_RING : 0;
	}
	if (li.head - seen > LAT_RING) {
		lost += li.head - seen - LAT_RING;
		seen = li.head - LAT_RING;
	}
	for (h = seen; h < li.head && nev < MAXEV; h++)
		ev[nev++] = li.ring[h % LAT_RING];
	seen = li.head;
}

static unsigned long us(unsigned long cyc)
{
	return cyc / (li.hz / 1000000);
}

static int cmp(const void *a, const void *b)
{
	unsigned long x = *(const unsigned long *) a,
		      y = *(const unsigned long *) b;

	return x < y ? -1 : x > y;
}

static void show(char *id, char *what, unsigned long *v, int n)
{
	int p99, p50;

	printf("%-4s %-34s ", id, what);
	if (n == 0) {
		printf("no samples\n");
		return;
	}
	qsort(v, n, sizeof(v[0]), cmp);
	p99 = (n * 99 + 99) / 100 - 1;
	p50 = (n - 1) / 2;
	printf("%3d  min %6lu  p50 %6lu  p99 %6lu  max %6lu us\n", n,
		us(v[0]), us(v[p50]), us(v[p99]), us(v[n - 1]));
}

/* For each event of type a, the time to the next event of type b
 * (matching arg if same, a different arg if other). */
static int pairs(int a, int b, int same, int other, unsigned long *out)
{
	int i, j, n = 0;

	for (i = 0; i < nev && n < MAXS; i++) {
		if (ev[i].ev != a) continue;
		for (j = i + 1; j < nev; j++) {
			if (ev[j].ev != b) continue;
			if (same && ev[j].arg != ev[i].arg) continue;
			if (other && ev[j].arg == ev[i].arg) continue;
			out[n++] = ev[j].cyc - ev[i].cyc;
			break;
		}
	}
	return n;
}

int main(int argc, char **argv)
{
	static unsigned long v[MAXS];
	unsigned long w;
	int fd, i, j, n, secs = 0, clear = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0) clear = 1;
		else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
			secs = atoi(argv[++i]);
		else {
			fprintf(stderr, "Usage: lat [-c] [-w SECONDS]\n");
			return 1;
		}
	}
	if ((fd = open("/dev/lat", O_RDWR)) < 0) {
		fprintf(stderr, "lat: no latency trace in this kernel "
				"(build it with make latcheck)\n");
		return 1;
	}
	if (clear) write(fd, "c", 1);
	collect(fd);
	for (i = 0; i < secs; i++) {
		sleep(1);
		collect(fd);
	}
	close(fd);

	printf("%d events%s:", nev, lost ? " (some lost: ring overflow)" : "");
	for (j = 1; j <= LE_EXIT; j++) {
		for (n = i = 0; i < nev; i++) if (ev[i].ev == j) n++;
		printf(" %s %d", j == LE_BTN ? "btn" : j == LE_BREPLY ? "reply"
			: j == LE_LCD ? "lcd" : j == LE_EXEC ? "exec" : "exit", n);
	}
	printf("\n");
	/* L1: presses that change nothing on the screen (an app that does
	 * not redraw) would pair with some much later frame: only frames
	 * within a second count, the rest are reported apart. */
	n = pairs(LE_BTN, LE_LCD, 0, 0, v);
	for (i = j = 0; i < n; i++)
		if (us(v[i]) < 1000000UL) v[j++] = v[i];
	show("L1", "button -> next LCD frame", v, j);
	if (n > j) printf("     (%d presses without a frame within 1 s)\n",
								n - j);
	show("L2", "button -> /dev/buttons answered", v,
					pairs(LE_BTN, LE_BREPLY, 0, 0, v));
	v[0] = li.mask_max;
	show("L4", "interrupts masked (max)", v, 1);
	if (li.rev_n > 0) {
		printf("L5   REVIVE -> process runs             %3lu  avg %6lu"
			"              max %6lu us\n", li.rev_n,
			us(li.rev_sum / li.rev_n), us(li.rev_max));
	} else printf("L5   REVIVE -> process runs             no samples\n");
	if (li.rev_n > 0)
		printf("     parts: to FS reply max %lu us, reply -> run max %lu"
			" us; %lu revived while shadowed\n", us(li.rev_fs_max),
			us(li.rev_q_max), li.rev_shadow_n);
	if (li.rev_n > 0)
		printf("     longest: proc %ld ran before, %lu flash ops in "
			"between; %lu over 100 us, %lu of them with flash\n",
			li.rev_q_prev, li.rev_q_flash, li.rev_q_slow,
			li.rev_q_slow_flash);
	if (li.rev_n > 0)
		printf("     longest spent in: tasks %lu us, MM %lu us, FS %lu us,"
			" processes %lu us\n", us(li.rev_q_who[0]),
			us(li.rev_q_who[1]), us(li.rev_q_who[2]),
			us(li.rev_q_who[3]));
	if (li.rev_n > 0)
		printf("     longest slice: proc %ld, %lu us\n",
			li.rev_q_big_proc, us(li.rev_q_big));
	if (li.flip_n > 0)
		printf("     image swaps %lu, longest %lu us (%lu bytes)\n",
			li.flip_n, us(li.flip_max), li.flip_bytes);
	/* L6: exec -> a frame by the same process before it exits or
	 * execs again (programs that never draw don't count) */
	n = 0;
	for (i = 0; i < nev && n < MAXS; i++) {
		if (ev[i].ev != LE_EXEC) continue;
		for (j = i + 1; j < nev; j++) {
			if (ev[j].arg != ev[i].arg) continue;
			if (ev[j].ev == LE_EXIT || ev[j].ev == LE_EXEC) break;
			if (ev[j].ev == LE_LCD) {
				v[n++] = ev[j].cyc - ev[i].cyc;
				break;
			}
		}
	}
	show("L6", "exec -> its first frame", v, n);
	/* L7: exits of processes that drew something */
	n = 0;
	for (i = 0; i < nev && n < MAXS; i++) {
		if (ev[i].ev != LE_EXIT) continue;
		for (j = i - 1; j >= 0; j--)
			if (ev[j].ev == LE_LCD && ev[j].arg == ev[i].arg) break;
		if (j < 0) continue;
		for (j = i + 1; j < nev; j++) {
			if (ev[j].ev == LE_LCD && ev[j].arg != ev[i].arg) {
				w = ev[j].cyc - ev[i].cyc;
				v[n++] = w;
				break;
			}
		}
	}
	show("L7", "app exit -> next frame (menu)", v, n);
	if (li.flash_n > 0)
		printf("L9   flash erase/program (stall)      %3lu"
			"              max %6lu us\n", li.flash_n,
			us(li.flash_max));
	if (li.boot_lcd_ms != 0)
		printf("L10  boot -> first frame                  %lu ms\n",
							li.boot_lcd_ms);
	{	/* T14b: Stop2 */
		struct tms tm;
		unsigned long up = (unsigned long) times(&tm) / CLK_TCK;
		unsigned long st = li.stop_lp / 32768;

		printf("Stop2  %lu of %lu s up (%lu%%), wake-ups: %lu by a "
			"button, %lu other\n", st, up, up ? st * 100 / up : 0,
			li.wake_btn, li.wake_other);
		if (li.wake_n > 0)
			printf("Stop2  in and out (clocks back)    %6lu  avg "
				"%5lu us        max %6lu us\n", li.wake_n,
				li.wake_sum / li.wake_n / 16,
				li.wake_max / 16);
		printf("Stop2  not taken: blocker %lu, USB %lu, alarm %lu, "
			"tick work %lu, arming %lu, timer write %lu, other %lu\n",
			li.nosleep[1], li.nosleep[2], li.nosleep[3],
			li.nosleep[4], li.nosleep[5], li.nosleep[6],
			li.nosleep[7]);
	}
	return 0;
}
