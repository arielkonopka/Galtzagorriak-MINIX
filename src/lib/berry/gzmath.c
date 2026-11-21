/* fmodf for the Berry VM (real % real) on the device; see gzmath.h. */
#include "gzmath.h"

float gz_fmodf(float x, float y)
{
	float q;

	if (y == 0.0f || isnan(x) || isnan(y) || isinf(x)) return NAN;
	if (isinf(y)) return x;
	q = x / y;
	/* |q| >= 2^24: every float is an integer, the remainder is lost */
	if (q >= 16777216.0f || q <= -16777216.0f) return 0.0f;
	return x - (float)(long)q * y;	/* truncation toward zero, as C */
}

#include <stdarg.h>
#include <string.h>

/* digits of |v| (finite, >= 0) as d.ddd * 10^e with prec digits after
 * the point (fixed: f, otherwise e); returns the length in out. */
static int fmt_fixed(char *out, float v, int prec)
{
	char *p = out;
	unsigned long ip;
	float fp;
	int i;

	for (i = 0, fp = 0.5f; i < prec; i++) fp /= 10.0f;
	v += fp;				/* round */
	if (v >= 4294967040.0f) {		/* beyond 32 bits: e form */
		return -1;
	}
	ip = (unsigned long)v;
	fp = v - (float)ip;
	{
		char t[12];
		int k = 0;
		do { t[k++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
		while (k) *p++ = t[--k];
	}
	if (prec > 0) {
		*p++ = '.';
		for (i = 0; i < prec; i++) {
			int d;
			fp *= 10.0f;
			d = (int)fp;
			if (d > 9) d = 9;
			*p++ = (char)('0' + d);
			fp -= (float)d;
		}
	}
	*p = '\0';
	return (int)(p - out);
}

static int fmt_exp(char *out, float v, int prec, char e)
{
	int x = 0, n;
	char *p;

	if (v != 0.0f) {
		while (v >= 10.0f) { v /= 10.0f; x++; }
		while (v < 1.0f) { v *= 10.0f; x--; }
	}
	n = fmt_fixed(out, v, prec);
	if (out[0] == '1' && out[1] == '0') {	/* 9.99 rounded to 10.0 */
		n = fmt_fixed(out, v / 10.0f, prec);
		x++;
	}
	p = out + n;
	*p++ = e;
	*p++ = x < 0 ? '-' : '+';
	if (x < 0) x = -x;
	*p++ = (char)('0' + x / 10);
	*p++ = (char)('0' + x % 10);
	*p = '\0';
	return (int)(p - out);
}

/* %g: prec significant digits, trailing zeros dropped (no '#'). */
static int fmt_g(char *out, float v, int prec, char e)
{
	int x = 0, n;
	float a = v;
	char *p, *d;

	if (prec == 0) prec = 1;
	if (a != 0.0f) {
		while (a >= 10.0f) { a /= 10.0f; x++; }
		while (a < 1.0f) { a *= 10.0f; x--; }
	}
	if (x < -4 || x >= prec || fmt_fixed(out, v, prec - 1 - x) < 0)
		n = fmt_exp(out, v, prec - 1, e);
	else
		n = (int)strlen(out);
	/* drop the trailing zeros of the fraction */
	p = strchr(out, '.');
	if (p) {
		d = out + n;
		e = 0;
		{
			char *q = p;
			while (*q && *q != 'e' && *q != 'E') q++;
			if (*q) { e = 1; d = q; }
		}
		{
			char *z = d;
			while (z > p + 1 && z[-1] == '0') z--;
			if (z == p + 1) z = p;
			memmove(z, d, strlen(d) + 1);
		}
		(void)e;
	}
	return (int)strlen(out);
}

int gz_snprintf(char *buf, size_t n, const char *fmt, ...)
{
	va_list ap;
	const char *s, *spec = NULL, *end = NULL;
	char num[48], *q;
	int left = 0, zero = 0, plus = 0, space = 0, width = 0, prec = 6;
	int len, pad, r;
	float v;
	size_t k;

	for (s = fmt; *s; s++) {
		if (*s != '%') continue;
		if (s[1] == '%') { s++; continue; }
		end = s + 1;
		while (*end && strchr("-+ #0", *end)) end++;
		while (*end && (strchr("0123456789.", *end))) end++;
		while (*end == 'l' || *end == 'L' || *end == 'h') end++;
		if (*end && strchr("eEfFgG", *end)) { spec = s; break; }
		end = NULL;
	}
	va_start(ap, fmt);
	if (spec == NULL) {
		r = vsnprintf(buf, n, fmt, ap);
		va_end(ap);
		return r;
	}
	v = (float)va_arg(ap, double);
	va_end(ap);
	for (s = spec + 1; strchr("-+ #0", *s); s++) {
		if (*s == '-') left = 1;
		if (*s == '0') zero = 1;
		if (*s == '+') plus = 1;
		if (*s == ' ') space = 1;
	}
	while (*s >= '0' && *s <= '9') width = width * 10 + (*s++ - '0');
	if (*s == '.') {
		prec = 0;
		s++;
		while (*s >= '0' && *s <= '9') prec = prec * 10 + (*s++ - '0');
	}
	if (prec > 20) prec = 20;
	q = num;
	if (v < 0.0f || (v == 0.0f && 1.0f / v < 0.0f)) { *q++ = '-'; v = -v; }
	else if (plus) *q++ = '+';
	else if (space) *q++ = ' ';
	if (isnan(v)) { strcpy(q, "nan"); zero = 0; }
	else if (isinf(v)) { strcpy(q, "inf"); zero = 0; }
	else if (*end == 'f' || *end == 'F') {
		if (fmt_fixed(q, v, prec) < 0) fmt_exp(q, v, prec, 'e');
	} else if (*end == 'e' || *end == 'E') fmt_exp(q, v, prec, *end);
	else fmt_g(q, v, prec, *end == 'G' ? 'E' : 'e');
	len = (int)strlen(num);
	pad = width > len ? width - len : 0;

	/* prefix, the padded number, suffix */
	r = 0;
#define PUT(c) do { if ((size_t)r + 1 < n) buf[r] = (c); r++; } while (0)
	for (s = fmt; s < spec; s++) {
		if (*s == '%' && s[1] == '%') s++;
		PUT(*s);
	}
	if (!left && !zero) while (pad-- > 0) PUT(' ');
	q = num;
	if (zero && !left && (*q == '-' || *q == '+' || *q == ' ')) PUT(*q++);
	if (zero && !left) while (pad-- > 0) PUT('0');
	while (*q) PUT(*q++);
	if (left) while (pad-- > 0) PUT(' ');
	for (s = end + 1; *s; s++) {
		if (*s == '%' && s[1] == '%') s++;
		PUT(*s);
	}
#undef PUT
	k = (size_t)r < n ? (size_t)r : n - 1;
	if (n > 0) buf[k] = '\0';
	return r;
}
