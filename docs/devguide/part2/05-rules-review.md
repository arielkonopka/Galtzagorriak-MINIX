# 5. Rules and review

Every change to Galtzagorriak MINIX is reviewed against the same rules. They come from the user
and are written down in the spec (section 0) and in the standing rules at the top of
[`docs/spec/next-tasks.md`](../../spec/next-tasks.md); this chapter collects them with the
reasons, and ends with a checklist for a PR.

## 5.1 Responsiveness first

The device must feel at least as quick as the stock firmware. When RAM and responsiveness
conflict, **responsiveness wins** (user, 2026-11-14). The budgets (spec rule 0.8):

| ID | Path | Budget |
|---|---|---|
| L1 | button press to the first changed pixel | ≤ 30 ms p99, ≤ 50 ms max, under load |
| L2 | button press to `/dev/buttons` answered | ≤ 2 ms p99 |
| L3 | interrupt latency for radio and pulse interrupts | ≤ 5 µs |
| L4 | longest stretch with interrupts masked | ≤ 20 µs (iButton slot ≤ 70 µs) |
| L5 | REVIVE to the process running | ≤ 100 µs |
| L6 | launcher to the app's first frame | ≤ 300 ms (≤ 600 ms if swapped) |
| L7 | app exit to the menu redrawn | ≤ 150 ms |
| L8 | flash stall during a radio stream, BLE connection or USB HID session | 0: not allowed |
| L9 | UI freeze from a flash write at other times | ≤ 25 ms, at most one page per 100 ms |
| L10 | boot to the launcher menu | ≤ 3 s |

Mechanisms that follow from them: no kernel path longer than 1 ms with the scheduler held (split
long copies into ≤ 4 KB chunks, R0.8b); flash writes only as idle work, one page per step
(R0.8c); swap never stalls the foreground (R0.8d). A PR that touches a measured path reports
`lat` numbers before and after, from the device, in `PORTING.md`.

## 5.2 RAM

- **Static RAM** of kernel, MM, FS and INIT may not grow beyond the phase's budget (rule 0.1).
  `make ramcheck` enforces `flipper/ram-budget.txt`; raising a line needs the reason in the same
  commit and normally the user's acceptance.
- **Per-process RAM** (data, bss, `chmem`) of system programs is measured (`free -t on`) and
  recorded in `minix.mk`. No base program above 4 KB of `.bss` (R2a.5): `make ramcheck` also runs
  `tools/bsscheck.py`; an exception needs a line with its reason in `flipper/bss-exceptions.txt`.
- **Hardware first** (rule 0.7): when a peripheral can hold the data or do the work, use it
  (RTC backup registers, the LED chip's engines, FIFOs, DMA, the flash for constant data). A
  change that buffers data in RAM says why the hardware option does not apply.
- Zero-RAM solutions are always welcome; `const` tables, bit fields in existing words and
  shared unions are the usual tools.

## 5.3 Flash writes

Internal flash wears (about 10,000 erases per page) and stalls the CPU while it is written. The
user's rule:

- **write it as rarely as possible, above all at run time**; build and install time writes are
  acceptable but should also be rare: compare before erase, append instead of rewrite, compact
  only when full;
- **no file data in internal flash** (spec L.1): settings, logs, keys and app data go to the SD
  card;
- every PR that can write flash at run time says **how often** in normal use and why the write
  cannot happen at build or install time;
- CI counts the erases of the self-test against `flipper/flash-budget.txt`, a ratchet;
- writes must survive a power cut at any point (`tools/powercut.py`).

## 5.4 Security invariants

Once established, these hold forever and every review re-checks them (spec 0.6):

- **I1**: kernel code touches a user address only after checking it against that process's map,
  or with unprivileged accesses;
- **I2**: no user-controlled value (a file header, a message, a sigcontext) decides privileged
  write addresses without a bounds check;
- **I3**: every user process runs with MPU isolation; there is no whole-RAM fallback;
- **I4**: code run with a set-uid identity comes from that file and nowhere else;
- **I5**: no DMA into or out of a process's memory outlives the process's ownership of it;
- **I6**: the L1–L10 budgets hold.

A security fix comes with a test in `flipper/tests/` that tries the attack and reports `PASS`
when it is refused.

## 5.5 Other standing decisions

- **Do not rewrite working MINIX core** without need; change it for RAM, responsiveness or
  security.
- **No kernel threads**; no TLS on the device (offload to an external module if ever needed).
- **Licence**: the project may become GPLv3; code from the stock Flipper firmware may be reused
  freely. Keep upstream copyright notices and say where code came from (spec D.3).
- **Option bytes** are not touched except as spec R17.4 plans, after Bluetooth works.
- **Code style**: the MINIX sources are K&R C with `PUBLIC`/`PRIVATE`/`FORWARD _PROTOTYPE`;
  follow the style of the file you edit. Comments say *why*, with the spec section
  (`/* R6.3: ... */`), since the reasons are what a later change needs.

## 5.6 Documentation follows behaviour

- A PR that changes something a user sees or does updates the matching chapter of
  [`docs/manual/`](../../manual/README.md) in the same PR, or says why not with a
  `Manual: ...` line in its description. A feature that lands removes its **(planned)** mark.
- A PR that changes an interface described in this guide updates the guide.
- Every code change adds a line under `Unreleased` in `CHANGELOG.md`, naming the spec item.
- A change to the base system's contents raises `BASE_VER` in `flipper/minix.mk`.
- An incompatible change to the program format or the system call interface raises `GZ_ABI`
  (spec J.3); versions follow spec J.

## 5.7 How work flows

- Work happens on a branch from `main`, one task per branch, and arrives as a PR naming the spec
  phase or task (R6.4, T15a, ...). PRs are reviewed and squash-merged.
- Two implementers work in parallel: the user's local agent (with the Flipper) and the cloud
  worker in GitHub Actions (no hardware). The cloud worker takes only issues labelled
  `claude-task`; the local agent labels issues it takes `claimed:local`. Device-only checks in a
  cloud PR are listed under "Needs on-device test".
- Design changes go into `docs/spec/` by PR first; the spec decides, the code follows.

## 5.8 PR checklist

- [ ] Names the spec item; does what it says and no more.
- [ ] `build` is green: images, `ramcheck`, QEMU `M2 USER PASS`, flash-erase budget, latency
      build, `powercut`.
- [ ] RAM: static change stated (0 B unless budgeted); `chmem` of new programs measured.
- [ ] Responsiveness: no new busy waits, no long masked sections, no long server calls; `lat`
      numbers from the device if a measured path changed.
- [ ] Flash: run-time writes stated with their frequency, or "none".
- [ ] Security: user pointers mapped, sizes and ids checked, permissions explicit; a test for a
      security fix.
- [ ] QEMU HAL updated for new HAL functions.
- [ ] `/dev` nodes in `proto` and in `base.postinst`; `BASE_VER` raised if the base changed.
- [ ] `CHANGELOG.md` line; manual chapter or `Manual:` line; this guide if an interface changed.
- [ ] `PORTING.md` updated with measurements where the task asks for them.
