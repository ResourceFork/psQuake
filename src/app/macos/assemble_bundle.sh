#!/bin/bash
#
# psQuake -- finish psQuake.app after the engine links, and ad-hoc sign it.
#
#   assemble_bundle.sh <app> <archs> <SDL2.framework> <staged codecs dir>
#                      <repo root> <data dir> <arena bsp or -> [<arena bsp>...]
#
#   Contents/MacOS/psQuake            the engine (linked by CMake)
#   Contents/Frameworks/SDL2.framework  vendored SDL 2.0.22, thinned, no headers
#   Contents/Frameworks/lib{ogg,vorbis,vorbisfile}.dylib   staged codecs
#   Contents/Resources/psQuake.icns   placeholder icon (QuakeSpasm's)
#   Contents/Resources/Licenses/      NOTICES.txt, SOURCE.txt and every licence text:
#                                     GPL-2 (engine, LibreQuake QuakeC, fteqcc), MIT
#                                     (psQuake), zlib (SDL), BSD-3 (Xiph, LibreQuake data)
#   Contents/Resources/id1/           LibreQuake game data, when fetched
#   Contents/Resources/LibreQuake/    LibreQuake's licence and credits
#   Contents/Resources/id1/maps/<arena>.bsp    the validated arenas, with LibreQuake:
#                                     psqtemple (New Game's), psqruins, psqforge, the banked psqarena,
#                                     psqterraces and psqtowers, and psqtesthall (tools/arena.cmake), each with
#   Contents/Resources/id1/maps/<arena>.lit    its coloured lightmap (the warm torchlight)
#
# <data dir> is data/librequake as tools/fetch-librequake.sh leaves it. When
# it holds no LibreQuake, any data an earlier build bundled is removed and a
# warning is printed: the app then needs registered Quake in the user root.
#
# Local ad-hoc signing only. Developer ID signing and notarization are a
# later phase.
#
set -euo pipefail

die() { echo "assemble_bundle.sh: $*" >&2; exit 1; }

[ $# -ge 7 ] || die "usage: assemble_bundle.sh <app> <archs> <SDL2.framework> <codecs dir> <repo root> <data dir> <arena bsp or -> [<arena bsp>...]"
app=$1
archs=$2
sdl=$3
codecs=$4
repo=$5
data=$6
arena=$7
shift 7
arenas=()
arena_names="psqtemple.bsp psqruins.bsp psqforge.bsp psqarena.bsp psqterraces.bsp psqtowers.bsp psqtesthall.bsp"
if [ "$arena" != - ]; then
    arenas=("$arena" "$@")
    case $arena in /*/psqtemple.bsp) ;; *) die "the first arena is not an absolute psqtemple.bsp: $arena" ;; esac
    for a in "${arenas[@]}"; do
        case $a in
            /*/psqtemple.bsp|/*/psqruins.bsp|/*/psqforge.bsp|/*/psqarena.bsp|/*/psqterraces.bsp|/*/psqtowers.bsp|/*/psqtesthall.bsp) ;;
            *) die "not an absolute path to a known arena: $a" ;;
        esac
        [ -f "$a" ] || die "no arena at $a"
        [ -s "${a%.bsp}.lit" ] || die "no coloured lightmap ${a%.bsp}.lit beside $a"
    done
