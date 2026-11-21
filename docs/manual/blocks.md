# Memory blocks: app data on the SD card

The Flipper gives a program a few KB of RAM. An app that needs more working data (a big map,
a long list, a dictionary) can keep it in **memory blocks**: numbered 4 KB blocks on the SD
card's swap partition `/dev/hd3`. The app reads a block into a buffer of its own and writes it
back from there; nothing is mapped (the Flipper has no MMU). This chapter is for people who
write apps; users only switch blocks on. The design is in the spec, section 12h (R12.34 to
R12.40).

Blocks are **off** by default.

## Switching blocks on

In `/etc/rc.conf` on the card:

```
blk=on
```

and reboot. With `swap=plain` too, `/etc/rc` runs `swapon /dev/hd3 plain blk` (swap and
blocks); with `swap=off` it runs `swapon /dev/hd3 blk` (blocks only, nothing is swapped). With
`swap=crypt` blocks stay off until encrypted swap exists. By hand, as root:

```
swapon off
swapon /dev/hd3 plain blk
```

`swapon` with no arguments shows the state. With blocks off every block call fails with
`ENOSYS`, so an app can fall back to a smaller mode.

## How much space

The space behind the 16 swap slots (3 MB) is the **block space**, cut into 256 extents. On the
64 MB partition of a small card that is about 54 MB of data; on an 8 GB or larger card up to
about 3.6 GB. A block takes 4.5 KB on the card (one header sector with its owner and a CRC of
every sector, then 4 KB of data).

An app gets space **only by asking**: `blk_open(bytes, flags)` reserves a contiguous run of
extents, rounded up, or fails with `ENOSPC`. Nothing else is evicted for it. A program that is
not root can hold at most half of the space, so one app cannot lock out the others.

## Rules an app can rely on

- After `blk_open`, every block of the run exists and reads as zeros until written.
- The reservation ends with `blk_close()`, `exec` or exit. Nothing is written then, and no other
  program can read your old blocks: a block whose header does not name the reader reads as
  zeros. The same holds after a reboot, and when pids are used again.
- A forked child has no reservation of its own.
- Each 512-byte sector carries a CRC-32. A damaged sector gives `EIO` for that sector only; the
  process is not killed.
- If the card is pulled, calls give `ENXIO` and the app lives on; try again later.
- A program in the **background** is served only when the system is idle (no key press, no
  program waiting for the CPU), and its writes count in the swap's write budget of 256 KB a
  minute. The **foreground** app is served at once. So block I/O never slows down the buttons.
- `blk_discard(id, n)` (or `vm_drop()`) says blocks are no longer needed; their contents are
  undefined afterwards. On cards with SD DISCARD the card is told at idle, which saves wear.
  When a run ends, and once when blocks are switched on, the system discards the old data the
  same way.
- In `plain` mode the data is on the card **unencrypted**. Set-uid and set-gid programs and
  packages with `Secret: yes` get `EPERM`: do not put passwords or keys in blocks.

## The two interfaces

**`<blk.h>`, in libc** (`man 3 blk`): whole blocks (`blk_get`, `blk_put`), parts of a block in
512-byte sectors (`blk_read`, `blk_write`), `blk_putf(id, buf, BLK_IFCHANGED)` that writes
nothing when the data is the same, `blk_info` (your run, the largest free run, the extent size),
`blk_discard`.

**`libblk`** (`man 3 libblk`, `<libblk.h>`, static, about 0.8 KB): a byte-addressed space over
the blocks, with frames of 1, 2 or 4 KB in a pool **you** give it (8 B per frame of metadata, no
static data). `vm_get(v, off, VM_RD or VM_RW)` returns a pointer into a frame and writes back the
least recently used changed frame when it needs room; `VM_PIN` keeps a frame until
`vm_unpin()`. `VA_GET`/`VA_SET` index arrays of fixed-size records that never cross a frame.

```c
#include <libblk.h>

static char pool[3 * (1024 + 8)];	/* three 1 KB frames */

int main(void)
{
	struct vm v;
	long *p;

	if (vm_open(&v, pool, sizeof(pool), 256UL * 1024, 1024, 0) < 0)
		return 1;			/* ENOSYS: blocks off */
	p = VA_SET(&v, long, 1000);		/* record 1000 */
	if (p != NULL) *p = 42;
	return vm_close(&v) < 0;		/* write back, give it back */
}
```

Link with `$(MB)/blk/libblk.o` (see `OBJS_blktest` in `flipper/minix.mk`). Good habits:

- Keep the frames few and small; a frame you are not using is RAM nobody else has.
- Write back with `vm_flush()` at points where losing the last changes would be harmful.
  Blocks do not survive a reboot. They are working memory, not files.
- Use `BLK_IFCHANGED` (libblk does not write frames you only read) to save card wear.

## What blocks cost

The memory manager keeps 2 B per process slot for the reservations and a few bytes of state
(39 B in all). Block I/O never writes the Flipper's internal flash.
