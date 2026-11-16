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

As a host, psQuake must:

- never list, renice or signal a process itself. Summoner, bundled as
  `psQuake.app/Contents/Resources/summoner`, is the only path to real
  processes. `PSQUAKE_POLL_CMD` / `PSQUAKE_RESPOND_CMD` override it for
  development only. Do not add `kill`, `setpriority`, `sysctl` process reads,
  `proc_pid*` calls or a native backend to `src/` or `psproc/`:
  `psproc_no_native_process_access` is a tripwire for the obvious calls, not
  a proof. The one `kill()` is the script backend killing its own timed-out
  child process group.
- keep the plugin's order and filter nothing; send `limit=` and the Sort by
  choice; map `grade` onto the classes the data has (`psq_grade_rung`).
- send `post` on every wound and `delete` on every death, in every kill mode,
  with `mode=dryrun|post-only|delete` (`psq_policy_mode`). A process monster
  killed by the environment (lava, slime, drowning, a fall, a pit, a
  crusher) sends the same `delete`, with the mode, as a player kill.
- fail safe: with no Summoner, no `python3` or a failing poll there are no
  process monsters, the reason is shown, and nothing falls back.

## Gate

```sh
tools/setup-build.sh                 # once per checkout (git-ignored data/)
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

Run ctest serially (the headless tests share user roots). The Summoner tests
run inside it (`psq_summoner_tests`, `psproc_select_test`'s end-to-end part,
`psq_bundle_summoner`).

## Safety rules for any run

- Never signal or renice a real process in a test. The `ps` plugin may only be
  used read-only (`describe`, `list`, `get`) or in dry run. Real actions are
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
