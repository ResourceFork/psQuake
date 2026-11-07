#!/usr/bin/env bash
#
# Host test for tools/psproc-drift.sh. Builds a throwaway "psDoom" repo and a
# throwaway "psQuake" repo under WORK (a directory in the build tree), then
# checks the report, --apply and --mark: pure copy, the psdoom_ -> psproc_
# rename filter, a 3-way merge that keeps a local adaptation, conflicts,
# unmapped and ignored upstream files, UPSTREAM held back until the work is
# done, refusal of a dirty tree, a non-git root and a non-descendant revision.
#
# Usage: psproc_drift_test.sh <path to psproc-drift.sh> <work dir>
#
set -euo pipefail

tool=${1:?usage: psproc_drift_test.sh TOOL WORKDIR}
work=${2:?usage: psproc_drift_test.sh TOOL WORKDIR}

# Guard the directory this test deletes and recreates: absolute, not /, and
# named for this test.
case "$work" in
    /*/psproc-drift-test) ;;
    *) echo "refusing work dir: $work" >&2; exit 2 ;;
esac
rm -rf -- "$work"
mkdir -p "$work"

fails=0
check() {   # check DESCRIPTION COMMAND...
    local what=$1; shift
    if "$@"; then echo "ok   $what"; else echo "FAIL $what"; fails=$((fails + 1)); fi
}

g() {   # git with a fixed identity and no user hooks or signing
    git -c user.name=test -c user.email=test@example.invalid \
        -c commit.gpgsign=false -c core.hooksPath=/dev/null "$@"
}

commit_up() { g -C "$up" add -A; g -C "$up" commit -q -m "$1"; git -C "$up" rev-parse HEAD; }

up="$work/psDoom"
qk="$work/psQuake"
g init -q "$up"
g init -q "$qk"
printf 'before the import\n' > "$up/HISTORY"
pre=$(commit_up pre)     # an ancestor of the base: --apply must refuse it

# Every mapped file, five lines each, in both repos. The psQuake copy of a C
# file goes through the same rename the tool applies (psdoom_ -> psproc_).
while IFS=$'\t' read -r src dst; do
    mkdir -p "$up/$(dirname "$src")" "$qk/$(dirname "$dst")"
    printf 'line1 %s\nline2\nline3\nline4\nline5\n' "$src" > "$up/$src"
    case "$src" in
        *.c|*.h) sed 's/psdoom_/psproc_/g' "$up/$src" > "$qk/$dst" ;;
        *)       cp "$up/$src" "$qk/$dst" ;;
    esac
done < <("$tool" --print-map)
base=$(commit_up base)

# psQuake: an adaptation on line 1 of proc_select.c.
printf '%s\n' "$base" > "$qk/psproc/UPSTREAM"
sed -i.bak '1s/.*/line1 ADAPTED/' "$qk/psproc/src/proc_select.c"
rm -f "$qk/psproc/src/proc_select.c.bak"
g -C "$qk" add -A
g -C "$qk" commit -q -m import

run() { "$tool" --root "$qk" --psdoom "$up" "$@"; }
upstream_is() { test "$(cat "$qk/psproc/UPSTREAM")" = "$1"; }

out=$(run) && rc=0 || rc=$?
check "report at base exits 0" test "$rc" -eq 0
check "report marks the adaptation" grep -q 'psproc/src/proc_select.c *adapted' <<<"$out"
check "rename filter: renamed file compares same" grep -q 'psproc/src/psproc_options.c *same' <<<"$out"

# A mapped file missing locally is pending.
mv "$qk/psproc/src/proc_fake.c" "$work/proc_fake.c.saved"
out=$(run) && rc=0 || rc=$?
check "report with a missing file exits 1" test "$rc" -eq 1
check "report marks it MISSING" grep -q 'psproc/src/proc_fake.c *MISSING' <<<"$out"
mv "$work/proc_fake.c.saved" "$qk/psproc/src/proc_fake.c"

