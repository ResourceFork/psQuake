#!/bin/bash
#
# psQuake -- check an assembled psQuake.app before it is shipped. Read-only.
#
#   check_bundle.sh [--release] [--clean-source] <psQuake.app>
#
# Always: the engine is arm64 only, links only system libraries and the
# bundled ones by @executable_path (no LC_RPATH), is ad-hoc signed with no
# entitlements and the signature verifies; progs.dat and every licence text
# NOTICES.txt names are present; no test-only console command is compiled
# in. With --release also: LibreQuake's paks, its licence documents and the
# arena maps (psqtemple, psqarena, psqterraces, psqtowers, psqtesthall) are bundled,
# each with its coloured lightmap: a .lit beside the .bsp, QLIT version 1,
# three bytes for each of the map's lightmap bytes (else the engine drops it
# and the torchlight renders white).
#
#   check_bundle.sh --probe-maps <maps dir>   only the arena map checks, on
#                                             a folder (ctest
#                                             psq_bundle_check_arena_lit)
# With --clean-source (tools/package-release.sh):
# SOURCE.txt names a commit with no uncommitted or untracked changes.
#
# Prints one line per check and exits 1 if any fails.
#
set -euo pipefail

pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; failed=1; }

# Never 'producer | grep -q' here: under pipefail, grep stopping early kills
# the producer with SIGPIPE and the pipeline reads as "no match". Each
# producer's output is read whole first.
check_no_test_commands() { # <mach-o file>
    local s
    s=$(strings -a "$1")
    if grep -q -e '^psq_test_' -e '^psq_render_' <<< "$s"; then
        fail "no test-only console commands in ${1##*/}"
    else
        pass "no test-only console commands in ${1##*/}"
    fi
}

check() { # <description> <command...>
    local what=$1
    shift
    if "$@" >/dev/null 2>&1; then pass "$what"; else fail "$what"; fi
}
# <maps dir> <name>: <name>.lit is a QLIT version 1 the size the engine
# wants for <name>.bsp: 8 + 3 x its lighting lump (lump 8: its length is
# the header's int at byte 72).
lit_matches() {
    local bsp="$1/$2.bsp" lit="$1/$2.lit" n
    [ -s "$bsp" ] && [ -s "$lit" ] || return 1
    [ "$(head -c 4 "$lit")" = QLIT ] || return 1
    [ "$(od -An -t d4 -j 4 -N 4 "$lit" | tr -d ' ')" = 1 ] || return 1
    n=$(od -An -t d4 -j 72 -N 4 "$bsp" | tr -d ' ')
    [ -n "$n" ] && [ "$(wc -c < "$lit" | tr -d ' ')" = "$((8 + 3 * n))" ]
}
check_arena_maps() { # <maps dir>
    local m
    for m in psqtemple psqruins psqarena psqterraces psqtowers psqtesthall; do
        check "the arena map, maps/$m.bsp" test -s "$1/$m.bsp"
        check "its coloured light, maps/$m.lit, matching it" lit_matches "$1" "$m"
    done
}
failed=0
# --probe-test-commands <file>: only that check, on any binary (ctest
# psq_bundle_check_catches_test_commands runs it on a test driver).
if [ "${1:-}" = --probe-test-commands ] && [ $# -eq 2 ]; then
    check_no_test_commands "$2"
    exit $failed
fi
if [ "${1:-}" = --probe-maps ] && [ $# -eq 2 ]; then
    check_arena_maps "$2"
    exit $failed
fi

release=0
clean_source=0
while [ $# -gt 1 ]; do
    case $1 in
        --release) release=1; shift ;;
        --clean-source) clean_source=1; shift ;;
        *) break ;;
    esac
