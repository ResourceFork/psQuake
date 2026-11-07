#!/usr/bin/env bash
#
# psproc-drift.sh -- compare psproc/ with the psDoom files it was imported
# from, and re-import psDoom changes through the same path map.
#
# psproc/ is a copy of psDoom's engine-free process layer. psproc/UPSTREAM
# holds the full psDoom commit it was last synced to (the "base"). Files may
# be renamed (the path map below) and carry psQuake adaptations. One rename
# reaches inside files: psDoom's psdoom_* option identifiers are psproc_* here,
# so every C file read from psDoom passes through RENAME_SED first.
#
# Usage:
#   tools/psproc-drift.sh [--psdoom DIR] [--commit REV] [--diff]
#       Report, never edits. For each mapped file: whether psproc/ differs
#       from psDoom at the base (a local adaptation), and whether psDoom
#       changed it between the base and REV (default: the base). Also lists
#       psDoom files under the watched paths that changed in that range with
#       no map or ignore entry, and mapped files still holding conflict
#       markers. --diff prints `diff -u <psDoom@REV> <psproc>` (after the
#       rename filter) for every file that differs.
#       Exit: 0 nothing pending, 1 upstream changes or problems to look at,
#       2 usage or repository error.
#
#   tools/psproc-drift.sh --apply REV [--psdoom DIR]
#       Re-import psDoom REV (a descendant of the base) into psproc/. For
#       every mapped file psDoom changed between the base and REV: a pure copy
#       where psproc/ still equals psDoom at the base, otherwise a 3-way merge
#       (git merge-file) that keeps the psQuake adaptation and adds psDoom's
#       change. Refuses to run with uncommitted changes under psproc/, and
#       never commits. psproc/UPSTREAM moves to REV only when nothing is left
#       to do; otherwise it stays at the base and the tool lists what to
#       finish, then `--mark REV`. Exit: 0 applied cleanly, 1 work left, 2
#       error.
#
#   tools/psproc-drift.sh --mark REV [--psdoom DIR]
#       After finishing an --apply by hand: check that no mapped file holds
#       conflict markers, that every watched psDoom change up to REV is mapped
#       or ignored, and that no mapped file was removed in psDoom; then write
#       REV to psproc/UPSTREAM. Exit: 0 written, 1 refused, 2 error.
#
#   tools/psproc-drift.sh --print-map
#       Print the path map (psDoom path, TAB, psQuake path).
#
# Options:
#   --psdoom DIR   psDoom git repository (default: ../psDoom next to this
#                  repository). Read only, through git show / diff / cat-file;
#                  its working tree and branches are never touched.
#   --root DIR     psQuake repository root (default: this script's repo).
#
# Re-importing a psDoom fix:
#   1. tools/psproc-drift.sh --commit <rev>         (see what will change)
#   2. tools/psproc-drift.sh --apply <rev>
#   3. If it exits 1: resolve conflicts, add map or ignore entries for the
#      files it lists (edit PATH_MAP / IGNORED below), then
#      tools/psproc-drift.sh --mark <rev>.
#   4. Build and test, then commit psproc/ (and this script, if the map
#      changed) with "Re-import psproc from ResourceFork/psDoom <rev>: <paths>".
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

# psDoom paths (git pathspecs) whose changes are surfaced even without a map
# entry: a new backend or triage file, or a test the layer may need.
WATCHED=("src/platform" "src/psdoom/proc_*" "src/psdoom/psdoom_options*"
         "tests" "docs/script-backend.md")

# Watched psDoom paths that psproc/ deliberately does not carry.
IGNORED=(
    "tests/window_state_test.c"       # Crispy Doom SDL window predicates
)

# Applied to every psDoom C file before it is compared or merged.
RENAME_SED='s/psdoom_/psproc_/g; s/PSDOOM_OPTIONS_H/PSPROC_OPTIONS_H/g'

die() { printf 'psproc-drift: %s\n' "$*" >&2; exit 2; }

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=""
psdoom=""
rev=""
apply_rev=""
mark_rev=""
show_diff=0

