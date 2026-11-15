#!/bin/bash
# The documentation rules of docs/spec/next-tasks.md ("Documentation
# follows behaviour"), checked on every PR:
#   1. A PR that changes code (anything but Markdown, docs/ and man/) adds
#      a line under "Unreleased" in CHANGELOG.md.     Skip: label no-changelog
#   2. A PR that changes something a user sees or does (commands, apps, the
#      GUI, /etc, the build targets and images) updates docs/manual/, or
#      has a line "Manual: <why no change is needed>" in its description.
#                                                     Skip: label no-manual
# Inputs (environment): FILES (changed paths, one per line), BODY, LABELS.
set -u
fail=0
has_label() { grep -qx "$1" <<<"$LABELS"; }
changed() { grep -Eq "$1" <<<"$FILES"; }

code=$(grep -Ev '(\.md$|^docs/|^man/|^README$|^LICENSE$)' <<<"$FILES" || true)
if [ -n "$code" ] && ! changed '^CHANGELOG\.md$'; then
	if has_label no-changelog; then
		echo "CHANGELOG: skipped (label no-changelog)"
	else
		echo "::error title=CHANGELOG.md::This PR changes code but not CHANGELOG.md. Add one line under 'Unreleased' (or label the PR no-changelog). Code files: $(head -5 <<<"$code" | xargs)"
		fail=1
	fi
else
	echo "CHANGELOG: ok"
fi

visible='^(flipper/(cmds|apps|gui|rootfs|srv)/|src/commands/|flipper/(Makefile|minix\.mk)$|flipper/tools/(mk|gzbuild))'
if changed "$visible" && ! changed '^docs/manual/'; then
	if has_label no-manual; then
		echo "Manual: skipped (label no-manual)"
	elif grep -Eiq '^[[:space:]>*_-]*manual:' <<<"$BODY"; then
		echo "Manual: the description explains why no chapter changes"
	else
		echo "::error title=docs/manual::This PR changes something users see ($(grep -E "$visible" <<<"$FILES" | head -3 | xargs)) but not docs/manual/. Update the matching chapter, or add a line 'Manual: <why no change is needed>' to the PR description (or label no-manual)."
		fail=1
	fi
else
	echo "Manual: ok"
fi
exit $fail