done
[ $# -eq 1 ] || { echo "usage: check_bundle.sh [--release] [--clean-source] <psQuake.app>" >&2; exit 2; }
app=$1
case $app in /*.app|/*.app/) ;; *) echo "check_bundle.sh: not an absolute .app path: $app" >&2; exit 2 ;; esac
app=${app%/}

bin="$app/Contents/MacOS/psQuake"
res="$app/Contents/Resources"
fw="$app/Contents/Frameworks"
lic="$res/Licenses"

[ -f "$bin" ] || { fail "engine at Contents/MacOS/psQuake"; exit 1; }
pass "engine at Contents/MacOS/psQuake"

archs=$(lipo -archs "$bin" 2>/dev/null || echo unreadable)
[ "$archs" = arm64 ] && pass "engine is arm64 only" || fail "engine is arm64 only (has: $archs)"

for f in "$fw/SDL2.framework/Versions/A/SDL2" "$fw/libogg.dylib" "$fw/libvorbis.dylib" \
         "$fw/libvorbisfile.dylib"; do
    a=$(lipo -archs "$f" 2>/dev/null || echo missing)
    [ "$a" = arm64 ] && pass "${f#"$app/"} is arm64 only" || fail "${f#"$app/"} is arm64 only (has: $a)"
done

bad_libs=$(otool -L "$bin" | tail -n +2 | awk '{print $1}' |
    grep -v -e '^/usr/lib/' -e '^/System/Library/' -e '^@executable_path/../Frameworks/' || true)
[ -z "$bad_libs" ] && pass "engine links only system and bundled libraries" ||
    fail "engine links only system and bundled libraries (also: $bad_libs)"
loadcmds=$(otool -l "$bin")
if grep -q LC_RPATH <<< "$loadcmds"; then fail "engine has no LC_RPATH"; else pass "engine has no LC_RPATH"; fi

check "signature verifies (strict, deep)" codesign --verify --strict --deep "$app"
sig=$(codesign -dv "$app" 2>&1 || true)
case $sig in *"Signature=adhoc"*) pass "ad-hoc signature" ;; *) fail "ad-hoc signature" ;; esac
# --xml: current codesign prints entitlements as [Key] lines otherwise.
ents=$(codesign -d --entitlements - --xml "$app" 2>/dev/null || true)
case $ents in *"<key>"*) fail "no entitlements" ;; *) pass "no entitlements" ;; esac

# The test drivers' console commands set the kill mode without the warning
# (psq_test_killmode); they must not be in the app.
check_no_test_commands "$bin"

check "game code at Resources/psquake/progs.dat" test -s "$res/psquake/progs.dat"
for f in NOTICES.txt SOURCE.txt psQuake-MIT.txt QuakeSpasm-GPL-2.0.txt \
         LibreQuake-QuakeC-GPL-2.0.txt LibreQuake-LICENCE-INFO.txt fteqcc-GPL-2.0.txt \
         SDL2-zlib.txt libogg-1.3.2-COPYING.txt libvorbis-1.3.5-COPYING.txt; do
    check "licence file Licenses/$f" test -s "$lic/$f"
done
check "GPL-2 text is GPL version 2" grep -q "Version 2, June 1991" "$lic/QuakeSpasm-GPL-2.0.txt"
check "LibreQuake QuakeC text is GPL version 2" grep -q "Version 2, June 1991" "$lic/LibreQuake-QuakeC-GPL-2.0.txt"
check "fteqcc text is GPL version 2" grep -q "Version 2, June 1991" "$lic/fteqcc-GPL-2.0.txt"
check "SDL text is the zlib licence" grep -qi "This software is provided 'as-is'" "$lic/SDL2-zlib.txt"
if [ $clean_source -eq 1 ]; then
    # A shipped zip names the exact source: a clean commit (assemble_bundle.sh
    # marks a tree with any change, untracked files included).
    check "SOURCE.txt names a clean commit" grep -Eqx 'commit: [0-9a-f]{40}' "$lic/SOURCE.txt"
else
    check "SOURCE.txt names the source" grep -Eq '^commit: ' "$lic/SOURCE.txt"
fi
# Every licence file NOTICES.txt names is in the folder.
# (LibreQuake's data licence only when its data is bundled.)
for f in $(grep -oE '[A-Za-z0-9._-]+\.txt' "$lic/NOTICES.txt" | sort -u); do
    [ "$f" = LibreQuake-data-BSD-3.txt ] && [ ! -d "$res/id1" ] && continue
    check "NOTICES.txt names $f, and it is bundled" test -s "$lic/$f"
done

if [ $release -eq 1 ]; then
    check "LibreQuake pak0.pak" test -s "$res/id1/pak0.pak"
    check "LibreQuake pak1.pak" test -s "$res/id1/pak1.pak"
    check "LibreQuake data licence (BSD-3)" grep -q "Redistribution and use in source and binary forms" \
        "$lic/LibreQuake-data-BSD-3.txt"
    for f in COPYING CREDITS README-IMPORTANT-LICENCE-INFO README.md MANIFEST.sha256; do
        check "LibreQuake/$f" test -s "$res/LibreQuake/$f"
    done
    check "every LibreQuake file matches the fetch manifest" \
        bash -c 'cd "$1" && shasum -a 256 -c -s LibreQuake/MANIFEST.sha256' _ "$res"
    check_arena_maps "$res/id1/maps"
fi

if [ $failed -ne 0 ]; then
    echo "check_bundle.sh: $app FAILED"
    exit 1
fi
echo "check_bundle.sh: $app OK"
