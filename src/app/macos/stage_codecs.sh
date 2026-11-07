#!/bin/bash
#
# psQuake -- stage the vendored Ogg/Vorbis dylibs for linking and bundling.
#
#   stage_codecs.sh <vendored codecs/lib dir> <output dir> <archs>
#
# <archs> is a comma-separated list ("arm64,x86_64"). For each of libogg, libvorbis and
# libvorbisfile this thins the vendored fat dylib (it also carries ppc and
# i386) to <archs>, points its install name and its references to the other
# two at @executable_path/../Frameworks/ (the vendored copies use
# @executable_path/), and ad-hoc signs it. The engine links these staged
# copies, so its load commands name the Frameworks paths too. No LC_RPATH is
# involved anywhere.
#
set -euo pipefail

die() { echo "stage_codecs.sh: $*" >&2; exit 1; }

[ $# -eq 3 ] || die "usage: stage_codecs.sh <codecs lib dir> <out dir> <archs>"
src=$1
out=$2
archs=$3

for p in "$src" "$out"; do
    [ -n "$p" ] || die "empty path"
    case $p in /*) ;; *) die "not absolute: $p" ;; esac
    [ "$p" != / ] || die "refusing /"
done
[ -f "$src/libvorbisfile.dylib" ] || die "no vendored codecs in $src"
[ -n "$archs" ] || die "no architectures"

arch_args=()
IFS=',' read -r -a arch_list <<< "$archs"
for a in "${arch_list[@]}"; do
    case $a in arm64|x86_64) arch_args+=(--arch "$a") ;;
        *) die "unsupported architecture: $a" ;; esac
done

mkdir -p "$out"
fw='@executable_path/../Frameworks'
for lib in libogg libvorbis libvorbisfile; do
    tmp="$out/$lib.dylib.tmp"
    rm -f "$tmp"
    ditto "${arch_args[@]}" "$src/$lib.dylib" "$tmp"
    install_name_tool -id "$fw/$lib.dylib" "$tmp" 2>/dev/null
    for dep in libogg libvorbis; do
        install_name_tool -change "@executable_path/$dep.dylib" \
            "$fw/$dep.dylib" "$tmp" 2>/dev/null
    done
    codesign --force --sign - "$tmp" 2>/dev/null
    mv -f "$tmp" "$out/$lib.dylib"
done

# Every reference must now be a system library or one of the three.
for lib in libogg libvorbis libvorbisfile; do
    otool -L "$out/$lib.dylib" | awk '/^[[:space:]]/ {print $1}' |
    while read -r dep; do
        case $dep in
            /usr/lib/*|"$fw"/libogg.dylib|"$fw"/libvorbis.dylib|"$fw"/libvorbisfile.dylib) ;;
            *) die "$lib.dylib still references $dep" ;;
        esac
    done
done