while [ $# -gt 0 ]; do
    case "$1" in
        --psdoom)    [ $# -ge 2 ] || die "--psdoom needs a directory"; psdoom=$2; shift 2 ;;
        --root)      [ $# -ge 2 ] || die "--root needs a directory"; root=$2; shift 2 ;;
        --commit)    [ $# -ge 2 ] || die "--commit needs a revision"; rev=$2; shift 2 ;;
        --apply)     [ $# -ge 2 ] || die "--apply needs a revision"; apply_rev=$2; shift 2 ;;
        --mark)      [ $# -ge 2 ] || die "--mark needs a revision"; mark_rev=$2; shift 2 ;;
        --diff)      show_diff=1; shift ;;
        --print-map) for m in "${PATH_MAP[@]}"; do printf '%s\t%s\n' "${m%%|*}" "${m#*|}"; done; exit 0 ;;
        -h|--help)   sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *)           die "unknown argument: $1 (try --help)" ;;
    esac
done
modes=0
[ -n "$rev" ] && modes=$((modes + 1))
[ -n "$apply_rev" ] && modes=$((modes + 1))
[ -n "$mark_rev" ] && modes=$((modes + 1))
[ "$modes" -le 1 ] || die "use one of --commit, --apply, --mark"
if [ "$show_diff" -eq 1 ] && [ -z "$rev" ] && [ "$modes" -eq 1 ]; then
    die "--diff only goes with the report (optionally with --commit)"
fi

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

want=${apply_rev:-${mark_rev:-${rev:-$base}}}
target=$(git -C "$psdoom" rev-parse --verify --quiet "$want^{commit}") \
    || die "revision not found in $psdoom: $want"

descends=1
git -C "$psdoom" merge-base --is-ancestor "$base" "$target" || descends=0

# psDoom file at a commit, through the rename filter for C files.
show() {
    case "$2" in
        *.c|*.h) git -C "$psdoom" show "$1:$2" 2>/dev/null | sed "$RENAME_SED" ;;
        *)       git -C "$psdoom" show "$1:$2" 2>/dev/null ;;
    esac
}
exists_at() { git -C "$psdoom" cat-file -e "$1:$2" 2>/dev/null; }

upstream_changed() {   # $1 = psDoom path; true if base..target changed it
    [ -n "$(git -C "$psdoom" diff --name-only "$base" "$target" -- "$1")" ]
}

unmapped_changes() {   # watched psDoom paths changed base..target, unmapped
    local f m known
    git -C "$psdoom" diff --name-only "$base" "$target" -- "${WATCHED[@]}" |
    while IFS= read -r f; do
        known=0
        for m in "${PATH_MAP[@]}"; do
            [ "${m%%|*}" = "$f" ] && known=1
        done
        for m in "${IGNORED[@]}"; do
            [ "$m" = "$f" ] && known=1
        done
        [ "$known" -eq 1 ] || printf '%s\n' "$f"
    done
}

