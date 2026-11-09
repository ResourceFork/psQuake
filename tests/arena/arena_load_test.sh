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
#   - psqarena loaded, and its psq_arena entity spawned as the process
#     controller with the map's grid (11 x 7 cells of 96); every entity has
#     a spawn function and the psq_ grid keys are QuakeC fields;
#   - no process monster spawned: a dedicated server keeps the process gate
#     shut, so this run of the real app lists and touches no process;
#   - the save in the arena refused;
#   - <exit map> loaded after the changelevel, and a save written there;
#   - the save refused again in the arena loaded as PSQARENA;
#   - no Host_Error, SV_Error or other engine error.
# A dedicated server has no player, so the spawn parms reset and the
# monsters themselves are covered by psq_headless_arena_* (fake processes)
# and the live check.
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
# The controller first thinks a second into the level: stay in each arena
# load for 60 frames (3 s at the dedicated server's 20 Hz) so it polls.
frames=$(printf 'wait\n%.0s' $(seq 1 60))
{
    printf '%s\n' 'map psqarena'
    printf '%s\n' "$frames"
    printf '%s\n' 'status' 'save psq_arena_try' \
        "changelevel $exit_map" 'wait' 'status' 'save psq_campaign_try' \
        'map PSQARENA'
    printf '%s\n' "$frames"
    printf '%s\n' 'save psq_upper_try' 'quit'
} > "$root/librequake/id1/psqcheck.cfg"

port=$((27000 + $$ % 900))
out="$root/engine.out"
"$exe" -dedicated 1 -port "$port" -psquake-root "$root" -condebug \
    +developer 1 +psq_debug 1 +exec psqcheck.cfg < /dev/null > "$out" 2>&1 &
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
expect "psq_arena is the controller, with the grid" "psq: arena grid 11 x 7 cells of 96"
grids=$(grep -cF "psq: arena grid 11 x 7 cells of 96" "$log" || true)
[ "$grids" -eq 2 ] || die "$grids controller starts, expected 2 (psqarena, loaded twice)"
if grep -q "No spawn function for" "$log"; then die "an entity has no spawn function"; fi
echo "arena_load_test: PASS every entity has a spawn function"
if grep -q '"psq_[a-z_]*" is not a field' "$log"; then die "a psq_ map key is not a QuakeC field"; fi
echo "arena_load_test: PASS the grid keys are QuakeC fields"
gated=$(grep -c "^psq: poll at .* gated" "$log" || true)
[ "$gated" -ge 2 ] || die "$gated gated polls, expected one per arena load: the controller never polled"
if grep -q "psq: spawned" "$log"; then die "a process monster spawned on a dedicated server"; fi
echo "arena_load_test: PASS no process monster on a dedicated server (gate shut)"
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
