/* hidtest - libhid (spec R11.3.2) on the PC.
 *
 * - the built-in US layout is the stock en-US.kl, byte for byte;
 * - layout files: exactly HID_KL_SIZE bytes, else EINVAL (and US, never
 *   half a layout); missing: ENOENT; names too long refused;
 * - all printable ASCII (and tab, newline), typed in US and in DE (the
 *   stock de-DE.kl), comes back through a reverse table of the layout:
 *   every character a key report, then an all-up report;
 * - UTF-8: multi-byte characters with no key are skipped and counted,
 *   malformed sequences (overlong, surrogates, > U+10FFFF, cut short)
 *   give -1 and move on by one byte;
 * - mouse reports clamp to -127..127, consumer usages are little endian.
 *
 * Usage: hidtest LAYOUTDIR (src/lib/hid/layouts).  Exit status: failures.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "libhid.h"

static int fails;
static const char *dir;

static void check(int ok, const char *what)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) fails++;
}

static char *path(const char *name)
{
	static char p[512];

	snprintf(p, sizeof p, "%s/%s", dir, name);
	return p;
}

static int file_of(const char *p, int n)
{
	unsigned char b[600];
	int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600), r;

	memset(b, 0x11, sizeof b);
	if (fd < 0) return -1;
	r = (int) write(fd, b, (size_t) n);
	close(fd);
	return r == n ? 0 : -1;
}

/* Type every printable ASCII character (and tab, newline) with layout kl,
 * read the reports back and turn them into characters again. */
static void roundtrip(const unsigned char *kl, const char *name)
{
	char text[128], back[128], what[128];
	unsigned char r[16];
	static int rev[0x10000];	/* modifiers << 8 | usage -> char + 1 */
	int fd, i, n, skipped, dup = 0, ok = 1, typed;
	char tmp[] = "/tmp/hidtestXXXXXX";
	unsigned k;

	memset(rev, 0, sizeof rev);
	n = 0;
	text[n++] = '\t';
	text[n++] = '\n';
	for (i = 0x20; i < 0x7F; i++) text[n++] = (char) i;
	text[n] = 0;
	for (i = 0; i < n; i++) {
		k = hid_key(kl, (unsigned char) text[i]);
		if (k == 0) {
			snprintf(what, sizeof what, "%s: '%c' has a key", name,
								text[i]);
			check(0, what);
			continue;
		}
		if (rev[k] != 0) dup++;
		rev[k] = (unsigned char) text[i] + 1;
	}
	snprintf(what, sizeof what, "%s: every printable character has its "
					"own key (reverse table)", name);
	check(dup == 0, what);

	if ((fd = mkstemp(tmp)) < 0) {
		check(0, "mkstemp");
		return;
	}
	unlink(tmp);
	typed = hid_type(fd, kl, text, &skipped);
	snprintf(what, sizeof what, "%s: hid_type typed all %d, none skipped",
								name, n);
	check(typed == n && skipped == 0, what);
	lseek(fd, 0L, SEEK_SET);
	for (i = 0; i < n; i++) {
		if (read(fd, r, 16) != 16) {
			ok = 0;
			break;
		}
		k = (unsigned) r[0] << 8 | r[2];
		back[i] = rev[k] ? (char) (rev[k] - 1) : '?';
		if (r[1] != 0 || r[3] || r[4] || r[5] || r[6] || r[7]) ok = 0;
		for (k = 8; k < 16; k++) if (r[k] != 0) ok = 0;	/* key-up */
	}
	back[i] = 0;
	close(fd);
	snprintf(what, sizeof what, "%s: the reports type the text back "
					"(key, then all up)", name);
	check(ok && strcmp(back, text) == 0, what);
}

static void layouts(void)
{
	unsigned char kl[HID_KL_SIZE], file[HID_KL_SIZE];
	char tmp[] = "/tmp/hidklXXXXXX", p[64];
	int fd, r, e;

	fd = open(path("en-US.kl"), O_RDONLY);
	r = fd >= 0 ? (int) read(fd, file, sizeof file) : -1;
	if (fd >= 0) close(fd);
	check(r == HID_KL_SIZE && memcmp(file, hid_us, HID_KL_SIZE) == 0,
		"built-in US is the stock en-US.kl, byte for byte");
	check(hid_kl_read(kl, path("en-US.kl")) == 0
		&& memcmp(kl, hid_us, HID_KL_SIZE) == 0, "hid_kl_read en-US.kl");
	memset(kl, 0, sizeof kl);
	check(hid_kl_load(kl, NULL) == 0 && memcmp(kl, hid_us, sizeof kl) == 0
		&& hid_kl_load(kl, "en-US") == 0, "hid_kl_load: US built in");
	check(hid_kl_load(kl, path("de-DE.kl")) == 0
		&& memcmp(kl, hid_us, sizeof kl) != 0, "hid_kl_load: a path");

	if ((fd = mkstemp(tmp)) >= 0) close(fd);
	snprintf(p, sizeof p, "%s", tmp);
	r = file_of(p, HID_KL_SIZE - 1) == 0 && hid_kl_read(kl, p) < 0
		&& errno == EINVAL && memcmp(kl, hid_us, sizeof kl) == 0;
	check(r, "a layout one byte short: EINVAL, US left in the buffer");
	memset(kl, 0, sizeof kl);
	r = file_of(p, HID_KL_SIZE + 1) == 0 && hid_kl_read(kl, p) < 0
		&& errno == EINVAL && memcmp(kl, hid_us, sizeof kl) == 0;
	check(r, "a layout one byte long: EINVAL");
	r = file_of(p, 512) == 0 && hid_kl_read(kl, p) < 0 && errno == EINVAL;
	check(r, "a 512-byte file: EINVAL");
	r = file_of(p, 0) == 0 && hid_kl_read(kl, p) < 0 && errno == EINVAL;
	check(r, "an empty file: EINVAL");
	unlink(p);
	r = hid_kl_read(kl, p);
	e = errno;
	check(r < 0 && e == ENOENT, "a missing file: ENOENT");
	r = hid_kl_load(kl, "no-such-layout-here");
	e = errno;
	check(r < 0 && e == ENOENT, "a missing name: ENOENT (both places)");
	r = hid_kl_load(kl, "a-name-that-is-much-longer-than-32-chars");
	e = errno;
	check(r < 0 && e == ENAMETOOLONG, "a name too long: ENAMETOOLONG");
}