marked_files() {       # mapped psproc files that still hold conflict markers
    local m dst
    for m in "${PATH_MAP[@]}"; do
        dst=${m#*|}
        if [ -f "$root/$dst" ] && grep -qE '^(<<<<<<<|>>>>>>>)( |$)' "$root/$dst"; then
            printf '%s\n' "$dst"
        fi
    done
}

removed_upstream() {   # mapped psDoom paths absent at target
    local m
    for m in "${PATH_MAP[@]}"; do
        exists_at "$target" "${m%%|*}" || printf '%s\n' "${m%%|*}"
    done
}

list() {               # print a heading and the indented lines of $2, if any
    [ -n "$2" ] || return 0
    printf '\n%s\n' "$1"
    printf '%s\n' "$2" | while IFS= read -r line; do printf '  %s\n' "$line"; done
}

short() { git -C "$psdoom" rev-parse --short "$1"; }

# ---- report -----------------------------------------------------------------
if [ -z "$apply_rev" ] && [ -z "$mark_rev" ]; then
    printf 'psDoom: %s\nbase (psproc/UPSTREAM): %s\ncompared with: %s\n' \
        "$psdoom" "$(short "$base")" "$(short "$target")"
    if [ "$descends" -eq 0 ]; then
        printf 'WARNING: %s does not descend from the base; --apply will refuse it.\n' "$(short "$target")"
    fi
    printf '\n%-34s %-9s %s\n' "psQuake file" "local" "upstream"
    pending=0
    for m in "${PATH_MAP[@]}"; do
        src=${m%%|*}; dst=${m#*|}
        if [ ! -f "$root/$dst" ]; then
            local_state="MISSING"; pending=1
        elif exists_at "$base" "$src" && show "$base" "$src" | cmp -s - "$root/$dst"; then
            local_state="same"
        else
            local_state="adapted"
        fi
        if ! exists_at "$target" "$src"; then
            up_state="REMOVED in psDoom"; pending=1
        elif upstream_changed "$src"; then
            up_state="CHANGED ($src)"; pending=1
        else
            up_state="-"
        fi
        printf '%-34s %-9s %s\n' "$dst" "$local_state" "$up_state"
    done
    unmapped=$(unmapped_changes)
    markers=$(marked_files)
    [ -z "$unmapped" ] || pending=1
    [ -z "$markers" ] || pending=1
    list "Changed in psDoom with no map or ignore entry:" "$unmapped"
    list "Conflict markers left in:" "$markers"
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
        printf '\nPending: see above (re-import with --apply %s).\n' "$(short "$target")"
        exit 1
    fi
    printf '\nNothing pending between base and %s.\n' "$(short "$target")"
    exit 0
fi

[ "$descends" -eq 1 ] || die "$(short "$target") does not descend from the base $(short "$base"); refusing to move psproc/ backwards or sideways"

# ---- mark -------------------------------------------------------------------
if [ -n "$mark_rev" ]; then
    markers=$(marked_files)
    unmapped=$(unmapped_changes)
    removed=$(removed_upstream)
    list "Conflict markers left in:" "$markers"
    list "Changed in psDoom with no map or ignore entry:" "$unmapped"
    list "Mapped but removed in psDoom (drop the map entry and the file):" "$removed"
    if [ -n "$markers$unmapped$removed" ]; then
        printf '\npsproc/UPSTREAM left at %s.\n' "$(short "$base")"
        exit 1
    fi
    printf '%s\n' "$target" > "$root/psproc/UPSTREAM"
    printf 'psproc/UPSTREAM -> %s\n' "$target"
    exit 0
fi

# ---- apply -------------------------------------------------------------------
toplevel=$(git -C "$root" rev-parse --show-toplevel 2>/dev/null || true)
toplevel=$(cd "${toplevel:-/nonexistent}" 2>/dev/null && pwd || true)
[ "$toplevel" = "$root" ] \
    || die "$root is not the top of a git work tree; --apply needs one to keep the change reviewable"
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
for m in "${PATH_MAP[@]}"; do
    src=${m%%|*}; dst=${m#*|}
    upstream_changed "$src" || continue
    exists_at "$target" "$src" || continue        # reported below
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
        fi
    fi
    changed+=("$src")
done

markers=$(marked_files)
unmapped=$(unmapped_changes)
removed=$(removed_upstream)
list "Resolve conflicts in:" "$markers"
list "Changed in psDoom with no map or ignore entry (not imported):" "$unmapped"
list "Mapped but removed in psDoom (left in place):" "$removed"

status=0
if [ -n "$markers$unmapped$removed" ]; then
    status=1
    printf '\npsproc/UPSTREAM left at %s. Finish the items above, then run:\n  %s --mark %s\n' \
        "$(short "$base")" "$0" "$(short "$target")"
else
    printf '%s\n' "$target" > "$root/psproc/UPSTREAM"
    printf '\npsproc/UPSTREAM -> %s\n' "$target"
fi
if [ ${#changed[@]} -gt 0 ]; then
    printf 'Suggested commit message:\n  Re-import psproc from ResourceFork/psDoom %s: %s\n' \
        "$(short "$target")" "${changed[*]}"
fi
exit "$status"
