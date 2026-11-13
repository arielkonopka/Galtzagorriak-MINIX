/* Milestone 0 bring-up test: serial banner, LCD text, button echo.
 * On QEMU it runs a short self-test and exits; on the Flipper it loops
 * showing button presses (hold BACK for 2 s to halt). */
#include "hal.h"
#include "fbcon.h"

static const char *button_name(unsigned b)
{
	switch (b) {
	case BTN_UP: return "UP";
	case BTN_DOWN: return "DOWN";
	case BTN_LEFT: return "LEFT";
	case BTN_RIGHT: return "RIGHT";
	case BTN_OK: return "OK";
	case BTN_BACK: return "BACK";
	}
	return "?";
}

int main(void)
{
	unsigned prev = 0, now, b;
	uint32_t back_since = 0;
	int c;

	hal_init();
	kprintf("\nMINIX 2.0.0 for Flipper Zero -- milestone 0\n");
	kprintf("board: %s, cpu %lu Hz\n", hal_board_name(),
		(unsigned long) hal_cpu_hz());

	hal_lcd_init();
	fbcon_init();
	fbcon_puts("MINIX 2.0.0 / Flipper Zero\n");
	fbcon_puts("milestone 0: hal bring-up\n\n");
	fbcon_flush();

	hal_delay_ms(100);
	kprintf("systick: %lu ms elapsed after 100 ms delay\n",
		(unsigned long) hal_ms);

#ifdef BOARD_QEMU
	/* Exercise scrolling and line wrap, then show the screen. */
	for (c = 0; c < 12; c++) {
		fbcon_puts("line ");
		fbcon_putc('0' + c / 10);
		fbcon_putc('0' + c % 10);
		fbcon_putc('\n');
	}
	fbcon_puts("0123456789abcdefghijklmnopqrstuvwxyz");
	fbcon_flush();
	kprintf("m0 self-test done\n");
	hal_halt(0);
#endif

	for (;;) {
		now = hal_buttons();
		for (b = 1; b <= BTN_BACK; b <<= 1) {
			if ((now & b) && !(prev & b)) {
				kprintf("press %s\n", button_name(b));
				fbcon_puts(button_name(b));
				fbcon_putc(' ');
			}
		}
		if ((now & BTN_BACK) && !(prev & BTN_BACK)) back_since = hal_ms;
		if ((now & BTN_BACK) && hal_ms - back_since > 2000) {
			fbcon_puts("\nhalted.");
			fbcon_flush();
			kprintf("halted\n");
			hal_halt(0);
		}
		prev = now;
		if ((c = hal_uart_getc()) >= 0) {	/* echo serial to LCD */
			kputc(c == '\r' ? '\n' : c);
			fbcon_putc(c == '\r' ? '\n' : c);
		}
		fbcon_flush();
		hal_delay_ms(10);
	}
}
