#!/bin/bash
#
# psQuake -- headless load of the arena in the built app, then the exit to
# the first campaign map.
#
#   arena_load_test.sh <psQuake.app> <scratch root> <exit map>
#
# Runs the engine as a dedicated server (no window, no input, UDP bound to
# 127.0.0.1 only on a per-run port) with -psquake-root in the build tree, so
# nothing is written to ~/Library/Application Support/psQuake. A config
# file in the scratch root loads psqarena, tries to save there, changes level
# to <exit map> (the path the arena's exit takes), saves again, then loads
# the arena as PSQARENA (the file system ignores case) and tries to save. The run
# passes when the console log shows:
#   - psqarena loaded, with psq_arena left unspawned (its QuakeC arrives in
#     phase 2, so the engine reports no spawn function for it);
#   - the save in the arena refused;
#   - <exit map> loaded after the changelevel, and a save written there;
#   - the save refused again in the arena loaded as PSQARENA;
#   - no Host_Error, SV_Error or other engine error.
# A dedicated server has no player, so the spawn parms reset itself is
# covered by psq_arena_test and the live check.
#
set -euo pipefail

die() { echo "arena_load_test: FAIL: $*" >&2; exit 1; }

[ $# -eq 3 ] || die "usage: arena_load_test.sh <psQuake.app> <scratch root> <exit map>"
app=$1
root=$2
exit_map=$3

for p in "$app" "$root"; do
    [ -n "$p" ] || die "empty path"
    case $p in /*) ;; *) die "not absolute: $p" ;; esac
    [ "$p" != / ] || die "refusing /"
done
case $root in */psq-arena-load-root) ;; *) die "scratch root must end in psq-arena-load-root: $root" ;; esac
case $root in "$HOME"/Library/*) die "refusing a root under ~/Library: $root" ;; esac
case $exit_map in *[!a-z0-9_]*|'') die "bad map name: $exit_map" ;; esac
exe="$app/Contents/MacOS/psQuake"
[ -x "$exe" ] || die "no engine at $exe"
[ -f "$app/Contents/Resources/id1/maps/psqarena.bsp" ] || die "the app has no arena"

rm -rf "$root"
mkdir -p "$root/librequake/id1"
printf '%s\n' \
    'map psqarena' 'wait' 'status' 'save psq_arena_try' \
    "changelevel $exit_map" 'wait' 'status' 'save psq_campaign_try' \
    'map PSQARENA' 'wait' 'save psq_upper_try' 'quit' \
    > "$root/librequake/id1/psqcheck.cfg"

port=$((27000 + $$ % 900))
out="$root/engine.out"
"$exe" -dedicated 1 -port "$port" -psquake-root "$root" -condebug \
    +exec psqcheck.cfg < /dev/null > "$out" 2>&1 &
pid=$!
for _ in $(seq 1 300); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$pid" 2>/dev/null; then
    # Our own child, by the pid we recorded; check it is still the engine.
    if ps -p "$pid" -o command= | grep -qF -- "$exe"; then
        kill "$pid"
    fi
    wait "$pid" 2>/dev/null || true
    cat "$out"
    die "the engine did not quit within 30 s"
fi
rc=0
wait "$pid" || rc=$?
log="$root/librequake/qconsole.log"
[ -f "$log" ] || { cat "$out"; die "no console log at $log"; }
echo "--- console log ($log)"
cat "$log"
echo "---"
[ "$rc" -eq 0 ] || die "engine exit $rc"

expect() { # <what> <fixed string>
    grep -qF -- "$2" "$log" || die "$1: no '$2' in the log"
    echo "arena_load_test: PASS $1"
}
expect "arena loaded" "map:     psqarena"
expect "psq_arena left unspawned" "classname      psq_arena"
# psq_arena must be the only entity the progs cannot spawn.
unspawned=$(grep -c "No spawn function for" "$log" || true)
[ "$unspawned" -eq 2 ] || die "$unspawned 'No spawn function' reports, expected 2 (psq_arena, loaded twice)"
others=$(grep -A3 "No spawn function for" "$log" | grep "^classname" | grep -vc "psq_arena$" || true)
[ "$others" -eq 0 ] || die "entities other than psq_arena have no spawn function"
echo "arena_load_test: PASS only psq_arena is unspawned"
expect "save refused in the arena" "Can't save in the psQuake arena."
expect "exit map loaded" "map:     $exit_map"
expect "save allowed on the campaign map" "psq_campaign_try.sav"
[ -f "$root/librequake/id1/psq_campaign_try.sav" ] || die "no campaign save file"
[ ! -e "$root/librequake/id1/psq_arena_try.sav" ] || die "a save was written in the arena"
[ ! -e "$root/librequake/id1/psq_upper_try.sav" ] || die "a save was written in the arena loaded as PSQARENA"
refusals=$(grep -c "Can't save in the psQuake arena." "$log" || true)
[ "$refusals" -eq 2 ] || die "$refusals save refusals, expected 2 (psqarena and PSQARENA)"
echo "arena_load_test: PASS save refused in the arena loaded as PSQARENA"
if grep -E "Host_Error|SV_Error|QUAKE ERROR|cannot find map|couldn't spawn|Couldn't spawn" "$log" "$out"; then
    die "engine error in the log"
fi
order=$(grep -nE "map: +(psqarena|$exit_map)|Can't save in the psQuake arena|psq_campaign_try.sav" "$log" |
    cut -d: -f2- | tr -s ' ' | cut -c1-40 | tr '\n' '|')
case $order in
    "map: psqarena|Can't save in the psQuake arena.|map: $exit_map|Saving game to "*) ;;
    *) die "steps out of order: $order" ;;
esac
echo "arena_load_test: PASS (arena loaded, save refused, exit to $exit_map, save there)"
