# 2. Your first C program

This chapter takes a small program from source to the Flipper. Building needs the PC setup of
[the manual's installation chapter](../../manual/install.md) (ARM toolchain, Python 3, and
`qemu-system-arm` if you want to try things without a device).

There is no SDK separate from the source tree yet: programs are built inside it, by
`flipper/minix.mk`, against the same libc as the system's own commands. A stand-alone SDK
(headers + `libc-pic.a` + linker script + `elf2mnx.py` in one archive) would be easy to cut from
it if wanted; it is not planned in the spec.

## 2.1 The example

`flipper/apps/hello.c` is the reference app and is shipped as the package `hello`:

```c
/* hello - the first Galtzagorriak app (an example package). */
#include <sys/types.h>
#include <sys/utsname.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	struct utsname u;

	if (uname(&u) < 0) u.nodename[0] = 0;
	printf("Kaixo! Hello from %s, a galtzagorri at your service.\n",
								u.nodename);
	if (argc > 1) printf("You asked for: %s\n", argv[1]);
	return 0;
}
```

Nothing in it is special: a POSIX program for MINIX 2.0 builds here as long as it fits the
memory limits of [chapter 1](01-platform.md).

## 2.2 Adding your own program

Say the program is `flipper/apps/tally.c`. Three edits to `flipper/minix.mk`:

1. Add it to the app list, so it is built like a command but not put into `/bin`:

   ```make
   APPS	:= hello tally
   ```

   (A program that belongs to the base system goes into `PORTCMDS` instead, with its source in
   `flipper/cmds/`; a screen app linked with the `gui` library goes into `GUIAPPS`, source in
   `flipper/gui/`. See chapter 4.)

2. Give it a stack + heap reserve if the default 6 KB is wrong for it (measure, 2.5):

   ```make
   CHMEM_tally := 4096
   ```

3. Add a package rule next to the `hello` one:

   ```make
   $(PKGDIR)/tally_1.0.tar: $(MB)/bin/tally tools/mkpkg.py
   	python3 -I tools/mkpkg.py -n tally -v 1.0 -o $(PKGDIR) \
   	   -D "Counts things" /usr/bin/tally=$<
   ```

   Add it to `PKGS` only if it should also be on every new SD card in `/lib/pkg`.

A program made of several files sets its object list the way `sh` and `mined` do:
`OBJS_tally = $(MB)/cmd/app/tally.o $(MB)/cmd/app/tallyio.o`.

## 2.3 Building

From the repository root:

```
make -C flipper BOARD=flipper build/flipper/minix/pkgs/tally_1.0.tar
```

What happens on the way, in case something breaks:

1. `arm-none-eabi-gcc` compiles with the user-program flags (`UCFLAGS`: gnu89, `-Os`,
   position-independent with `r9` as the data base).
2. It links against the shared libc in flash (its stubs, `shlibc/stubs.o`, plus the few
   members kept static, `libc-static.a`) with `ld/user-shlibc.ld`, keeping relocations
   (`-Wl,-q`). Only the stubs of the functions you call end up in your text, 12 bytes each.
   The shared libc costs every process its 608-byte data area; a tiny resident program that
   must stay near 1 KB of RAM (`jiggle`, spec R11.3.6) goes into `STATICPROGS` instead and
   links the few libc members it calls statically (`libc-pic.a`, `ld/user-pic.ld`): `jiggle`
   has 188 B of data and bss that way. With so small a stack, call `execv()` with your own
   argument array, not `execl()`: libc's `execl()` keeps a 1 KB array on the stack.
3. `tools/elf2mnx.py` turns the ELF into a MINIX a.out with separate text and data, a
   relocation table, the `chmem` reserve, the ABI number and the id of the libc build. The
   result is `build/flipper/minix/bin/tally`; it runs only with the libc of the same build
   (part 2, 1.6).
4. `tools/mkpkg.py` packs it as `/usr/bin/tally` into `tally_1.0.tar`.

`BOARD=qemu` builds the same thing for the emulator (2.6).

## 2.4 Sending it to the Flipper and running it

1. Log in as root on the USB console and leave it at the shell prompt; close the terminal
   program on the PC.
2. On the PC:

   ```
   flipper/tools/flsend build/flipper/minix/pkgs/tally_1.0.tar
   ```

   `flsend` types `pkg get` on the Flipper and sends the file with a CRC check (about
   100 KB/s). Add `--flash` to also put the program text into internal flash (chapter 5); do
   that for releases, not for every test build.
3. Open the terminal again and run `tally`. `/usr/bin` is in the default `PATH`.

`pkg remove tally` removes it again. While iterating, sending the package each time is the
normal loop; it takes a second or two.

## 2.5 Measuring what it costs

Two numbers matter: how much of its reserve the program really uses, and how much RAM is left
while it runs.

```
free -t on              # MM reports at every exit how much reserve was never touched
tally <worst case>
free -t off
free -v                 # free RAM, the largest hole, and the holes one by one
```

Set `CHMEM_tally` to what was used plus about 2 KB. `free -v` also shows the flash text cache
and the installed region, so you can see whether your program's text is in flash.

## 2.6 Trying it in QEMU

QEMU emulates a Cortex-M4 board (`mps2-an386`) with the same kernel and servers, the SD card as
a disk image, and no Flipper hardware (the device files exist, but the screen, radio and so on
do nothing or report `ENXIO`). It is good for logic, files, pipes and memory; it says nothing
about timing.

```
make -C flipper minix disk         # BOARD=qemu is the default
make -C flipper minix-run          # boots to a login prompt on the terminal
```

To put your program on the QEMU disk, add it to `UPROGS` for the session (it then lands in
`/bin` on the image) or add its package to `PKGS` (it lands in `/lib/pkg`; install it with
`pkg install /lib/pkg/tally_1.0.tar`). `minix-run` stops after 60 seconds by itself; run
`qemu-system-arm` by hand (the line is in `minix.mk`) for longer sessions.

## 2.7 Testing the logic on the PC

The pattern used by `camfind` and `sbctl` (`flipper/apps/camfind`, `flipper/apps/sbctl`):

- Put the logic in a **core file with no I/O and no `malloc`** (`cfcore.c`, `sbproto.c`): it
  takes bytes and returns bytes or a filled-in struct; the caller owns all memory.
- Write a host test (`test_cfcore.c`) and a small `Makefile` with `make test` that builds it
  with the host compiler and `-fsanitize=address,undefined`.
- Add a `target-check` rule that compiles the core with `arm-none-eabi-gcc` in gnu89 mode, so
  it stays buildable for the Flipper.
- The Flipper program around the core is then thin: open devices, call the core, draw.

This catches most bugs before a single flash or SD write, and is what the CI worker (which has
no Flipper) can test.

## 2.8 Conventions for new programs

- **Structured output** (spec K.1): a program that prints records supports `-t`: tab-separated
  values with one header line. Field names carry their unit: `_hz`, `_dbm`, `_ms`, `_mv`, `_c`
  (degrees Celsius); `time` is a UTC `time_t`. Escape `\t`, `\n` and `\\` inside fields. Stream
  it line by line; do not build the table in memory.
- **Settings**: read them with `cfopen()` (chapter 5), keep them in `Key: value` text files,
  and write only to the SD card.
- **Errors**: print `program: what failed: strerror(errno)` to `stderr` and return non-zero.
- **Manual**: a program a user runs gets a section in [`docs/manual/`](../../manual/README.md)
  and a line under `Unreleased` in `CHANGELOG.md` in the PR that adds it.
