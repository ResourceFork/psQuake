#!/bin/bash
#
# psQuake -- build a release psQuake.app and package it as a zip.
#
#   tools/package-release.sh [--build-dir DIR] [--no-build] [--test]
#
#   --build-dir DIR  the CMake build tree, inside the repository and named
#                    build* (default: build-release)
#   --no-build       package the app already in DIR
#   --test           run ctest in DIR before packaging
#
# Configures DIR with CMAKE_BUILD_TYPE=Release and builds it (an arm64,
# ad-hoc signed psQuake.app with LibreQuake and the four arena maps bundled,
# each with its .lit: psqtemple, psqarena, psqterraces, psqtowers and psqtesthall; run
# tools/setup-build.sh first), checks it with src/app/macos/check_bundle.sh
# --release, then writes into DIR/dist:
#
#   psQuake-<version>-arm64.zip         the app (ditto, signatures kept)
#   psQuake-<version>-arm64.zip.sha256  its SHA-256
#   psQuake-<version>-arm64-files.txt   every file in the app, with sizes
#   psQuake-<version>-arm64-check.txt   the bundle check's report
#
# Signs nothing beyond the build's own ad-hoc signature; notarization and
# Developer ID signing are not done. Uploads nothing.
#
set -euo pipefail

die() { echo "package-release: $*" >&2; exit 1; }

repo=$(cd "$(dirname "$0")/.." && pwd -P)
[ -f "$repo/src/app/macos/check_bundle.sh" ] || die "not the psQuake repository: $repo"

build="$repo/build-release"
do_build=1
do_test=0
while [ $# -gt 0 ]; do
    case $1 in
        --build-dir) [ $# -ge 2 ] || die "--build-dir needs a directory"; build=$2; shift 2 ;;
        --no-build) do_build=0; shift ;;
        --test) do_test=1; shift ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done
case $build in /*) ;; *) build="$repo/$build" ;; esac
build=${build%/}
# Only a build tree of this repository: everything below writes or removes
# files in it.
[ -n "$build" ] && [ "$build" != / ] || die "refusing build directory: '$build'"
case $build in "$repo"/build*) ;; *) die "the build directory must be $repo/build*: $build" ;; esac
case $build in */..|*/../*|*/.|*/./*) die "the build directory must be a plain path: $build" ;; esac

if [ $do_build -eq 1 ]; then
    cmake -S "$repo" -B "$build" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$build" -j "$(sysctl -n hw.ncpu)"
fi
[ -f "$build/CMakeCache.txt" ] || die "no CMake build tree in $build"
grep -qx "CMAKE_BUILD_TYPE:STRING=Release" "$build/CMakeCache.txt" ||
    die "$build is not a Release build tree (CMAKE_BUILD_TYPE)"
app="$build/psQuake.app"
[ -f "$app/Contents/MacOS/psQuake" ] || die "no psQuake.app in $build"
if [ $do_test -eq 1 ]; then
    ctest --test-dir "$build" --output-on-failure
fi

version=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$app/Contents/Info.plist")
case $version in [0-9]*.[0-9]*) ;; *) die "unexpected version in Info.plist: $version" ;; esac
name="psQuake-$version-arm64"
dist="$build/dist"
mkdir -p "$dist"
[ -d "$dist" ] || die "no dist directory: $dist"

report="$dist/$name-check.txt"
if ! /bin/bash "$repo/src/app/macos/check_bundle.sh" --release --clean-source "$app" > "$report"; then
    cat "$report"
    die "the bundle check failed; nothing packaged"
fi

zip="$dist/$name.zip"
rm -f "$zip" "$zip.sha256" "$dist/$name-files.txt"
(cd "$build" && find psQuake.app -type f -exec stat -f '%z %N' {} + | sort -k2) \
    > "$dist/$name-files.txt"
ditto -c -k --sequesterRsrc --keepParent "$app" "$zip"
(cd "$dist" && shasum -a 256 "$name.zip" > "$name.zip.sha256")

# The zip must unpack to an app whose signature still verifies.
check_dir="$dist/.unpack-check"
rm -rf "$check_dir"
mkdir -p "$check_dir"
ditto -x -k "$zip" "$check_dir"
codesign --verify --strict --deep "$check_dir/psQuake.app" ||
    die "the zipped app's signature does not verify"
rm -rf "$check_dir"

echo "package-release: $zip"
cat "$zip.sha256"
echo "package-release: $(wc -l < "$dist/$name-files.txt" | tr -d ' ') files; check report $report"
