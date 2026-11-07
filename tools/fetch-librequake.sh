#!/bin/bash
#
# psQuake -- download LibreQuake v0.09-beta into data/librequake/ (git-ignored)
# so the build can bundle it into psQuake.app.
#
#   tools/fetch-librequake.sh                  download the pinned release
#   tools/fetch-librequake.sh --from-zip FILE  use a copy you already have
#
# The archive is checked against a pinned SHA-256 before anything is
# extracted, every entry is checked to stay inside full/ (no absolute paths,
# no "..", no links), and both paks are checked against pinned SHA-256s after
# extraction. Nothing from the archive is ever run. The result replaces
# data/librequake/ only when every check passes.
#
# LibreQuake: https://github.com/lavenderdotpet/LibreQuake (BSD-3 assets,
# GPL-2 progs; see data/librequake/id1/docs/COPYING and
# README-IMPORTANT-LICENCE-INFO).
#
set -euo pipefail

URL='https://github.com/lavenderdotpet/LibreQuake/releases/download/v0.09-beta/full.zip'
ZIP_SHA256='623e463b35811216244f9ba15e0c45abc1765288650b5e3d36a899b35e4bcf3d'
PAK0_SHA256='08018cb7fc66908881dbae75fbf691125d3f3524fcfddc5c579e8e8877d46382'
PAK1_SHA256='0123a39fe034a93cc69ff033f1f707a49d1f64c91833368d38d9a0c70892fe54'

die() { echo "fetch-librequake: $*" >&2; exit 1; }
say() { echo "fetch-librequake: $*"; }

from_zip=
case ${1-} in
    '') ;;
    --from-zip) [ $# -eq 2 ] || die "usage: --from-zip FILE"; from_zip=$2 ;;
    -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
esac

sha256_of() { shasum -a 256 "$1" | awk '{print $1}'; }

# Repository root: the parent of this script's directory, checked by marker.
here=$(cd "$(dirname "$0")" && pwd -P)
repo=$(cd "$here/.." && pwd -P)
[ -n "$repo" ] && [ "$repo" != / ] || die "cannot find the repository"
[ -f "$repo/third_party/quakespasm/LICENSE.txt" ] && [ -f "$repo/tools/fetch-librequake.sh" ] ||
    die "not the psQuake repository: $repo"

data="$repo/data"
dest="$data/librequake"
work="$data/.fetch-librequake"
case $dest in "$repo"/data/librequake) ;; *) die "bad destination: $dest" ;; esac
case $work in "$repo"/data/.fetch-librequake) ;; *) die "bad work dir: $work" ;; esac

manifest() { # <dir>: sha256 of every file under <dir>/id1, sorted by path
    (cd "$1" && find id1 -type f -print0 | LC_ALL=C sort -z |
        xargs -0 shasum -a 256)
}

if [ -f "$dest/id1/pak0.pak" ] && [ -f "$dest/MANIFEST.sha256" ] &&
   [ "$(sha256_of "$dest/id1/pak0.pak")" = "$PAK0_SHA256" ] &&
   [ "$(sha256_of "$dest/id1/pak1.pak")" = "$PAK1_SHA256" ] &&
   manifest "$dest" | cmp -s - "$dest/MANIFEST.sha256"; then
    say "data/librequake is already LibreQuake v0.09-beta; nothing to do"
    exit 0
fi

mkdir -p "$data"
rm -rf "$work"
mkdir -p "$work/extract"
trap 'rm -rf "$work"' EXIT

zip="$work/full.zip"
if [ -n "$from_zip" ]; then
    [ -f "$from_zip" ] || die "no such file: $from_zip"
    say "using $from_zip"
    cp "$from_zip" "$zip"
else
    say "downloading $URL"
    curl --fail --location --proto '=https' --tlsv1.2 --silent --show-error \
        --output "$zip" "$URL"
fi

got=$(sha256_of "$zip")
[ "$got" = "$ZIP_SHA256" ] || die "archive SHA-256 is $got, expected $ZIP_SHA256"
say "archive SHA-256 matches ($ZIP_SHA256)"

# Every entry must be a plain file or directory under full/.
while IFS= read -r entry; do
    case $entry in
        full/*) ;;
        *) die "archive entry outside full/: $entry" ;;
    esac
    case /$entry/ in
        */../*|*/./*) die "archive entry with a dot component: $entry" ;;
    esac
done < <(zipinfo -1 "$zip")
if zipinfo "$zip" | awk 'NR>2 && /^l/ {found=1} END {exit !found}'; then
    die "archive contains a symbolic link"
fi

unzip -q "$zip" -d "$work/extract"
src="$work/extract/full"
[ -f "$src/id1/pak0.pak" ] && [ -f "$src/id1/pak1.pak" ] || die "no id1 paks in the archive"
[ -z "$(find "$src" -type l)" ] || die "extracted tree contains a link"
[ "$(sha256_of "$src/id1/pak0.pak")" = "$PAK0_SHA256" ] || die "pak0.pak SHA-256 mismatch"
[ "$(sha256_of "$src/id1/pak1.pak")" = "$PAK1_SHA256" ] || die "pak1.pak SHA-256 mismatch"
say "pak0.pak and pak1.pak SHA-256 match"

manifest "$src" > "$src/MANIFEST.sha256"

rm -rf "$dest"
mv "$src" "$dest"
say "LibreQuake v0.09-beta is in data/librequake ($(du -sh "$dest" | awk '{print $1}'))"
say "build again (cmake --build build) to bundle it into psQuake.app"
