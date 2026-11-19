/* fuzz.c - the PC fuzzer of mtpd's container parser (spec AC10.4.6, T29a).
 *
 * libFuzzer (clang -fsanitize=fuzzer,address,undefined): each input is
 * the byte stream a USB host would send, frames and all (mtpfd.c, built
 * with read() and write() pointed here).  The core serves it against a
 * real folder in /tmp (made fresh every 2000 inputs), so the operations
 * run down into the files.  Anything mtpd writes back is checked to be
 * well formed frames.  A crash, a sanitizer report, a leak or a hang
 * (libFuzzer -timeout) fails.
 *
 *   make -C flipper/mtpd fuzz			a short run (CI)
 *   make -C flipper/mtpd fuzz FUZZTIME=3600	an hour (AC10.4.6)
 */
#include <sys/stat.h>
#include <dirent.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mtp.h"

void tr_fd(int in, int out);

static const uint8_t *in;
static size_t inlen, inpos;
static unsigned long runs;
static char root[64];
static unsigned outstate, outleft;	/* the frame checker */
static uint8_t outhdr[2];

ssize_t fz_read(int fd, void *buf, size_t n)
{
	size_t k = inlen - inpos;

	(void) fd;
	if (k > n) k = n;
	/* Short reads too: the first input byte steers their size. */
	if (k > 1 && inlen > 0 && (in[0] & 1)) k = 1 + (inpos % k);
	memcpy(buf, in + inpos, k);
	inpos += k;
	return k;
}

/* mtpd's output: frames of 1..1024 bytes, or events of 1..1024. */
ssize_t fz_write(int fd, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	size_t i;
	unsigned f;

	(void) fd;
	for (i = 0; i < n; i++) {
		if (outleft > 0) { outleft--; continue; }
		outhdr[outstate++] = p[i];
		if (outstate < 2) continue;
		outstate = 0;
		f = (outhdr[0] | outhdr[1] << 8) & 0x7FFF;
		if (f == 0 || f > 1024) abort();	/* a broken frame */
		outleft = f;
	}
	return n;
}

static void rm_r(const char *dir)
{
	char p[512];
	struct dirent *e;
	struct stat st;
	DIR *d = opendir(dir);

	if (d == NULL) return;
	while ((e = readdir(d)) != NULL) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
		if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
			rm_r(p);
			rmdir(p);
		} else {
			unlink(p);
		}
	}
	closedir(d);
}

static void fresh(void)
{
	char p[128];
	FILE *f;

	rm_r(root);
	snprintf(p, sizeof(p), "%s/galtzagorriak", root);
	mkdir(p, 0777);
	snprintf(p, sizeof(p), "%s/galtzagorriak/etxea.sub", root);
	if ((f = fopen(p, "w")) != NULL) {
		fputs("Filetype: Flipper SubGhz Key File\n", f);
		fclose(f);
	}
	snprintf(p, sizeof(p), "%s/README", root);
	if ((f = fopen(p, "w")) != NULL) fclose(f);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (root[0] == 0) {
		snprintf(root, sizeof(root), "/tmp/mtpfuzz.%d", (int) getpid());
		mkdir(root, 0700);
	}
	if (runs++ % 2000 == 0) fresh();
	in = data;
	inlen = size;
	inpos = 0;
	outstate = outleft = 0;
	tr_fd(0, 1);
	if (fs_init(root) < 0) abort();
	mtp_init();
	while (mtp_step())
		;
	fs_list_close();
	fs_close();
	if (outstate != 0 || outleft != 0) abort();	/* a frame cut short */
	return 0;
}
