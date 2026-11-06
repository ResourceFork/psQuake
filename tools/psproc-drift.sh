#!/usr/bin/env bash
#
# psproc-drift.sh -- compare psproc/ with the psDoom files it was imported
# from, and re-import psDoom changes through the same path map.
#
# psproc/ is a copy of psDoom's engine-free process layer. psproc/UPSTREAM
# holds the full psDoom commit it was last synced to (the "base"). Files may
# be renamed (the path map below) and carry psQuake adaptations.
#
# Usage:
#   tools/psproc-drift.sh [--psdoom DIR] [--commit REV] [--diff]
#       Report, never edits. For each mapped file: whether psproc/ differs
#       from psDoom at the base (a local adaptation), and whether psDoom
#       changed it between the base and REV (default: the base). Also lists
#       psDoom files under the watched paths that changed in that range but
#       have no map entry. --diff prints `diff -u <psDoom@REV> <psproc>` for
#       every file that differs.
#       Exit: 0 nothing changed upstream, 1 upstream changes to bring over,
#       2 usage or repository error.
#
#   tools/psproc-drift.sh --apply REV [--psdoom DIR]
#       Re-import psDoom REV into psproc/. For every mapped file psDoom
#       changed between the base and REV: a pure copy where psproc/ still
#       equals psDoom at the base, otherwise a 3-way merge (git merge-file)
#       that keeps the psQuake adaptation and adds psDoom's change. Then
#       writes REV to psproc/UPSTREAM. Refuses to run with uncommitted
#       changes under psproc/, never commits, and leaves conflict markers
#       for you to resolve. Exit: 0 applied cleanly, 1 conflicts or
#       unmapped upstream files to look at, 2 error.
#
#   tools/psproc-drift.sh --print-map
#       Print the path map (psDoom path, TAB, psQuake path).
#
# Options:
#   --psdoom DIR   psDoom git repository (default: ../psDoom next to this
#                  repository). Read only, through `git show`; its working
#                  tree and branches are never touched.
#   --root DIR     psQuake repository root (default: this script's repo).
#
# Re-importing a psDoom fix:
#   1. git -C ../psDoom fetch                       (or use a local commit)
#   2. tools/psproc-drift.sh --commit <rev>         (see what will change)
#   3. tools/psproc-drift.sh --apply <rev>
#   4. Resolve any conflicts, map any new files the report lists, build
#      and test, then commit psproc/ alone with the message
#      "Re-import psproc from ResourceFork/psDoom <rev>: <paths>".
#
set -euo pipefail

# psDoom path -> psQuake path. Keep in step with the import commit message.
PATH_MAP=(
    "src/platform/proc_types.h|psproc/include/proc_types.h"
    "src/platform/proc_backend.h|psproc/include/proc_backend.h"
    "src/platform/proc_fake.h|psproc/include/proc_fake.h"
    "src/platform/proc_script.h|psproc/include/proc_script.h"
    "src/psdoom/proc_select.h|psproc/include/proc_select.h"
    "src/psdoom/psdoom_options.h|psproc/include/psproc_options.h"
    "src/platform/proc_backend.c|psproc/src/proc_backend.c"
    "src/platform/proc_fake.c|psproc/src/proc_fake.c"
    "src/platform/proc_script.c|psproc/src/proc_script.c"
    "src/psdoom/proc_select.c|psproc/src/proc_select.c"
    "src/psdoom/psdoom_options.c|psproc/src/psproc_options.c"
    "src/platform/macos/proc_macos.h|psproc/src/macos/proc_macos.h"
    "src/platform/macos/proc_macos.c|psproc/src/macos/proc_macos.c"
    "tests/proc_select_test.c|psproc/tests/proc_select_test.c"
    "docs/script-backend.md|psproc/docs/script-backend.md"
)

# psDoom paths whose changes the report surfaces even without a map entry
# (a new backend file or test the layer may need).
WATCHED=(src/platform src/psdoom/proc_select.c src/psdoom/proc_select.h
         src/psdoom/psdoom_options.c src/psdoom/psdoom_options.h
         tests docs/script-backend.md)

die() { printf 'psproc-drift: %s\n' "$*" >&2; exit 2; }

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=""
psdoom=""
rev=""
apply_rev=""
show_diff=0

