/* Minimal printf for the debug console: %c %s %d %i %u %x %X %p %%, with
 * optional '-', '0', field width and 'l' modifier.  No floating point. */
#include <stdarg.h>
#include "hal.h"

void kputc(int c)
{
	char ch = c;

	if (c == '\n') kputc('\r');
	hal_uart_putc(c);
	hal_usb_write(&ch, 1);		/* also to the USB serial port */
}

static void pad(int n, int c)
{
	while (n-- > 0) kputc(c);
}

static void put_num(unsigned long v, unsigned base, int upper, int neg,
		    int width, int zero, int left)
{
	char buf[12];
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int n = 0, ndigits;

	do {
		buf[n++] = digits[v % base];
		v /= base;
	} while (v != 0);
	ndigits = n;
	if (neg) width--;
	if (!left && !zero) pad(width - ndigits, ' ');
	if (neg) kputc('-');
	if (!left && zero) pad(width - ndigits, '0');
	while (n > 0) kputc(buf[--n]);
	if (left) pad(width - ndigits, ' ');
}

void kprintf(const char *fmt, ...)
{
	va_list ap;
	const char *s;
	int width, zero, left, len, lng;
	long sv;

	va_start(ap, fmt);
	for (; *fmt != 0; fmt++) {
		if (*fmt != '%') {
			kputc(*fmt);
			continue;
		}
		fmt++;
		left = zero = 0;
		if (*fmt == '-') { left = 1; fmt++; }
		if (*fmt == '0') { zero = 1; fmt++; }
		for (width = 0; *fmt >= '0' && *fmt <= '9'; fmt++)
			width = width * 10 + (*fmt - '0');
		lng = 0;
		if (*fmt == 'l') { lng = 1; fmt++; }
		switch (*fmt) {
		case 'c':
			kputc(va_arg(ap, int));
			break;
		case 's':
			s = va_arg(ap, const char *);
			if (s == 0) s = "(null)";
			for (len = 0; s[len] != 0; len++) ;
			if (!left) pad(width - len, ' ');
			while (*s != 0) kputc(*s++);
			if (left) pad(width - len, ' ');
			break;
		case 'd':
		case 'i':
			sv = lng ? va_arg(ap, long) : va_arg(ap, int);
			put_num(sv < 0 ? -(unsigned long) sv : (unsigned long) sv,
				10, 0, sv < 0, width, zero, left);
			break;
		case 'u':
			put_num(lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned),
				10, 0, 0, width, zero, left);
			break;
		case 'x':
		case 'X':
			put_num(lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned),
				16, *fmt == 'X', 0,
				width, zero, left);
			break;
		case 'p':
			kputc('0'); kputc('x');
			put_num((unsigned long) va_arg(ap, void *), 16, 0, 0, 8, 1, 0);
			break;
		case '%':
			kputc('%');
			break;
		case 0:
			fmt--;
			break;
		default:
			kputc('%');
			kputc(*fmt);
		}
	}
	va_end(ap);
}
