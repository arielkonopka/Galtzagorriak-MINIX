# Security finding S13: `umap()` doesn't bound user buffers (found 2026-11-14 while reviewing #2)

**Severity: critical.** Any user process can read or write kernel memory and other processes' memory through an ordinary `read()`/`write()` buffer.

## Where
`src/kernel/system.c`, `umap()`, path `CHIP == ARM_CM`, `SHADOWING == 1` (main @ 9708f4d):

```c
if (seg != T)
	seg = (vc < rp->p_map[D].mem_vir + rp->p_map[D].mem_len ? D : S);
if ((vir_addr>>CLICK_SHIFT) >= rp->p_map[seg].mem_vir + rp->p_map[seg].mem_len)
	return 0;
pa = vir_addr;          /* identity: virtual == physical */
```

1. **No lower bound.** Virtual addresses are physical here, so `mem_vir` is the segment's physical start. Nothing rejects `vir_addr < mem_vir`. The kernel sits at the bottom of SRAM1 (0x20000008…), below every process, so `read(fd, (char *) 0x20000100, n)` passes `numap()` and FS or a driver writes into the kernel. `write()` from the same address leaks kernel memory. In original MINIX, virtual addresses were offsets from 0, so this case couldn't happen. The identity mapping removed that protection.
2. **Only the start is checked against the end.** If a buffer starts inside D and runs past the end of S, `seg` becomes S (because `vc` ≥ D end), and only the start is compared with S's end. The copy then runs into whatever process lies above.

`numap()`, `do_copy`/`sys_copy`, `do_vcopy` and all device drivers (FS, TTY, MEM, flipdev, disk) depend on `umap`, so all of them are affected.

## Fix (spec R4a, add as R4a.9)
- For D/S: accept only if `vir_addr >> CLICK_SHIFT >= p_map[D].mem_vir` and the last click `vc` < `p_map[S].mem_vir + p_map[S].mem_len`. The whole range must lie in D..S (the gap between break and stack can stay allowed, as it is the process's own).
- For T: `p_map[T].mem_vir <= start` and `vc < mem_vir + mem_len`.
- Watch out for the shadowing path (`p_shadow`) and for servers (MM/FS) that legitimately copy their own or ABS memory.

## Consequences for code that already merged
- `mm/exec.c gather()` (#2) reads argv/envp strings with `sys_copy(who, D, …)`. String literals live in `.rodata` in the **T** segment (flash or RAM text), so today this works *only because of the bug*. After the fix, `gather()` must try `T` when `D` fails, or `execl("/bin/sh", "sh", …)`, `system()` and `popen()` will get EFAULT.
- The same applies to any driver that reads a user buffer which may point into rodata (for example `write(fd, "literal", n)` through FS). FS reads user data as D. Allow a read-only fallback to T for *source* buffers (copy from user), and never for destination buffers.

## Test (AC)
- `tests/umapk`: `read(open("/dev/zero"), (char *) 0x20000100, 16)` → must return -1/EFAULT, and the kernel must keep running.
- `write(1, (char *) 0x20000100, 16)` → EFAULT.
- A buffer starting at the top of the stack with length 64 KB → EFAULT.
- `write(1, "literal\n", 8)` and `execl("/bin/echo", "echo", "ok", NULL)` still work.
