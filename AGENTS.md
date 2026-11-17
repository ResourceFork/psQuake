# Agent guide: psQuake

## Read first: the specs live in psDoom

psQuake is a host of psDoom's script contract and Summoner. The normative specs
are psDoom's, at the commit in [psproc/UPSTREAM](psproc/UPSTREAM):
`docs/spec/README.md`, `script-contract.md`, `summoner.md` and
`process-access.md` in [ResourceFork/psDoom](https://github.com/ResourceFork/psDoom)
(read them with `git -C ../psDoom show $(cat psproc/UPSTREAM):docs/spec/README.md`).
They are not copied here. Where psQuake's text and a spec differ, the spec
decides and psQuake's text is the bug. The copies that travel with this repo:

- [psproc/docs/script-backend.md](psproc/docs/script-backend.md): the full
  script contract (imported with psproc/, `PSQUAKE_*` names).
- [summoner/README.md](summoner/README.md): Summoner, copied whole from psDoom
  at [summoner/UPSTREAM](summoner/UPSTREAM).

Where psQuake meets each spec:

- process access: no native backend; the bundled Summoner is
  `psQuake.app/Contents/Resources/summoner` (`PSQ_InstallBundledSummoner`,
  `src/engine/psq_builtins.c`); `PSQUAKE_POLL_CMD` / `PSQUAKE_RESPOND_CMD`
  override it for development. `psproc_no_native_process_access` is the
  tripwire.
- script contract: `psq_policy_mode` (the kill mode as `mode=`),
  `psq_grade_rung` (grade to class), the Sort by row (`src/engine/psq_menu.c`).
- psQuake's own rule: a process monster the environment kills (lava, slime,
  drowning, a fall, a pit, a crusher) sends the same `delete`, with the mode,
  as a player kill (QuakeC `psq_died`; tested in `psq_policy_test` and the
  `psq_headless_hazards_*` runs).

## Gate

```sh
tools/setup-build.sh                 # once per checkout (git-ignored data/)
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

Run ctest serially (the headless tests share user roots). The Summoner tests
run inside it (`psq_summoner_tests`, `psproc_select_test`'s end-to-end part,
`psq_bundle_summoner`).

## Safety rules for any run

- Never signal or renice a real process in a test. The built-in `ps` plugin
  acts by default (it follows the host mode), so it may only be used
  read-only (`describe`, `list`, `get`) or with `mode=dryrun`. Real actions are
  tested through the fake backend, a respond recorder the test writes, or the
  CSV plugin on a CSV the test created.
- Every Summoner run sets an explicit scratch root (`--root` under `build/`)
  and a scratch `HOME`. Never use the real `~/.config/summoner`, and never
  trust an inherited `SUMMONER_ROOT`.
- Do not launch `psQuake.app` on the user's screen. The headless and
  offscreen render tests need no window.

## Changing the shared parts

`psproc/` and `summoner/` are copies of psDoom's. Fix them in psDoom first,
then re-import: `tools/psproc-drift.sh --apply <rev>` for psproc/ (see
[docs/architecture.md](docs/architecture.md)), and a whole-folder copy of
psDoom's `summoner/` at `<rev>` (tracked files only) with
`summoner/UPSTREAM` set to it. Changing the script contract or Summoner's
plugin contract needs the owner's go.