# Upstream fix: proc_backend.c (unadapted: pure copy), proc_select.c line 5
# (adapted on line 1: merge), psdoom_options.c gains psdoom_* code (renamed on
# the way in), plus an unwatched game file and an ignored test.
sed -i.bak '3s/.*/line3 UPSTREAM/' "$up/src/platform/proc_backend.c"
sed -i.bak '5s/.*/line5 UPSTREAM/' "$up/src/psdoom/proc_select.c"
printf 'int psdoom_guard(void) { return psdoom_kill_policy; }\n' >> "$up/src/psdoom/psdoom_options.c"
rm -f "$up"/src/platform/*.bak "$up"/src/psdoom/*.bak
printf 'game\n' > "$up/src/psdoom/psdoom.c"
printf 'doom only\n' > "$up/tests/window_state_test.c"
fix=$(commit_up fix)

out=$(run --commit "$fix") && rc=0 || rc=$?
check "report with upstream change exits 1" test "$rc" -eq 1
check "report lists the changed file" grep -q 'proc_backend.c *same *CHANGED' <<<"$out"
check "unwatched game file not listed" test -z "$(grep 'psdoom.c$' <<<"$out" || true)"
check "ignored test not listed" test -z "$(grep 'window_state_test' <<<"$out" || true)"

# Refusals: non-descendant revision, dirty psproc/, non-git root.
out=$(run --apply "$base~0" 2>&1) && rc=0 || rc=$?
check "apply at the base is a no-op" test "$rc" -eq 0
out=$(run --apply "$pre" 2>&1) && rc=0 || rc=$?
check "apply refuses a downgrade (non-descendant revision)" test "$rc" -eq 2
check "downgrade left UPSTREAM alone" upstream_is "$base"
out=$(run --mark "$pre" 2>&1) && rc=0 || rc=$?
check "mark refuses a downgrade" test "$rc" -eq 2
out=$(run --commit "$pre") && rc=0 || rc=$?
check "report warns about a non-descendant revision" grep -q 'does not descend' <<<"$out"

printf 'x\n' >> "$qk/psproc/src/proc_fake.c"
out=$(run --apply "$fix" 2>&1) && rc=0 || rc=$?
check "apply refuses a dirty psproc/" test "$rc" -eq 2
g -C "$qk" checkout -q -- psproc/src/proc_fake.c

nogit="$work/nogit"
mkdir -p "$nogit"
cp -R "$qk/psproc" "$nogit/psproc"
out=$("$tool" --root "$nogit" --psdoom "$up" --apply "$fix" 2>&1) && rc=0 || rc=$?
check "apply refuses a root that is not a git work tree" test "$rc" -eq 2
check "non-git copy untouched" cmp -s "$qk/psproc/src/proc_backend.c" "$nogit/psproc/src/proc_backend.c"

# Clean apply.
out=$(run --apply "$fix") && rc=0 || rc=$?
check "clean apply exits 0" test "$rc" -eq 0
check "apply copied the unadapted file" cmp -s "$up/src/platform/proc_backend.c" "$qk/psproc/src/proc_backend.c"
check "merge kept the adaptation" grep -q '^line1 ADAPTED$' "$qk/psproc/src/proc_select.c"
check "merge took the upstream change" grep -q '^line5 UPSTREAM$' "$qk/psproc/src/proc_select.c"
check "renamed identifiers on the way in" grep -q '^int psproc_guard(void) { return psproc_kill_policy; }$' "$qk/psproc/src/psproc_options.c"
check "no psdoom_ identifier imported" test -z "$(grep -l 'psdoom_' "$qk/psproc/src/psproc_options.c" || true)"
check "UPSTREAM moved to the fix" upstream_is "$fix"
check "nothing was committed" test "$(git -C "$qk" rev-list --count HEAD)" = 1
check "scratch dir removed" test -z "$(ls -d "$qk"/build-psproc-drift.* 2>/dev/null || true)"
check "psDoom tree untouched" test -z "$(git -C "$up" status --porcelain)"
g -C "$qk" add -A
g -C "$qk" commit -q -m reimport

# An unmapped watched file holds UPSTREAM back until it is dealt with.
printf 'new\n' > "$up/tests/new_test.c"
newf=$(commit_up newfile)
out=$(run --apply "$newf") && rc=0 || rc=$?
check "apply with an unmapped file exits 1" test "$rc" -eq 1
check "apply lists the unmapped file" grep -q 'tests/new_test.c' <<<"$out"
check "UPSTREAM held back" upstream_is "$fix"
out=$(run --mark "$newf") && rc=0 || rc=$?
check "mark refuses while the file is unmapped" test "$rc" -eq 1
check "UPSTREAM still held back" upstream_is "$fix"
g -C "$up" rm -q tests/new_test.c
gone=$(commit_up dropnew)
out=$(run --mark "$gone") && rc=0 || rc=$?
check "mark succeeds once nothing is left" test "$rc" -eq 0
check "UPSTREAM marked" upstream_is "$gone"
g -C "$qk" add -A
g -C "$qk" commit -q -m mark

# A psDoom change to the adapted line conflicts, is left marked, and blocks
# both UPSTREAM and --mark.
sed -i.bak '1s/.*/line1 UPSTREAM/' "$up/src/psdoom/proc_select.c"
rm -f "$up"/src/psdoom/*.bak
clash=$(commit_up clash)
out=$(run --apply "$clash") && rc=0 || rc=$?
check "conflicting apply exits 1" test "$rc" -eq 1
check "conflict reported" grep -q 'CONFLICT psproc/src/proc_select.c' <<<"$out"
check "conflict markers left" grep -q '^<<<<<<<' "$qk/psproc/src/proc_select.c"
check "UPSTREAM not moved on conflict" upstream_is "$gone"
out=$(run --mark "$clash") && rc=0 || rc=$?
check "mark refuses with conflict markers" test "$rc" -eq 1
out=$(run) && rc=0 || rc=$?
check "report flags the conflict markers" grep -q 'Conflict markers left in' <<<"$out"

rm -rf -- "$work"
echo "$fails failed"
[ "$fails" -eq 0 ]