while [ $# -gt 0 ]; do
    case "$1" in
        --psdoom)    [ $# -ge 2 ] || die "--psdoom needs a directory"; psdoom=$2; shift 2 ;;
        --root)      [ $# -ge 2 ] || die "--root needs a directory"; root=$2; shift 2 ;;
        --commit)    [ $# -ge 2 ] || die "--commit needs a revision"; rev=$2; shift 2 ;;
        --apply)     [ $# -ge 2 ] || die "--apply needs a revision"; apply_rev=$2; shift 2 ;;
        --diff)      show_diff=1; shift ;;
        --print-map) for m in "${PATH_MAP[@]}"; do printf '%s\t%s\n' "${m%%|*}" "${m#*|}"; done; exit 0 ;;
        -h|--help)   sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *)           die "unknown argument: $1 (try --help)" ;;
    esac
done
[ -n "$apply_rev" ] && [ -n "$rev" ] && die "use --commit or --apply, not both"

# ---- locate and check both repositories ------------------------------------
if [ -z "$root" ]; then
    root=$(git -C "$script_dir" rev-parse --show-toplevel 2>/dev/null) \
        || die "cannot find the psQuake repository above $script_dir"
fi
root=$(cd "$root" 2>/dev/null && pwd) || die "no such directory: $root"
[ "$root" != "/" ] || die "refusing to use / as the psQuake root"
[ -f "$root/psproc/UPSTREAM" ] || die "$root/psproc/UPSTREAM not found (is $root the psQuake root?)"

if [ -z "$psdoom" ]; then
    psdoom="$root/../psDoom"
fi
psdoom=$(cd "$psdoom" 2>/dev/null && pwd) || die "psDoom repository not found (pass --psdoom DIR)"
git -C "$psdoom" rev-parse --git-dir >/dev/null 2>&1 || die "$psdoom is not a git repository"

base=$(tr -d '[:space:]' < "$root/psproc/UPSTREAM")
[ -n "$base" ] || die "psproc/UPSTREAM is empty"
base=$(git -C "$psdoom" rev-parse --verify --quiet "$base^{commit}") \
    || die "base commit from psproc/UPSTREAM is not in $psdoom (fetch it first)"

target=${apply_rev:-${rev:-$base}}
target=$(git -C "$psdoom" rev-parse --verify --quiet "$target^{commit}") \
    || die "revision not found in $psdoom: ${apply_rev:-$rev}"

# psDoom blob at a commit, or nothing if absent. Status 1 when absent.
show() { git -C "$psdoom" show "$1:$2" 2>/dev/null; }
exists_at() { git -C "$psdoom" cat-file -e "$1:$2" 2>/dev/null; }

upstream_changed() {   # $1 = psDoom path; true if base..target changed it
    [ -n "$(git -C "$psdoom" diff --name-only "$base" "$target" -- "$1")" ]
}

unmapped_changes() {   # watched psDoom paths changed base..target, unmapped
    local f m mapped
    git -C "$psdoom" diff --name-only "$base" "$target" -- "${WATCHED[@]}" |
    while IFS= read -r f; do
        mapped=0
        for m in "${PATH_MAP[@]}"; do
            [ "${m%%|*}" = "$f" ] && mapped=1
        done
        [ "$mapped" -eq 1 ] || printf '%s\n' "$f"
    done
}

short() { git -C "$psdoom" rev-parse --short "$1"; }

