#!/bin/bash
#
# psQuake -- set up everything the build needs beyond the system compiler:
#
#   1. LibreQuake game data (tools/fetch-librequake.sh) into data/librequake/.
#   2. The map compilers for the arena: ericw-tools qbsp, vis and light,
#      built from source at a pinned commit into data/ericw-tools/bin/.
#      light needs Embree 4 and TBB, which come from Homebrew.
#
#   tools/setup-build.sh               check, and install what is missing
#   tools/setup-build.sh --check       report only; exit 1 if anything is missing
#   tools/setup-build.sh --no-data     skip the LibreQuake download
#
# Outside the repository it runs only `brew install` for whichever of embree
# and tbb is missing (both Apache-2.0; Homebrew adds their dependency hwloc,
# and may bootstrap its own Ruby runtime), with Homebrew's auto-update,
# cleanup and upgrades of installed formulae turned off. Everything else stays
# in the git-ignored data/ folder. Without Homebrew it stops and says what to do.
#
# ericw-tools (https://github.com/ericwa/ericw-tools) is GPL-3.0-or-later. It
# is a build tool only: psQuake does not link or ship it; the arena BSP it
# writes is psQuake data.
#
# Pinning: tag 2.0.0-alpha11, which must point at commit c243aa3...; the
# submodules are the commits that commit records; nothing from the clone runs
# until it matches. data/ericw-tools/STAMP records the pin, the Embree/TBB
# versions and the SHA-256 of qbsp, vis and light; --check (run by every
# configure) fails if any of them changed, and the build then skips the arena.
# The source and build trees are removed once the binaries are installed.
#
set -euo pipefail

ERICW_URL='https://github.com/ericwa/ericw-tools.git'
ERICW_TAG='2.0.0-alpha11'
ERICW_COMMIT='c243aa348c9e5a1838bb69cf6c3ff20bebd94a8a'
BREW_FORMULAE=(embree tbb)

die() { echo "setup-build: $*" >&2; exit 1; }
say() { echo "setup-build: $*"; }

check_only=0
want_data=1
for arg in "$@"; do
    case $arg in
        --check) check_only=1 ;;
        --no-data) want_data=0 ;;
        -h|--help) awk 'NR>1 && /^set -euo/ {exit} NR>1' "$0"; exit 0 ;;
        *) die "unknown option: $arg (try --help)" ;;
    esac
done

# Repository root: the parent of this script's directory, checked by marker.
here=$(cd "$(dirname "$0")" && pwd -P)
repo=$(cd "$here/.." && pwd -P)
[ -n "$repo" ] && [ "$repo" != / ] || die "cannot find the repository"
[ -f "$repo/third_party/quakespasm/LICENSE.txt" ] && [ -f "$repo/tools/setup-build.sh" ] ||
    die "not the psQuake repository: $repo"

top="$repo/data/ericw-tools"
src="$top/src"
bld="$top/build"
bin="$top/bin"
stamp="$top/STAMP"
case $top in "$repo"/data/ericw-tools) ;; *) die "bad destination: $top" ;; esac

missing=0

# --- 1. LibreQuake -----------------------------------------------------------
if [ "$want_data" = 1 ]; then
    if [ "$check_only" = 1 ]; then
        if [ -f "$repo/data/librequake/id1/pak0.pak" ]; then
            say "LibreQuake: present"
        else
            say "LibreQuake: missing (run tools/fetch-librequake.sh)"; missing=1
        fi
    else
        "$repo/tools/fetch-librequake.sh"
    fi
fi

# --- 2. Homebrew Embree and TBB ---------------------------------------------
command -v cmake >/dev/null || die "cmake not found: install CMake 3.16 or later"
command -v git >/dev/null || die "git not found: install the Xcode Command Line Tools"
command -v c++ >/dev/null || die "c++ not found: install the Xcode Command Line Tools"

if ! command -v brew >/dev/null; then
    say "Homebrew not found. The arena's light compiler needs Embree 4 and TBB:"
    say "  install Homebrew (https://brew.sh), then run: brew install ${BREW_FORMULAE[*]}"
    [ "$check_only" = 1 ] && exit 1
    die "cannot install ${BREW_FORMULAE[*]} without Homebrew"
fi

brew_versions() {
    local f v out=""
    for f in "${BREW_FORMULAE[@]}"; do
        v=$(brew list --versions "$f" 2>/dev/null || true)
        [ -n "$v" ] || return 1
        out="$out${out:+; }$v"
    done
    printf '%s' "$out"
}

if ! versions=$(brew_versions); then
    if [ "$check_only" = 1 ]; then
        say "Homebrew: missing one of ${BREW_FORMULAE[*]} (run: brew install ${BREW_FORMULAE[*]})"
        exit 1
    fi
    missing_formulae=()
    for f in "${BREW_FORMULAE[@]}"; do
        brew list --versions "$f" >/dev/null 2>&1 || missing_formulae+=("$f")
    done
    say "installing with Homebrew: ${missing_formulae[*]}"
    # Install only what is missing; never upgrade what is already there.
    HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1 \
        HOMEBREW_NO_INSTALL_UPGRADE=1 HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1 \
        HOMEBREW_NO_ENV_HINTS=1 brew install "${missing_formulae[@]}"
    versions=$(brew_versions) || die "brew install ${missing_formulae[*]} did not install them"
fi
case $versions in "embree 4."*) ;; *) die "need Embree 4.x, have: $versions" ;; esac
say "Homebrew: $versions"
brew_prefix=$(brew --prefix)
[ -n "$brew_prefix" ] && [ -d "$brew_prefix" ] || die "cannot find the Homebrew prefix"

