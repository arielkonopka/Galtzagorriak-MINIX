/* memset/memcpy/memmove/memcmp/strlen for the freestanding kernel build.
 * GCC may also emit calls to these on its own, so they must exist. */
#include <stddef.h>

void *memset(void *s, int c, size_t n)
{
	unsigned char *p = s;

	while (n-- > 0) *p++ = (unsigned char) c;
	return s;
}

void *memcpy(void *restrict d, const void *restrict s, size_t n)
{
	unsigned char *dp = d;
	const unsigned char *sp = s;

	while (n-- > 0) *dp++ = *sp++;
	return d;
}

void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *dp = d;
	const unsigned char *sp = s;

	if (dp < sp || dp >= sp + n) {
		while (n-- > 0) *dp++ = *sp++;
	} else {
		dp += n;
		sp += n;
		while (n-- > 0) *--dp = *--sp;
	}
	return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;

	for (; n > 0; n--, p++, q++)
		if (*p != *q) return *p - *q;
	return 0;
}

size_t strlen(const char *s)
{
	const char *p = s;

	while (*p != 0) p++;
	return (size_t) (p - s);
}
