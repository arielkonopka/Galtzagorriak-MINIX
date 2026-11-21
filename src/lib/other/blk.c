/* blk_open() ... blk_write() - swappable memory blocks	(spec 12h, R12.38)
 *
 * Thin wrappers around MEMINFO MI_BLK (R12.36): the request descriptor
 * lives on the caller's stack, MM reads it with sys_copy.  See <blk.h>.
 * Static (SHLIBC_STATIC): only programs that use blocks carry it.
 */
#include <lib.h>
#include <errno.h>
#include <minix/callnr.h>
#include <blk.h>

static int blkcall(int op, int flags, unsigned long id, unsigned first,
					unsigned count, const void *buf)
{
	struct blkreq rq;
	message m;

	rq.op = op;
	rq.flags = flags;
	rq.first = first;
	rq.count = count;
	rq.id = id;
	rq.buf = (char *) buf;
	rq.status = 0;
	m.m1_i2 = MI_BLK;
	m.m1_p1 = (char *) &rq;
	return _syscall(MM, MEMINFO, &m);
}

int blk_open(unsigned long bytes, int flags)
{
	return blkcall(BLK_OPEN, flags, bytes, 0, 0, (void *) 0);
}

int blk_close(void)
{
	return blkcall(BLK_CLOSE, 0, 0, 0, 0, (void *) 0);
}

int blk_info(struct blkinfo *bi)
{
	return blkcall(BLK_INFO, 0, 0, 0, 0, bi);
}

int blk_get(unsigned long id, void *buf)
{
	return blkcall(BLK_GET, 0, id, 0, BLK_NSECT, buf);
}

int blk_put(unsigned long id, const void *buf)
{
	return blkcall(BLK_PUT, 0, id, 0, BLK_NSECT, buf);
}

int blk_putf(unsigned long id, const void *buf, int flags)
{
	return blkcall(BLK_PUT, flags, id, 0, BLK_NSECT, buf);
}

static int part(int op, unsigned long id, unsigned off, const void *buf,
							unsigned len)
{
	if (len == 0 || (off | len) % BLK_SECT != 0 || off >= BLK_SIZE
					|| len > BLK_SIZE - off) {
		errno = EINVAL;
		return -1;
	}
	return blkcall(op, 0, id, off / BLK_SECT, len / BLK_SECT, buf);
}

int blk_read(unsigned long id, unsigned off, void *buf, unsigned len)
{
	return part(BLK_GET, id, off, buf, len);
}

int blk_write(unsigned long id, unsigned off, const void *buf, unsigned len)
{
	return part(BLK_PUT, id, off, buf, len);
}