# --- 3. ericw-tools ----------------------------------------------------------
# STAMP: the pin, the Embree/TBB versions it was built against, and the
# SHA-256 of each installed binary. The build (tools/arena.cmake) runs
# --check and compiles the arena only when all of it still matches.
stamp_for() { # <versions>: the STAMP the current binaries would have
    local tool
    printf 'ericw-tools %s %s\n' "$ERICW_TAG" "$ERICW_COMMIT"
    printf 'homebrew %s\n' "$1"
    for tool in qbsp vis light; do
        [ -f "$bin/$tool" ] || return 1
        printf '%s %s\n' "$tool" "$(shasum -a 256 "$bin/$tool" | awk '{print $1}')"
    done
}
if [ -x "$bin/qbsp" ] && [ -x "$bin/vis" ] && [ -x "$bin/light" ] && [ -f "$stamp" ] &&
   have=$(stamp_for "$versions") && [ "$have" = "$(cat "$stamp")" ]; then
    say "ericw-tools $ERICW_TAG is built in data/ericw-tools/bin and matches its STAMP"
    exit "$missing"
fi
if [ "$check_only" = 1 ]; then
    say "ericw-tools: not built for this pin, or the binaries or Homebrew libraries changed (run tools/setup-build.sh)"
    exit 1
fi

case $src in "$top"/src) ;; *) die "bad source dir: $src" ;; esac
case $bld in "$top"/build) ;; *) die "bad build dir: $bld" ;; esac
mkdir -p "$top"
# Always a fresh clone: a .git left by an interrupted run could carry other
# remotes, submodule URLs or hooks.
rm -rf "$src" "$bld" "$bin" "$stamp"
git init -q "$src"
git -C "$src" remote add origin "$ERICW_URL"
say "fetching ericw-tools $ERICW_TAG ($ERICW_COMMIT)"
# The annotated tag, so the tools report their version (git describe); the
# commit it points at must be the pin.
git -C "$src" -c protocol.allow=never -c protocol.https.allow=always \
    fetch -q --depth 1 origin "refs/tags/$ERICW_TAG:refs/tags/$ERICW_TAG"
got=$(git -C "$src" rev-parse "refs/tags/$ERICW_TAG^{commit}")
[ "$got" = "$ERICW_COMMIT" ] || die "tag $ERICW_TAG is $got, expected $ERICW_COMMIT"
git -C "$src" checkout -q --force --detach "$ERICW_COMMIT"
got=$(git -C "$src" rev-parse HEAD)
[ "$got" = "$ERICW_COMMIT" ] || die "checked out $got, expected $ERICW_COMMIT"
# Submodules: only https URLs on github.com, at the commits the pinned commit
# records (git checks each fetched commit against that id).
while read -r _key url; do
    case $url in https://github.com/*) ;; *) die "unexpected submodule URL: $url" ;; esac
done < <(git -C "$src" config -f .gitmodules --get-regexp '^submodule\..*\.url$')
git -C "$src" -c protocol.allow=never -c protocol.https.allow=always \
    submodule -q update --init --force --depth 1
if git -C "$src" submodule status --recursive | grep -q '^[-+U]'; then
    git -C "$src" submodule status --recursive >&2
    die "submodules do not match the pinned commit"
fi
say "source matches $ERICW_TAG ($ERICW_COMMIT)"

say "building qbsp, vis and light (a few minutes)"
# ericw-tools' configure step fetches googletest at the commit its own
# CMakeLists pins (GIT_TAG f8d7d77c06936315286eb55f8de22cd23c188571, v1.14.0)
# and runs googletest's CMake code; nothing from it is compiled.
cmake -S "$src" -B "$bld" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$brew_prefix" \
    -DDISABLE_TESTS=ON -DDISABLE_DOCS=ON -DENABLE_LIGHTPREVIEW=OFF \
    > "$top/configure.out" 2>&1 || { tail -40 "$top/configure.out" >&2; die "configure failed (data/ericw-tools/configure.out)"; }
grep -q "git describe returned \"$ERICW_TAG\"" "$top/configure.out" ||
    die "ericw-tools did not take its version from tag $ERICW_TAG (data/ericw-tools/configure.out)"
jobs=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
cmake --build "$bld" --target qbsp vis light -j "$jobs" \
    > "$top/build.out" 2>&1 || { tail -40 "$top/build.out" >&2; die "build failed (data/ericw-tools/build.out)"; }

mkdir -p "$bin"
for tool in qbsp vis light; do
    found=$(find "$bld" -type f -perm -u+x -name "$tool" -print -quit)
    [ -n "$found" ] || die "no $tool binary in $bld"
    cp -f "$found" "$bin/$tool"
done
# Each binary may link only system and Homebrew libraries.
for tool in qbsp vis light; do
    libs=$(otool -L "$bin/$tool") || die "otool cannot read $bin/$tool"
    libs=$(printf '%s\n' "$libs" | awk 'NR>1 {print $1}')
    [ -n "$libs" ] || die "otool lists no libraries for $tool"
    while read -r lib; do
        case $lib in
            /usr/lib/*|/System/*|"$brew_prefix"/*) ;;
            *) die "$tool links $lib, which is not a system or Homebrew library" ;;
        esac
    done <<< "$libs"
done
stamp_for "$versions" > "$stamp" || die "cannot write $stamp"
# Keep only the binaries: the source and build trees hold thousands of C/C++
# files (googletest among them) that psQuake's repository-wide source checks
# would scan, and the binaries need neither.
rm -rf "$src" "$bld"
say "ericw-tools $ERICW_TAG built into data/ericw-tools/bin"
say "configure and build again (cmake -S . -B build) to compile the arena"
exit "$missing"
