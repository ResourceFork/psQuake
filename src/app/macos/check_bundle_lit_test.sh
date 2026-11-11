#!/bin/bash
#
# psQuake -- ctest psq_bundle_check_arena_lit: check_bundle.sh's arena map
# check passes on the built arenas with their coloured lightmaps, and fails
# on each way a .lit can be wrong: missing, cut short, not a QLIT, another
# version, or another arena's.
#
#   check_bundle_lit_test.sh <check_bundle.sh> <build/arena> <work dir>
#
# <work dir> is a scratch folder in the build tree, replaced on each run.
#
set -euo pipefail
[ $# -eq 3 ] || { echo "usage: check_bundle_lit_test.sh <check_bundle.sh> <arena dir> <work dir>" >&2; exit 2; }
check=$1
arena=$2
work=$3
for p in "$check" "$arena" "$work"; do
    case $p in /*) ;; *) echo "not an absolute path: $p" >&2; exit 2 ;; esac
    [ "$p" != / ] || { echo "refusing /" >&2; exit 2; }
done
[ -f "$check" ] || { echo "no $check" >&2; exit 2; }
[ -s "$arena/psqarena.bsp" ] || { echo "no arenas in $arena" >&2; exit 2; }
[ "$work" = "$arena/lit-probe" ] || { echo "the work dir must be $arena/lit-probe: $work" >&2; exit 2; }

failed=0
expect() { # <pass|fail> <what>
    local want=$1 what=$2 got=pass
    bash "$check" --probe-maps "$work" > "$work.out" 2>&1 || got=fail
    if [ "$got" = "$want" ]; then
        echo "PASS: the map check $([ "$want" = pass ] && echo accepts || echo rejects) $what"
    else
        echo "FAIL: the map check should ${want} $what, and ${got}ed:"
        cat "$work.out"
        failed=1
    fi
}
fresh() {
    rm -rf "$work"
    mkdir -p "$work"
    for m in psqtemple psqruins psqforge psqarena psqterraces psqtowers psqtesthall; do
        cp "$arena/$m.bsp" "$arena/$m.lit" "$work/"
    done
}

fresh
expect pass "the built arenas"
rm "$work/psqtowers.lit"
expect fail "an arena with no .lit"
fresh
head -c 1000 "$arena/psqterraces.lit" > "$work/psqterraces.lit"
expect fail "a .lit cut short"
fresh
{ printf 'XLIT'; tail -c +5 "$arena/psqarena.lit"; } > "$work/psqarena.lit"
expect fail "a .lit that is not a QLIT"
fresh
{ printf 'QLIT\002\000\000\000'; tail -c +9 "$arena/psqarena.lit"; } > "$work/psqarena.lit"
expect fail "a .lit of another version"
fresh
cp "$arena/psqarena.lit" "$work/psqtesthall.lit"
expect fail "another arena's .lit"
rm -rf "$work" "$work.out"
exit $failed
