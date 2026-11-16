# 4. Servers and system calls

MM and FS are MINIX 2.0's servers, extended for the port. This chapter shows where new calls go
and what each kind of extension costs. The rule from the owner applies first: **do not rewrite
working MINIX core without need**; touch it for RAM, responsiveness or security, or when a
feature cannot be done outside it.

## 4.1 Choosing the layer

Ask in this order, and stop at the first that works:

1. **A user program or library.** Most features need nothing new in the system (a command, a
   libc helper such as `cfopen()`, a `libapp` function).
2. **A device file** (Part 2, chapter 3). Hardware and hardware-like services: a minor, maybe an
   `ioctl`.
3. **A control on an existing call.** MM's `MEMINFO` already carries several sub-operations
   (4.3); a new MM control often fits there without a new call number.
4. **A new system call** (4.4), when the operation is a real part of the process or file model.
5. **A new kernel call** (4.5), when MM or FS needs something only the kernel can do.

Each step down costs more review, more ways to break invariants I1–I5, and usually RAM.

## 4.2 How a call travels

A libc function builds a `message` and calls `_syscall(MM or FS, CALLNR, &m)`, which traps with
`SVC` into `sendrec`. The server's main loop receives it, dispatches through its `call_vec`
table (`src/mm/table.c`, `src/fs/table.c`) to `do_xxx()`, and replies. A server needing the
kernel calls a `sys_xxx()` stub (`src/lib/syslib/`), which sends to the SYS task
(`src/kernel/system.c`). Servers and tasks talk with `_taskcall()` and `sendrec()`.

Call numbers are in `include/minix/callnr.h` (`NCALLS` is 77). Port additions so far:
`VFORK` (57), `MEMINFO` (69, to MM), `INOCHK` (70, to FS, accepted from MM only).

## 4.3 `MEMINFO`: MM's control call

`do_meminfo()` in `src/mm/alloc.c` returns free memory and the holes, and dispatches on
`m1_i2` to sub-operations (`MI_*` in `include/minix/type.h`):

| `m1_i2` | Operation | Used by |
|---|---|---|
| 0 | free total, largest hole, the hole list | `free` |
| `MI_KSTACKS` | kernel and server stack high-water marks | `free -k` |
| `MI_TRACE` | report unused `chmem` at exit (root) | `free -t` |
| `MI_FCACHE` | flash text cache and installed-region summary | `free -v` |
| `MI_INSTALL` | install, flash, bind programs; one step per call (root) | `pkg` |
| `MI_RESET` | cause of the last reset | `ver` |
| `MI_FCBUDGET` | run-time cache erase budget and counters (setting it: root) | `pkg budget` |

This MINIX has no `svrctl`, so MM settings go through here. A new MM control is a new `MI_*`
value, a branch in `do_meminfo()`, the permission check first (`mp->mp_effuid != SUPER_USER`
for anything that changes state), and a user command that calls it. 0 B RAM if it needs no new
state.

## 4.4 Adding a system call

1. A number in `callnr.h` (raise `NCALLS` if it goes past the end).
2. The handler `do_xxx()` in MM or FS, declared in the server's `proto.h`, entered in its
   `call_vec` (`table.c`). The other server's table gets `no_sys` at that slot.
3. A libc stub in `src/lib/posix/` or `src/lib/other/` (and, for a `_xxx` with an alias, the
   alias list in `src/lib/syscall/`, from which `tools/genaliases.py` makes the ARM aliases).
4. A man page in `man/`, a line in `CHANGELOG.md`, and an m2test case if QEMU can exercise it.
5. A security test in `flipper/tests/` if the call takes pointers, ids or sizes from the user.

Rules for the handler:
- Copy user data with `sys_copy()` between the caller's `D` space and the server; never trust
  a size, an offset or a pid from the message without a bounds check (invariant I2).
- Check permissions explicitly. "Only root", "only the owner", "only from MM" (`who ==
  MM_PROC_NR`, as `INOCHK` does) are all patterns in the tree.
- Keep it short. A server call runs to completion while every other process that needs that
  server waits. Long work is split into steps the caller repeats (see `MI_INSTALL`: one page
  erase or 256 bytes per call, so `fork`/`exec` of other processes go on in between; this is
  what keeps L6 and L9 within budget during an install).

## 4.5 Kernel calls

A kernel call is a `SYS_*` code in `include/minix/com.h`, a `case` in `sys_task()` and a
`do_xxx()` in `src/kernel/system.c`, and a stub in `src/lib/syslib/`. Port additions:
`SYS_KSTACKS` (stack marks) and `SYS_FLASH` (flash erase and program for MM, only inside the
writable regions; the ROM image is refused with `EROFS`).

Kernel calls are for MM and FS, not for user processes. A kernel call that writes memory must go through
`numap()`/`numap_src()` for the target process's map or through unprivileged accesses
(invariant I1). Anything longer than about 1 ms must be split (R0.8b).

## 4.6 Server memory

MM and FS have fixed tables sized for 16 processes: FS 48 file slots, 40 in-core inodes and 4 super
blocks (root, `/usr`, `/home` and `/rom` use all four, so another mount needs a slot first);
MM 32 holes. Their static RAM is in `ram-budget.txt` (MM about 10.2 KB, FS about
27.2 KB with the SRAM2 buffer cache), and their stacks are 1 KB (MM) and 2 KB (FS); `free -k`
shows the high-water marks. A new feature in a server must fit these or come with a budget line
in the spec. The tricks that worked so far: state that lives in the process table entries that
already exist, bits in existing words, and work done in the caller's memory instead of a server
buffer.

## 4.7 Where things are planned to go

- `poll()` in FS with a `DEV_SELECT` request to device tasks (R4b.6).
- Supplementary groups in FS and MM (R4b.4).
- Swap-out and swap-in of blocked processes in MM, to the SD card only (Phase 12).
- `setitimer` subset on the ms timer (R4b.2, with R4b.6).

Read the spec section before starting on any of them: each has a RAM budget and acceptance
tests.
