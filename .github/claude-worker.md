# Instructions for the GitHub Actions worker

You implement one task (the issue or comment that started you) for Galtzagorriak
MINIX, a MINIX 2.0.0 port to the Flipper Zero (STM32WB55, 256 KB RAM of which
MINIX uses 192 KB, code runs from flash). The owner's local agent works on other
tasks at the same time; the review side reads your PR and squash-merges it.

## Before you write code
1. Read `docs/spec/README.md`, the standing rules and "CI gates" at the top of
   `docs/spec/next-tasks.md`, and the spec sections the task names in
   `docs/spec/galtzagorriak-spec.md` (search it; do not read all of it).
2. If the issue names a task (T…), read that task in `next-tasks.md`. If it is
   marked DONE, or the issue is too vague to implement, say so in your progress
   comment and stop without code changes.

## Rules (the owner's; reviews reject PRs that break them)
- **Responsiveness first**: budgets L1–L10 (spec rule 0.8). Never add busy
  waits or long work in interrupts or in the kernel/MM/FS paths a key press uses.
- **RAM is tight**: `make BOARD=flipper ramcheck` must pass. Do not raise a line in
  `flipper/ram-budget.txt` unless the task allows it; then give the reason in the
  same commit. Prefer zero-RAM solutions (const tables, flash, hardware features).
- **Few flash writes**: none at run time if avoidable; the M2 erase count must stay
  within `flipper/flash-budget.txt`.
- **Do not rewrite working MINIX core** unless the task needs it for RAM,
  responsiveness or security.
- **No threads, no TLS** on the device.
- Licence: GPLv3 is fine; stock Flipper firmware code may be reused.
- Never touch `.github/` (workflows, these instructions), secrets or tags.

## Save your work as you go
A run can stop at its turn or time limit. After **each finished step** of your
checklist (it builds, or at least compiles), commit and push it with the git
scripts you were given (`git-add.sh`, `git-commit.sh -m "WIP <task>: <step>"`,
`git-push.sh origin HEAD`), and keep the checklist in your comment current.
Then a cut-off run leaves a branch that the next run continues: the owner or
the review side opens a draft PR from it and comments `@claude continue`.
When you are started on a PR, read its commits and your earlier comment first
and continue from the first unchecked step; do not redo finished steps.
Plan big tasks so that each step is a few files, not the whole task at once.

## Check your work (all in this runner; the toolchain is installed)
```
cd flipper
make BOARD=flipper -j"$(nproc)" minix card base-flash base pkgs
make BOARD=flipper ramcheck
make -j"$(nproc)" INIT=m2 minix disk && make INIT=m2 minix-run | tee m2.log
grep 'M2 USER PASS' m2.log && python3 -I tools/flashcheck.py flash-budget.txt m2 m2.log
```
If the change touches installs or the flash region, also run
`make -j"$(nproc)" minix disk && python3 -I tools/powercut.py -j"$(nproc)"`.
You have no Flipper. Anything that needs the device (L1–L10 with `lat`, radio,
USB, SD timing) goes into the PR description under "Needs on-device test", with
the exact steps, for the owner's local agent; after the merge they become
checkboxes in the open issue labelled `device-checks` (#109).
A change on the Flipper path that only the device can accept (a new hardware
path, a driver) merges **off by default** (or with no change on the Flipper
path) until the local agent has run those checks. Never: option bytes,
CPU2/FUS, enabling a hardware path by default ("Autonomy" in
`docs/spec/next-tasks.md`).

## Finish
- Add one line under `Unreleased` in `CHANGELOG.md`. Update the matching chapter of
  `docs/manual/` if users see the change, or explain in the PR text
  ("Manual: …") why not.
- Commit with a message naming the task (e.g. "T15: …"). Push your branch.
- In your final progress comment give: what changed, RAM (ramcheck numbers),
  flash writes, responsiveness, what was tested here, "Needs on-device test", and
  the PR link. The PR body should start with "Closes #<issue>" and have the same
  sections.
- Write in the language of the issue (Polish or English).
