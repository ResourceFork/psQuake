#!/bin/bash
#
# psQuake -- headless load of the arenas in the built app, along their exit
# chain to the first campaign map, then the test arena by name.
#
#   arena_load_test.sh <psQuake.app> <scratch root> <campaign map> <test arena> <arena>...
#
# Runs the engine as a dedicated server (no window, no input, UDP bound to
# 127.0.0.1 only on a per-run port) with -psquake-root in the build tree, so
# nothing is written to ~/Library/Application Support/psQuake. A config
# file in the scratch root loads the first <arena> (New Game's) and tries to
# save; changes level to each next <arena> in turn (the path each arena's
# exit takes) and tries to save there; changes level to <campaign map> and
# saves; loads the first arena again in capitals (the file system ignores
# case) and tries to save; and loads <test arena> by name and tries to save.
# The run passes when the console log shows:
#   - every arena loaded, each with its psq_arena entity spawned as the
#     process controller with a grid ("psq: arena grid C x R cells of 96");
#     every entity has a spawn function and the psq_ grid keys are QuakeC
#     fields (psq_grid entities included);
#   - no process monster spawned: a dedicated server keeps the process gate
#     shut, so this run of the real app lists and touches no process;
#   - the save refused in every arena (the arena rule names them all);
#   - <campaign map> loaded after the chain, and a save written there;
#   - no Host_Error, SV_Error or other engine error.
# A dedicated server has no player, so the spawn parms reset and the
# monsters themselves are covered by psq_headless_arena_* (fake processes).
#
set -euo pipefail

die() { echo "arena_load_test: FAIL: $*" >&2; exit 1; }

[ $# -ge 5 ] || die "usage: arena_load_test.sh <psQuake.app> <scratch root> <campaign map> <test arena> <arena>..."
app=$1
root=$2
campaign=$3
test_arena=$4
shift 4
chain=("$@")

for p in "$app" "$root"; do
    [ -n "$p" ] || die "empty path"
    case $p in /*) ;; *) die "not absolute: $p" ;; esac
    [ "$p" != / ] || die "refusing /"
done
case $root in */psq-arena-load-root) ;; *) die "scratch root must end in psq-arena-load-root: $root" ;; esac
case $root in "$HOME"/Library/*) die "refusing a root under ~/Library: $root" ;; esac
for m in "$campaign" "$test_arena" "${chain[@]}"; do
    case $m in *[!a-z0-9_]*|'') die "bad map name: $m" ;; esac
done
exe="$app/Contents/MacOS/psQuake"
[ -x "$exe" ] || die "no engine at $exe"
for m in "$test_arena" "${chain[@]}"; do
    [ -f "$app/Contents/Resources/id1/maps/$m.bsp" ] || die "the app has no $m.bsp"
done
first=${chain[0]}
upper=$(printf '%s' "$first" | tr '[:lower:]' '[:upper:]')

rm -rf "$root"
mkdir -p "$root/librequake/id1"
# The controller first thinks a second into the level: stay in each arena
# load for 60 frames (3 s at the dedicated server's 20 Hz) so it polls.
frames=$(printf 'wait\n%.0s' $(seq 1 60))
{
    printf '%s\n' "map $first" "$frames" 'status' "save psq_try_$first"
    for m in "${chain[@]:1}"; do
        printf '%s\n' "changelevel $m" "$frames" 'status' "save psq_try_$m"
    done
    printf '%s\n' "changelevel $campaign" 'wait' 'status' 'save psq_campaign_try'
    printf '%s\n' "map $upper" "$frames" 'save psq_upper_try'
    printf '%s\n' "map $test_arena" "$frames" 'status' "save psq_try_$test_arena" 'quit'
} > "$root/librequake/id1/psqcheck.cfg"

port=$((27000 + $$ % 900))
out="$root/engine.out"
"$exe" -dedicated 1 -port "$port" -psquake-root "$root" -condebug \
    +developer 1 +psq_debug 1 +exec psqcheck.cfg < /dev/null > "$out" 2>&1 &
pid=$!
for _ in $(seq 1 600); do
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
    die "the engine did not quit within 60 s"
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
loads=$(( ${#chain[@]} + 2 ))   # the chain, the first again in capitals, the test arena
for m in "${chain[@]}" "$test_arena"; do
    expect "$m loaded" "map:     $m"
done
grids=$(grep -cE "^psq: arena grid [0-9]+ x [0-9]+ cells of 96" "$log" || true)
[ "$grids" -eq "$loads" ] || die "$grids controller starts with a grid, expected $loads (one per arena load)"
echo "arena_load_test: PASS each arena's psq_arena is the controller, with a grid"
expect "the test arena keeps its 11 x 7 grid" "psq: arena grid 11 x 7 cells of 96"
if grep -q "No spawn function for" "$log"; then die "an entity has no spawn function"; fi
echo "arena_load_test: PASS every entity has a spawn function"
if grep -q '"psq_[a-z_]*" is not a field' "$log"; then die "a psq_ map key is not a QuakeC field"; fi
echo "arena_load_test: PASS the grid keys are QuakeC fields"
gated=$(grep -c "^psq: poll at .* gated" "$log" || true)
[ "$gated" -ge "$loads" ] || die "$gated gated polls, expected one per arena load: a controller never polled"
if grep -q "psq: spawned" "$log"; then die "a process monster spawned on a dedicated server"; fi
echo "arena_load_test: PASS no process monster on a dedicated server (gate shut)"
expect "campaign map loaded" "map:     $campaign"
expect "save allowed on the campaign map" "psq_campaign_try.sav"
[ -f "$root/librequake/id1/psq_campaign_try.sav" ] || die "no campaign save file"
for m in "${chain[@]}" "$test_arena"; do
    [ ! -e "$root/librequake/id1/psq_try_$m.sav" ] || die "a save was written in $m"
done
[ ! -e "$root/librequake/id1/psq_upper_try.sav" ] || die "a save was written in $upper"
refusals=$(grep -c "Can't save in the psQuake arena." "$log" || true)
[ "$refusals" -eq "$loads" ] || die "$refusals save refusals, expected $loads (every arena load)"
echo "arena_load_test: PASS save refused in every arena, $upper included"
if grep -E "Host_Error|SV_Error|QUAKE ERROR|cannot find map|couldn't spawn|Couldn't spawn" "$log" "$out"; then
    die "engine error in the log"
fi
pat="map: +($(IFS='|'; echo "${chain[*]}")|$campaign)"
order=$(grep -nE "$pat|psq_campaign_try.sav" "$log" | cut -d: -f2- | tr -s ' ' | cut -c1-40 |
    tr '\n' '|')
want=""
for m in "${chain[@]}" "$campaign"; do want="${want}map: $m|"; done
case $order in
    "${want}Saving game to "*) ;;
    *) die "steps out of order: $order (want ${want}Saving game ...)" ;;
esac
echo "arena_load_test: PASS (${chain[*]} in turn, then $campaign, save there; $test_arena by name)"
