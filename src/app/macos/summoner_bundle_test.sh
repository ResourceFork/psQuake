#!/bin/bash
#
# The built psQuake.app carries Summoner: Contents/Resources/summoner/summon,
# its core and its built-in plugins (and not its tests), and that bundled copy
# runs as psQuake runs it (python3 -I -B), read-only: `describe`, `refresh`
# and `get` against the built-in `ps` plugin, with a scratch root and a
# scratch HOME. Nothing is acted on. Adapted from psDoom's
# tests/app_bundle_test.sh.
#
# Usage: summoner_bundle_test.sh <path to psQuake.app> <work dir>
set -euo pipefail

app=${1:?usage: summoner_bundle_test.sh APP WORK}
work=${2:?usage: summoner_bundle_test.sh APP WORK}
case "$work" in
    /*/summoner-bundle-test) ;;
    *) echo "refusing work dir $work"; exit 2 ;;
esac
res="$app/Contents/Resources/summoner"
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }

check "summon is bundled and executable" '[[ -x "$res/summon" ]]'
check "core is bundled"                  '[[ -f "$res/summoner.py" ]]'
check "ps plugin is bundled"             '[[ -f "$res/plugins/processes.py" ]]'
check "csv plugin is bundled"            '[[ -f "$res/plugins/csv_rows.py" ]]'
check "tests are not bundled"            '[[ ! -e "$res/tests" ]]'
check "UPSTREAM is not bundled"          '[[ ! -e "$res/UPSTREAM" ]]'
check "engine sits in MacOS"             '[[ -x "$app/Contents/MacOS/psQuake" ]]'
check "the engine knows the bundled path" \
    'strings -a "$app/Contents/MacOS/psQuake" | grep -q "/Resources/summoner/summon"'
check "the engine installs it at startup (its log line is linked)" \
    'strings -a "$app/Contents/MacOS/psQuake" | grep -q "process monsters: from the bundled process list"'

py=""
for c in /opt/homebrew/bin/python3 /usr/local/bin/python3 \
         /Library/Developer/CommandLineTools/usr/bin/python3 \
         /Applications/Xcode.app/Contents/Developer/usr/bin/python3; do
    if [[ -x "$c" ]]; then py=$c; break; fi
done
if [[ -z "$py" ]]; then
    echo "skip run checks: no python3 on this host (psQuake would show no process monsters)"
else
    rm -rf "$work"
    mkdir -p "$work/home"
    out=$(env -u SUMMONER_ROOT HOME="$work/home" \
          "$py" -I -B "$res/summon" --root "$work/root" describe)
    check "bundled summon describes the ps plugin" '[[ "$out" == *relevance* ]]'
    env -u SUMMONER_ROOT HOME="$work/home" "$py" -I -B "$res/summon" --root "$work/root" refresh
    out=$(env -u SUMMONER_ROOT HOME="$work/home" \
          "$py" -I -B "$res/summon" --root "$work/root" get)
    check "bundled summon lists processes" '[[ "$out" == *"# dim id=relevance"* && "$out" == *"id="* ]]'
    check "nothing written to the scratch home" '[[ ! -e "$work/home/.config" ]]'
    check "no __pycache__ in the app" '[[ ! -e "$res/__pycache__" && ! -e "$res/plugins/__pycache__" ]]'
    check "the app signature still holds" 'codesign --verify --strict "$app" 2>/dev/null'
    rm -rf "$work"
fi
exit $fail
