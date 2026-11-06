#!/usr/bin/env bash
#
# Host test for tools/psproc-drift.sh. Builds a throwaway "psDoom" repo and a
# throwaway "psQuake" repo under WORK (a directory in the build tree), then
# checks the report and --apply: pure copy, 3-way merge that keeps a local
# adaptation, conflict, unmapped upstream file, and refusal on a dirty tree.
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

up="$work/psDoom"
qk="$work/psQuake"
g init -q "$up"
g init -q "$qk"

# Every mapped file, one line each, in both repos.
while IFS=$'\t' read -r src dst; do
    mkdir -p "$up/$(dirname "$src")" "$qk/$(dirname "$dst")"
    printf 'line1 %s\nline2\nline3\nline4\nline5\n' "$src" > "$up/$src"
    cp "$up/$src" "$qk/$dst"
done < <("$tool" --print-map)
g -C "$up" add -A
g -C "$up" commit -q -m base
base=$(git -C "$up" rev-parse HEAD)

# psQuake: an adaptation on line 1 of proc_select.c, then commit.
mkdir -p "$qk/tools"
printf '%s\n' "$base" > "$qk/psproc/UPSTREAM"
sed -i.bak '1s/.*/line1 ADAPTED/' "$qk/psproc/src/proc_select.c"
rm -f "$qk/psproc/src/proc_select.c.bak"
g -C "$qk" add -A
g -C "$qk" commit -q -m import

run() { "$tool" --root "$qk" --psdoom "$up" "$@"; }

out=$(run) && rc=0 || rc=$?
check "report at base exits 0" test "$rc" -eq 0
check "report marks the adaptation" grep -q 'psproc/src/proc_select.c *adapted' <<<"$out"
check "report marks an untouched file same" grep -q 'psproc/src/proc_backend.c *same' <<<"$out"

# Upstream: change proc_backend.c (no local edit), proc_select.c line 5 (local
# edit is on line 1: must merge), and add an unmapped test.
sed -i.bak '3s/.*/line3 UPSTREAM/' "$up/src/platform/proc_backend.c"
sed -i.bak '5s/.*/line5 UPSTREAM/' "$up/src/psdoom/proc_select.c"
rm -f "$up"/src/platform/*.bak "$up"/src/psdoom/*.bak
printf 'new\n' > "$up/tests/new_test.c"
g -C "$up" add -A
g -C "$up" commit -q -m fix
fix=$(git -C "$up" rev-parse HEAD)

out=$(run --commit "$fix") && rc=0 || rc=$?
check "report with upstream change exits 1" test "$rc" -eq 1
check "report lists the changed file" grep -q 'proc_backend.c *same *CHANGED' <<<"$out"
check "report lists the unmapped file" grep -q 'tests/new_test.c' <<<"$out"

# Dirty psproc/ blocks --apply.
printf 'x\n' >> "$qk/psproc/src/proc_fake.c"
out=$(run --apply "$fix" 2>&1) && rc=0 || rc=$?
check "apply refuses a dirty psproc/" test "$rc" -eq 2
g -C "$qk" checkout -q -- psproc/src/proc_fake.c

out=$(run --apply "$fix") && rc=0 || rc=$?
check "apply exits 1 (unmapped file to look at)" test "$rc" -eq 1
check "apply copied the untouched file" cmp -s "$up/src/platform/proc_backend.c" "$qk/psproc/src/proc_backend.c"
check "merge kept the adaptation" grep -q '^line1 ADAPTED$' "$qk/psproc/src/proc_select.c"
check "merge took the upstream change" grep -q '^line5 UPSTREAM$' "$qk/psproc/src/proc_select.c"
check "UPSTREAM moved to the fix" test "$(cat "$qk/psproc/UPSTREAM")" = "$fix"
check "nothing was committed" test "$(git -C "$qk" rev-list --count HEAD)" = 1
check "scratch dir removed" test -z "$(ls -d "$qk"/build-psproc-drift.* 2>/dev/null)"
check "psDoom tree untouched" test -z "$(git -C "$up" status --porcelain)"
g -C "$qk" add -A
g -C "$qk" commit -q -m reimport

# A psDoom change to the adapted line conflicts and is left marked.
sed -i.bak '1s/.*/line1 UPSTREAM/' "$up/src/psdoom/proc_select.c"
rm -f "$up"/src/psdoom/*.bak
g -C "$up" add -A
g -C "$up" commit -q -m clash
clash=$(git -C "$up" rev-parse HEAD)
out=$(run --apply "$clash") && rc=0 || rc=$?
check "conflicting apply exits 1" test "$rc" -eq 1
check "conflict reported" grep -q 'CONFLICT psproc/src/proc_select.c' <<<"$out"
check "conflict markers left" grep -q '^<<<<<<<' "$qk/psproc/src/proc_select.c"

rm -rf -- "$work"
echo "$fails failed"
[ "$fails" -eq 0 ]
