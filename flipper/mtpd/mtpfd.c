/* mtpfd.c - mtpd's stream transport (spec R10.4, T29a): MTP over a byte
 * stream on file descriptors, for tests (a pipe on the PC, the second
 * serial line on QEMU).  USB (T29b) brings its own transport.
 *
 * A byte stream has no packets, no interrupt endpoint and no class
 * requests, so it carries USB's three paths in frames, each a 2-byte
 * little-endian header and its bytes:
 *
 *   host -> mtpd	1..1024		that many bytes of bulk OUT
 *			0xFFFF		cancel (USB: the Cancel class request)
 *			0		hang up (USB: the cable pulled); a
 *					serial line has no end of file
 *   mtpd -> host	1..1024		that many bytes of bulk IN
 *			0x8000 | n	an event container of n bytes (interrupt IN)
 *
 * Anything else from the host is a broken stream: mtpd ends, like an
 * unplugged cable.  The containers inside are exactly USB's.
 */
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>
#include "mtp.h"

#define FRAME		1024
#define F_CANCEL	0xFFFF
#define F_HANGUP	0
#define F_EVENT		0x8000

static int in_fd, out_fd = 1;
static unsigned left;		/* bytes of the current OUT frame */

void tr_fd(int in, int out)
{
	in_fd = in;
	out_fd = out;
	left = 0;
}

static int rd_all(void *buf, unsigned n)
{
	char *p = buf;
	int r;

	while (n > 0) {
		r = read(in_fd, p, n);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) return r == 0 ? TR_EOF : TR_ERR;
		p += r;
		n -= r;
	}
	return 1;
}

static int wr_all(const void *buf, unsigned n)
{
	const char *p = buf;
	int r;

	while (n > 0) {
		r = write(out_fd, p, n);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) return TR_ERR;
		p += r;
		n -= r;
	}
	return 0;
}

int tr_read(void *buf, unsigned n)
{
	unsigned char h[2];
	unsigned f;
	int r;

	if (n == 0) return TR_ERR;
	if (left == 0) {
		if ((r = rd_all(h, 2)) != 1) return r;
		f = h[0] | h[1] << 8;
		if (f == F_CANCEL) return TR_CANCEL;
		if (f == F_HANGUP) return TR_EOF;
		if (f > FRAME) return TR_ERR;
		left = f;
	}
	if (n > left) n = left;
	for (;;) {
		r = read(in_fd, buf, n);
		if (r < 0 && errno == EINTR) continue;
		break;
	}
	if (r <= 0) return r == 0 ? TR_EOF : TR_ERR;
	left -= r;
	return r;
}

static int frame(const void *buf, unsigned n, unsigned flag)
{
	unsigned char h[2];

	h[0] = n;
	h[1] = (n | flag) >> 8;
	if (wr_all(h, 2) < 0) return TR_ERR;
	return wr_all(buf, n);
}

int tr_write(const void *buf, unsigned n)
{
	const char *p = buf;
	unsigned k;

	while (n > 0) {
		k = n < FRAME ? n : FRAME;
		if (frame(p, k, 0) < 0) return TR_ERR;
		p += k;
		n -= k;
	}
	return 0;
}

int tr_event(const void *buf, unsigned n)
{
	if (n == 0 || n > FRAME) return TR_ERR;
	return frame(buf, n, F_EVENT);
}