elif [ $# -ne 0 ]; then
    die "no first arena (-), yet more arenas given"
fi

for p in "$app" "$sdl" "$codecs" "$repo" "$data"; do
    [ -n "$p" ] || die "empty path"
    case $p in /*) ;; *) die "not absolute: $p" ;; esac
    [ "$p" != / ] || die "refusing /"
done
case $app in *.app) ;; *) die "not an .app: $app" ;; esac
[ -f "$app/Contents/MacOS/psQuake" ] || die "no engine in $app"
[ -f "$app/Contents/Info.plist" ] || die "no Info.plist in $app"
[ -f "$sdl/Versions/A/SDL2" ] || die "no SDL2.framework at $sdl"
[ -f "$codecs/libvorbisfile.dylib" ] || die "no staged codecs in $codecs"
[ -f "$repo/third_party/quakespasm/LICENSE.txt" ] || die "not the psQuake repo: $repo"

arch_args=()
IFS=',' read -r -a arch_list <<< "$archs"
for a in "${arch_list[@]}"; do
    case $a in arm64|x86_64) arch_args+=(--arch "$a") ;;
        *) die "unsupported architecture: $a" ;; esac
done

# Ad-hoc signing adds a 16-byte LC_CODE_SIGNATURE to any slice that has
# none. If the slice has less room than that between its load commands and
# its first section, the command would overwrite code (the vendored SDL2's
# x86_64 slice has 8 bytes). Refuse to sign such a file.
check_sign_room() { # <mach-o file>
    local f=$1 a end first
    for a in $(lipo -archs "$f"); do
        if otool -arch "$a" -l "$f" | grep -q LC_CODE_SIGNATURE; then
            continue
        fi
        end=$(otool -arch "$a" -h "$f" | tail -1 | awk '{print 32 + $7}')
        first=$(otool -arch "$a" -l "$f" |
            awk '/^Section/ {s=1} s && $1 == "offset" && $2 > 0 {print $2; exit}')
        [ -n "$end" ] && [ -n "$first" ] || die "cannot read load commands of $f ($a)"
        [ $((first - end)) -ge 16 ] ||
            die "$f ($a): only $((first - end)) bytes for a code signature; signing would corrupt it"
    done
}

contents="$app/Contents"
fw="$contents/Frameworks"
res="$contents/Resources"
mkdir -p "$fw" "$res"

# SDL2.framework: thinned copy without headers, re-signed.
rm -rf "$fw/SDL2.framework"
ditto "${arch_args[@]}" "$sdl" "$fw/SDL2.framework"
rm -rf "$fw/SDL2.framework/Versions/A/Headers" "$fw/SDL2.framework/Headers"
check_sign_room "$fw/SDL2.framework/Versions/A/SDL2"
codesign --force --sign - "$fw/SDL2.framework" 2>/dev/null

# Codecs (already thinned, re-pathed and signed by stage_codecs.sh).
for lib in libogg libvorbis libvorbisfile; do
    cp -f "$codecs/$lib.dylib" "$fw/$lib.dylib"
    check_sign_room "$fw/$lib.dylib"
    codesign --force --sign - "$fw/$lib.dylib" 2>/dev/null
done

# Icon and licences.
cp -f "$repo/third_party/quakespasm/MacOSX/QuakeSpasm.icns" "$res/psQuake.icns"
mkdir -p "$res/Licenses"
cp -f "$repo/third_party/quakespasm/LICENSE.txt" "$res/Licenses/QuakeSpasm-GPL-2.0.txt"
cp -f "$repo/LICENSE" "$res/Licenses/psQuake-MIT.txt"
cp -f "$repo/third_party/licenses/libogg-1.3.2-COPYING.txt" "$res/Licenses/"
cp -f "$repo/third_party/licenses/libvorbis-1.3.5-COPYING.txt" "$res/Licenses/"
cp -f "$sdl/Versions/A/Resources/License.txt" "$res/Licenses/SDL2-zlib.txt"
cp -f "$repo/third_party/NOTICES.txt" "$res/Licenses/NOTICES.txt"
cp -f "$repo/third_party/librequake-qc/docs/misc-docs/COPYING" \
    "$res/Licenses/LibreQuake-QuakeC-GPL-2.0.txt"
cp -f "$repo/third_party/librequake-qc/docs/README-IMPORTANT-LICENCE-INFO" \
    "$res/Licenses/LibreQuake-LICENCE-INFO.txt"
cp -f "$repo/third_party/fteqcc/LICENSE" "$res/Licenses/fteqcc-GPL-2.0.txt"
for f in strlcpy-strlcat-ISC miniz-MIT lodepng-zlib stb_image_write-MIT; do
    cp -f "$repo/third_party/licenses/$f.txt" "$res/Licenses/$f.txt"
done
# The source the app was built from (GPL-2: NOTICES.txt points here).
src_rev=unknown
if git -C "$repo" rev-parse --verify -q HEAD >/dev/null 2>&1; then
    src_rev=$(git -C "$repo" rev-parse HEAD)
    if [ -n "$(git -C "$repo" status --porcelain 2>/dev/null)" ]; then
        src_rev="$src_rev (with uncommitted or untracked changes)"
    fi
fi
printf 'psQuake source: https://github.com/ResourceFork/psQuake\ncommit: %s\n' \
    "$src_rev" > "$res/Licenses/SOURCE.txt"

# LibreQuake game data.
if [ -f "$data/id1/pak0.pak" ] && [ -f "$data/MANIFEST.sha256" ]; then
    if [ -f "$res/LibreQuake/MANIFEST.sha256" ] && [ -d "$res/id1" ] &&
       cmp -s "$data/MANIFEST.sha256" "$res/LibreQuake/MANIFEST.sha256"; then
        echo "psQuake: bundled LibreQuake is up to date"
    else
        echo "psQuake: copying LibreQuake into the bundle"
        rm -rf "$res/id1" "$res/LibreQuake"
        ditto "$data/id1" "$res/id1"
        mkdir -p "$res/LibreQuake"
        for f in COPYING CREDITS README-IMPORTANT-LICENCE-INFO README.md; do
            [ -f "$data/id1/docs/$f" ] || die "LibreQuake data lacks id1/docs/$f"
            cp -f "$data/id1/docs/$f" "$res/LibreQuake/$f"
        done
        cp -f "$data/MANIFEST.sha256" "$res/LibreQuake/MANIFEST.sha256"
    fi
    cp -f "$res/LibreQuake/COPYING" "$res/Licenses/LibreQuake-data-BSD-3.txt"
    # The arena: a loose map beside LibreQuake's paks (LibreQuake counts as
    # registered data, so the engine reads loose files under id1).
    # Each with its coloured lightmap, <arena>.lit beside the .bsp (the
    # engine loads it from the map's own folder).
    for n in $arena_names; do
        rm -f "$res/id1/maps/$n" "$res/id1/maps/${n%.bsp}.lit"
    done
    if [ "$arena" != - ]; then
        mkdir -p "$res/id1/maps"
        for a in "${arenas[@]}"; do
            cp -f "$a" "$res/id1/maps/$(basename "$a")"
            cp -f "${a%.bsp}.lit" "$res/id1/maps/$(basename "${a%.bsp}").lit"
            echo "psQuake: bundled the arena maps/$(basename "$a") and its .lit"
        done
    else
        echo "psQuake: WARNING: no arena map; New Game opens the start map." >&2
        echo "psQuake: WARNING: run tools/setup-build.sh, then configure again, to build it." >&2
    fi
else
    rm -rf "$res/id1" "$res/LibreQuake"
    rm -f "$res/Licenses/LibreQuake-data-BSD-3.txt"
    echo "psQuake: WARNING: no LibreQuake data in $data." >&2
    echo "psQuake: WARNING: the app will only run with registered Quake in" >&2
    echo "psQuake: WARNING:   ~/Library/Application Support/psQuake/id1 (pak0.pak and pak1.pak)." >&2
    echo "psQuake: WARNING: run tools/fetch-librequake.sh, then build again, to bundle LibreQuake." >&2
fi

# Sign the app last (it seals Resources), then check it.
check_sign_room "$contents/MacOS/psQuake"
codesign --force --sign - "$app" 2>/dev/null
codesign --verify --strict "$app"
echo "psQuake: assembled and ad-hoc signed $app"
