/*
 * snprintf, vsnprintf - print formatted output on an array of a given size
 *
 * Galtzagorriak MINIX (spec R3.6).  At most n - 1 characters are stored,
 * always followed by a null character (when n > 0).  Returns the number of
 * characters stored (without the null), or -1 if the output did not fit:
 * then s holds the first n - 1 characters.  (C99 returns the length the
 * whole output would have; _doprnt cannot tell that without a second pass.)
 */

#include	<stdio.h>
#include	<stdarg.h>
#include	"loc_incl.h"

int
vsnprintf(char *s, size_t n, const char *format, va_list arg)
{
	int retval;
	FILE tmp_stream;

	if (n == 0) return -1;
	if (n > 32767) n = 32767;
	tmp_stream._fd     = -1;		/* full: __flushbuf says EOF */
	tmp_stream._flags  = _IOWRITE + _IONBF + _IOWRITING;
	tmp_stream._buf    = (unsigned char *) s;
	tmp_stream._ptr    = (unsigned char *) s;
	tmp_stream._count  = (int) n - 1;

	retval = _doprnt(format, arg, &tmp_stream);
	*tmp_stream._ptr = '\0';		/* at most s[n - 1] */

	return retval < 0 || tmp_stream._count < 0 ? -1 : retval;
}

int
snprintf(char *s, size_t n, const char *format, ...)
{
	va_list ap;
	int retval;

	va_start(ap, format);
	retval = vsnprintf(s, n, format, ap);
	va_end(ap);
	return retval;
}
