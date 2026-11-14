# Design documents

Documents written by the review side of the project for the implementing agent.

- `galtzagorriak-spec.md`: phased implementation spec (Revision 2, 20 phases). Each PR should name the phase it implements.
- `modernization-security-report.md`: security and modernization review of main @ 62ea5c1, with Cortex-M4/STM32WB55 notes.

Workflow: implement on a separate branch, open a pull request against `main`, and name the spec phase in the PR description. PRs are reviewed for correctness, security, RAM impact and spec conformance, then squash-merged.
Spec updates also arrive here by pull request.
