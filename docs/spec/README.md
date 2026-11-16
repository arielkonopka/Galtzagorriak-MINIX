# Design documents

Documents written by the review side of the project for the implementing agent.

- `galtzagorriak-spec.md`: phased implementation spec (Revision 2, 20 phases). Each PR should name the phase it implements.
- `modernization-security-report.md`: security and modernization review of main @ 62ea5c1, with Cortex-M4/STM32WB55 notes.
- `power-plan.md`: power budget analysis (2026-11-15) and the PR plan with on-device tests for next-tasks T14.
- `strategy.md`: work strategy (2026-11-15): critical path, lanes (local agent / CI worker / review), milestones and releases, measured pace, forecast, risks.
- `../manual/`: the user and system manual (start: `install.md`). PRs that change user-visible behaviour update it (standing rule in `next-tasks.md`).
- `../devguide/`: the Galtzagorriak MINIX Development Guide (writing programs for the system; working on the system itself). PRs that change an interface it describes update it.
- `../../CHANGELOG.md` (repo root): release notes; versioning rules in spec section J. Feature PRs add a line under `Unreleased`.

Workflow: implement on a separate branch, open a pull request against `main`, and name the spec phase in the PR description. PRs are reviewed for correctness, security, RAM impact and spec conformance, then squash-merged. The GitHub Actions check `build` (Flipper images, `ramcheck`, QEMU `M2 USER PASS`) must be green before a merge.
Spec updates also arrive here by pull request.
