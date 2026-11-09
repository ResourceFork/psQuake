#!/bin/bash
#
# psQuake -- check an assembled psQuake.app before it is shipped. Read-only.
#
#   check_bundle.sh [--release] <psQuake.app>
#
# Always: the engine is arm64 only, links only system libraries and the
# bundled ones by @executable_path (no LC_RPATH), is ad-hoc signed with no
# entitlements and the signature verifies; progs.dat and every licence text
# NOTICES.txt names are present; no test-only console command is compiled
# in. With --release also: LibreQuake's paks, its licence documents and the
# arena map are bundled.
#
# Prints one line per check and exits 1 if any fails.
#
set -euo pipefail

release=0
if [ "${1:-}" = --release ]; then
    release=1
    shift
fi
[ $# -eq 1 ] || { echo "usage: check_bundle.sh [--release] <psQuake.app>" >&2; exit 2; }
app=$1
case $app in /*.app|/*.app/) ;; *) echo "check_bundle.sh: not an absolute .app path: $app" >&2; exit 2 ;; esac
app=${app%/}

failed=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; failed=1; }
check() { # <description> <command...>
    local what=$1
    shift
    if "$@" >/dev/null 2>&1; then pass "$what"; else fail "$what"; fi
}

bin="$app/Contents/MacOS/psQuake"
res="$app/Contents/Resources"
fw="$app/Contents/Frameworks"
lic="$res/Licenses"

[ -f "$bin" ] || { fail "engine at Contents/MacOS/psQuake"; exit 1; }
pass "engine at Contents/MacOS/psQuake"

archs=$(lipo -archs "$bin")
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
if otool -l "$bin" | grep -q LC_RPATH; then fail "engine has no LC_RPATH"; else pass "engine has no LC_RPATH"; fi

check "signature verifies (strict, deep)" codesign --verify --strict --deep "$app"
sig=$(codesign -dv "$app" 2>&1 || true)
case $sig in *"Signature=adhoc"*) pass "ad-hoc signature" ;; *) fail "ad-hoc signature" ;; esac
ents=$(codesign -d --entitlements - "$app" 2>/dev/null || true)
case $ents in *"<key>"*) fail "no entitlements" ;; *) pass "no entitlements" ;; esac

# The test drivers' console commands set the kill mode without the warning
# (psq_test_killmode); they must not be in the app.
if strings -a "$bin" | grep -q -e '^psq_test_' -e '^psq_render_'; then
    fail "no test-only console commands in the engine"
else
    pass "no test-only console commands in the engine"
fi

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
check "SOURCE.txt names a commit" grep -Eq '^commit: [0-9a-f]{40}' "$lic/SOURCE.txt"
# Every licence file NOTICES.txt names is in the folder.
for f in $(grep -oE '[A-Za-z0-9.-]+\.txt' "$lic/NOTICES.txt" | sort -u); do
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
    check "the arena map, maps/psqarena.bsp" test -s "$res/id1/maps/psqarena.bsp"
fi

if [ $failed -ne 0 ]; then
    echo "check_bundle.sh: $app FAILED"
    exit 1
fi
echo "check_bundle.sh: $app OK"
