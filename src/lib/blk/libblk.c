/* libblk - frames over swappable memory blocks	(spec 12h, R12.38)
 *
 * The app gives a pool of its own memory; vm_open() splits it into frames
 * of 1, 2 or 4 KB and, behind them, one struct vm_frame (8 B) per frame.
 * A frame caches one frame-sized piece ("unit") of the app's block space:
 * unit u is bytes u * fsize .. (u + 1) * fsize - 1, that is block
 * u * fsize / BLK_SIZE, sectors from (u * fsize % BLK_SIZE) / BLK_SECT.
 * A 4 KB frame is a whole block (blk_get/blk_put, one write); a smaller
 * one is a sector range (blk_read/blk_write: header + range).
 *
 * No malloc, no stdio, no static data: everything is in struct vm and
 * the pool, so several pools can live side by side.  The frames are
 * found by a linear search (pools have a few frames); the victim is the
 * least recently used unpinned frame.  See <libblk.h>, libblk(3).
 */
#include <errno.h>
#include <libblk.h>

#define VF_DIRTY	0x01

static int fail(struct vm *v, int e)
{
	v->err = e;
	errno = e;
	return -1;
}

int vm_open(struct vm *v, void *pool, unsigned poolsize, unsigned long size,
						unsigned framesize, int flags)
{
	unsigned n, i;
	int r;

	v->err = 0;
	if (framesize != 1024 && framesize != 2048 && framesize != 4096)
		return fail(v, EINVAL);
	n = poolsize / (framesize + sizeof(struct vm_frame));
	if (n == 0 || n > 0xFFFF || ((unsigned long) pool & 3) != 0)
		return fail(v, EINVAL);
	if ((r = blk_open(size, flags)) < 0) return fail(v, errno);
	v->pool = (char *) pool;
	v->f = (struct vm_frame *) (v->pool + (unsigned long) n * framesize);
	v->size = (unsigned long) r * BLK_SIZE;
	v->fsize = framesize;
	v->nframes = n;
	v->clock = 0;
	for (i = 0; i < n; i++) {
		v->f[i].unit = VM_NONE;
		v->f[i].age = 0;
		v->f[i].flags = 0;
		v->f[i].pins = 0;
	}
	return 0;
}

/* Frame i to the card (it is dirty).  0 or -1; it stays dirty on -1. */
static int wr(struct vm *v, unsigned i)
{
	unsigned long pos = (unsigned long) v->f[i].unit * v->fsize;
	char *p = v->pool + (unsigned long) i * v->fsize;
	int r;

	if (v->fsize == BLK_SIZE) r = blk_put(pos / BLK_SIZE, p);
	else r = blk_write(pos / BLK_SIZE, (unsigned) (pos % BLK_SIZE), p,
								v->fsize);
	if (r < 0) return fail(v, errno);
	v->f[i].flags &= ~VF_DIRTY;
	return 0;
}

static int rd(struct vm *v, unsigned i, unsigned long unit)
{
	unsigned long pos = unit * v->fsize;
	char *p = v->pool + (unsigned long) i * v->fsize;
	int r;

	if (v->fsize == BLK_SIZE) r = blk_get(pos / BLK_SIZE, p);
	else r = blk_read(pos / BLK_SIZE, (unsigned) (pos % BLK_SIZE), p,
								v->fsize);
	if (r < 0) return fail(v, errno);
	return 0;
}

static int find(struct vm *v, unsigned long unit)
{
	unsigned i;

	for (i = 0; i < v->nframes; i++)
		if (v->f[i].unit == unit) return (int) i;
	return -1;
}

void *vm_get(struct vm *v, unsigned long off, int mode)
{
	unsigned long unit;
	unsigned i, best = 0;
	unsigned short a, old = 0;
	int k, found = 0;
	struct vm_frame *f;

	if (off >= v->size) {
		fail(v, ERANGE);
		return (void *) 0;
	}
	unit = off / v->fsize;
	if ((k = find(v, unit)) < 0) {
		/* An empty frame, else the least recently used unpinned. */
		for (i = 0; i < v->nframes; i++) {
			f = &v->f[i];
			if (f->pins != 0) continue;
			if (f->unit == VM_NONE) { best = i; found = 1; break; }
			a = (unsigned short) (v->clock - f->age);
			if (!found || a > old) { best = i; old = a; found = 1; }
		}
		if (!found) {
			fail(v, EBUSY);
			return (void *) 0;
		}
		f = &v->f[best];
		if ((f->flags & VF_DIRTY) && wr(v, best) < 0) return (void *) 0;
		f->unit = VM_NONE;
		if (rd(v, best, unit) < 0) return (void *) 0;
		f->unit = (unsigned) unit;
		f->flags = 0;
		k = (int) best;
	}
	f = &v->f[k];
	f->age = ++v->clock;
	if (mode & VM_RW) f->flags |= VF_DIRTY;
	if ((mode & VM_PIN) && f->pins < 0xFF) f->pins++;
	return v->pool + (unsigned long) k * v->fsize + off % v->fsize;
}

void vm_unpin(struct vm *v, unsigned long off)
{
	int k = find(v, off / v->fsize);

	if (k >= 0 && v->f[k].pins > 0) v->f[k].pins--;
}

int vm_flush(struct vm *v)
{
	unsigned i;
	int r = 0;

	for (i = 0; i < v->nframes; i++)
		if ((v->f[i].flags & VF_DIRTY) && wr(v, i) < 0) r = -1;
	if (r < 0) errno = v->err;
	return r;
}

void vm_drop(struct vm *v, unsigned long off, unsigned long len)
{
	unsigned long u0, u1;
	unsigned i;

	/* Only frames wholly inside the range: the rest of a frame that
	 * is partly inside still holds the app's data. */
	if (len == 0) return;
	u0 = (off + v->fsize - 1) / v->fsize;
	u1 = (off + len) / v->fsize;	/* first unit past the range */
	if (off + len < off) u1 = VM_NONE;
	for (i = 0; i < v->nframes; i++) {
		if (v->f[i].unit == VM_NONE || v->f[i].unit < u0
					|| v->f[i].unit >= u1) continue;
		v->f[i].unit = VM_NONE;
		v->f[i].flags = 0;
		v->f[i].pins = 0;
	}
	/* R12.40: the whole blocks inside the range, at MM's idle. */
	u0 = (off + BLK_SIZE - 1) / BLK_SIZE;
	u1 = off + len < off ? (unsigned long) -1 / BLK_SIZE
			     : (off + len) / BLK_SIZE;
	if (u1 > u0) (void) blk_discard(u0, u1 - u0);
}

int vm_close(struct vm *v)
{
	int r = vm_flush(v);

	if (blk_close() < 0) r = -1;
	return r;
}