# ---- report -----------------------------------------------------------------
if [ -z "$apply_rev" ]; then
    printf 'psDoom: %s\nbase (psproc/UPSTREAM): %s\ncompared with: %s\n\n' \
        "$psdoom" "$(short "$base")" "$(short "$target")"
    printf '%-34s %-9s %s\n' "psQuake file" "local" "upstream"
    pending=0
    for m in "${PATH_MAP[@]}"; do
        src=${m%%|*}; dst=${m#*|}
        if [ ! -f "$root/$dst" ]; then
            local_state="missing"
        elif exists_at "$base" "$src" && show "$base" "$src" | cmp -s - "$root/$dst"; then
            local_state="same"
        else
            local_state="adapted"
        fi
        if ! exists_at "$target" "$src"; then
            up_state="removed in psDoom"; pending=1
        elif upstream_changed "$src"; then
            up_state="CHANGED ($src)"; pending=1
        else
            up_state="-"
        fi
        printf '%-34s %-9s %s\n' "$dst" "$local_state" "$up_state"
    done
    unmapped=$(unmapped_changes)
    if [ -n "$unmapped" ]; then
        pending=1
        printf '\nChanged in psDoom with no map entry (map them if psproc needs them):\n'
        printf '  %s\n' $unmapped
    fi
    if [ "$show_diff" -eq 1 ]; then
        for m in "${PATH_MAP[@]}"; do
            src=${m%%|*}; dst=${m#*|}
            [ -f "$root/$dst" ] || continue
            if ! show "$target" "$src" | cmp -s - "$root/$dst"; then
                printf '\n'
                show "$target" "$src" | diff -u --label "psDoom@$(short "$target"):$src" \
                    --label "$dst" - "$root/$dst" || true
            fi
        done
    fi
    if [ "$pending" -eq 1 ]; then
        printf '\nUpstream changes pending: re-import with --apply %s\n' "$(short "$target")"
        exit 1
    fi
    printf '\nNo upstream changes between base and %s.\n' "$(short "$target")"
    exit 0
fi

# ---- apply -------------------------------------------------------------------
if [ -n "$(git -C "$root" status --porcelain -- psproc)" ]; then
    die "psproc/ has uncommitted changes; commit or discard them first"
fi
[ "$base" != "$target" ] || { printf 'psproc is already at psDoom %s.\n' "$(short "$target")"; exit 0; }

# Scratch for the base/target blobs git merge-file needs: inside the repo
# root, under a name .gitignore covers (build*/), removed on exit.
tmp=$(mktemp -d "$root/build-psproc-drift.XXXXXX")
cleanup() {
    case "$tmp" in
        "$root"/build-psproc-drift.*) [ -d "$tmp" ] && rm -rf -- "$tmp" ;;
        *) printf 'psproc-drift: not removing unexpected scratch path %s\n' "$tmp" >&2 ;;
    esac
}
trap cleanup EXIT

changed=()
conflicts=()
for m in "${PATH_MAP[@]}"; do
    src=${m%%|*}; dst=${m#*|}
    upstream_changed "$src" || continue
    if ! exists_at "$target" "$src"; then
        printf 'removed in psDoom, left in place: %s (%s)\n' "$dst" "$src"
        conflicts+=("$dst (removed upstream)")
        continue
    fi
    show "$target" "$src" > "$tmp/target"
    if [ ! -f "$root/$dst" ]; then
        mkdir -p "$(dirname "$root/$dst")"
        cp "$tmp/target" "$root/$dst"
        printf 'added    %s\n' "$dst"
    elif exists_at "$base" "$src" && show "$base" "$src" | cmp -s - "$root/$dst"; then
        cp "$tmp/target" "$root/$dst"
        printf 'copied   %s\n' "$dst"
    else
        if exists_at "$base" "$src"; then show "$base" "$src" > "$tmp/base"; else : > "$tmp/base"; fi
        if git merge-file -L "$dst (psQuake)" -L "psDoom $(short "$base")" \
               -L "psDoom $(short "$target")" "$root/$dst" "$tmp/base" "$tmp/target"; then
            printf 'merged   %s\n' "$dst"
        else
            printf 'CONFLICT %s\n' "$dst"
            conflicts+=("$dst")
        fi
    fi
    changed+=("$src")
done

printf '%s\n' "$target" > "$root/psproc/UPSTREAM"

unmapped=$(unmapped_changes)
status=0
if [ -n "$unmapped" ]; then
    printf '\nChanged in psDoom with no map entry (not imported):\n'
    printf '  %s\n' $unmapped
    status=1
fi
if [ ${#conflicts[@]} -gt 0 ]; then
    printf '\nResolve conflicts in:\n'
    printf '  %s\n' "${conflicts[@]}"
    status=1
fi
printf '\npsproc/UPSTREAM -> %s\n' "$target"
if [ ${#changed[@]} -gt 0 ]; then
    printf 'Suggested commit message:\n  Re-import psproc from ResourceFork/psDoom %s: %s\n' \
        "$(short "$target")" "${changed[*]}"
fi
exit "$status"