static void utf8(void)
{
	static const struct { const char *s; long c[4]; } t[] = {
		{ "a", { 'a', 0 } },
		{ "\xc3\xb1", { 0xF1, 0 } },			/* n tilde */
		{ "\xe2\x82\xac", { 0x20AC, 0 } },		/* euro */
		{ "\xf0\x9f\x98\x80", { 0x1F600, 0 } },
		{ "\xc0\x80" "a", { -1, -1, 'a', 0 } },		/* overlong */
		{ "\xe0\x80\x80", { -1, -1, -1, 0 } },		/* overlong */
		{ "\xed\xa0\x80", { -1, -1, -1, 0 } },		/* surrogate */
		{ "\xf4\x90\x80\x80", { -1, -1, -1, -1 } },	/* > 10FFFF */
		{ "\xe2\x82", { -1, -1, 0 } },			/* cut short */
		{ "\x80" "b", { -1, 'b', 0 } },			/* stray */
	};
	const char *s;
	unsigned char r[16];
	int i, j, ok = 1, skipped, fd;
	char tmp[] = "/tmp/hidu8XXXXXX";

	for (i = 0; i < (int) (sizeof t / sizeof t[0]); i++) {
		s = t[i].s;
		for (j = 0; j < 4; j++) {
			long c = hid_utf8(&s);

			if (c != t[i].c[j]) {
				printf("    case %d step %d: %ld\n", i, j, c);
				ok = 0;
			}
			if (c == 0) break;
		}
	}
	check(ok, "UTF-8: code points, malformed sequences skipped bytewise");
	if ((fd = mkstemp(tmp)) < 0) return;
	unlink(tmp);
	i = hid_type(fd, hid_us, "Kaixo! \xc3\xb1\xe2\x82\xac\x01", &skipped);
	check(i == 7 && skipped == 3 && lseek(fd, 0L, SEEK_CUR) == 7 * 16,
		"hid_type: 'Kaixo! ' typed, n tilde, euro and ^A skipped");
	lseek(fd, 0L, SEEK_SET);
	check(read(fd, r, 16) == 16 && r[0] == 0x02 && r[2] == 0x0E,
		"hid_type: 'K' is Shift + k (0x0e)");
	close(fd);
	check(hid_type(-1, hid_us, "a", NULL) < 0 && errno == EBADF,
		"hid_type: a failed write gives -1");
}

static void reports(void)
{
	unsigned char m[4], c[2], k[8];

	hid_mouse(m, 1, -500, 500, -127);
	check(m[0] == 1 && m[1] == 0x81 && m[2] == 0x7F && m[3] == 0x81,
		"hid_mouse clamps to -127..127 (never 0x80)");
	hid_mouse(m, 0, -128, 0, 128);
	check(m[1] == 0x81 && m[3] == 0x7F, "hid_mouse: -128 and 128 clamp");
	hid_cons(c, 0x00E9);
	check(c[0] == 0xE9 && c[1] == 0, "hid_cons: little endian");
	hid_kbd(k, 0x40, 0x14);
	check(k[0] == 0x40 && k[1] == 0 && k[2] == 0x14 && k[7] == 0,
		"hid_kbd: modifiers, 0, the key");
}

int main(int argc, char **argv)
{
	unsigned char de[HID_KL_SIZE];

	if (argc != 2) {
		fprintf(stderr, "usage: hidtest LAYOUTDIR\n");
		return 2;
	}
	dir = argv[1];
	printf("hidtest: libhid (spec R11.3.2)\n");
	layouts();
	roundtrip(hid_us, "US");
	if (hid_kl_read(de, path("de-DE.kl")) != 0) check(0, "read de-DE.kl");
	else roundtrip(de, "DE");
	check(hid_key(de, 'z') == 0x1C && hid_key(de, 'y') == 0x1D
		&& hid_key(de, '@') == 0x4014,
		"DE: z on the US y key (0x1c) and back, @ is AltGr + q");
	utf8();
	reports();
	printf("hidtest: %d failure(s)\n", fails);
	return fails != 0;
}
