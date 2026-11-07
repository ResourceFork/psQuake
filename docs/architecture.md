# psQuake architecture

psQuake turns the processes running on a Mac into Quake monsters. It reuses
psDoom's process layer and safety model on a different engine. This page is the
short map; the detailed plan and its tracking live on the project board.

## Layers

Process data flows through three layers, as in psDoom. Each can be reasoned
about and tested on its own.

```
OS (sysctl, setpriority, kill)      or a user script
        |
psproc backend   psproc/src/macos/proc_macos.c, proc_script.c, proc_fake.c
        |        behind the proc_backend_t vtable (list, current_uid,
        |        renice, kill)
psproc select    psproc/src/proc_select.c: filter (uid, self, launcher
        |        chain, session-critical names), rank, truncate.
        |        psproc_options.c: kill policy and CLI flags.
game             QuakeSpasm + QuakeC (phases 1-2): spawn, label, wound
                 (renice) and kill (SIGTERM, opt-in only)
```

The default kill policy is renice with no kill. Killing is opt-in
(`-psquake-live` or the options page).

## Repository layout

```
psproc/            process layer copied from psDoom 436ba83 (MIT)
  include/ src/ src/macos/ tests/ docs/script-backend.md
  UPSTREAM         psDoom commit psproc/ is synced to
tests/             psQuake's own host tests
tools/             psproc-drift.sh (report and re-import psDoom changes)
docs/              this file
```

Planned for later phases: `third_party/quakespasm`, `third_party/fteqcc`, the
LibreQuake data, `src/engine`, `src/core`, `src/app/macos`, `qc/`.

## Keeping psproc in step with psDoom

`psproc/` is a copy, not a submodule. psQuake's adaptations, all in commit
26d25ab:

- `psdoom_options.{c,h}` became `psproc_options.{c,h}`, and its `psdoom_*`
  identifiers became `psproc_*`. Only the engine-free part is kept, and it gains
  `psproc_options_sanitize()`.
- `psdoom_options_parse_args(void)` became
  `psproc_options_parse_args(argc, argv, prefix)`, giving `-psquake-*` flags.
- `proc_script_install_from_env(void)` became `(prefix)`, giving `PSQUAKE_*`
  variables.
- The imported test links the options module instead of a stub.
- `docs/script-backend.md` is retitled "psproc script backend" and gains a
  psQuake note.

`tools/psproc-drift.sh` holds the path map and the `psdoom_` to `psproc_`
rename filter. A fix to the shared layer is made in psDoom first and arrives
here as one re-import commit (`--apply <rev>`, then `--mark <rev>` if it left
work). The re-import 3-way merges psDoom's change into the psQuake
adaptations. When a second safety
fix has to reach both games, promote the layer to a shared repository consumed
by both.

## Phase plan

| Phase | Done when |
| --- | --- |
| 0 | Repo bootstrap; `psproc/` imported with provenance; CMake library and host tests green; drift check with re-import |
| 1 | QuakeSpasm 0.97.0 built by CMake into `psQuake.app` (arm64, bundled SDL2, no Cocoa launcher); data selection: bundled LibreQuake, or a registered `id1` the user supplies; UDP bound to loopback; mouse-grab fix with a predicate test |
| 2 | QuakeC builtins and hooks, relaxed precache, controller, morph, swarm; extracted engine-free core with host tests; PID identity guard re-imported from psDoom |
| 3 | Process labels drawn in the 2D pass (projection, occlusion, fade, PID toggle) |
| 4 | Arena generator and validator; New Game to the arena, exit to `lq_e1m1` or `e1m1` |
| 5 | Options page, cvars, live-kill warning, protected-list review; release `.app` |

Changes from the feasibility plan: psQuake ships LibreQuake data and never
loads its own content on id's shareware data, because id's shareware licence
forbids user levels. The scoped shareware exception is dropped. The PID
identity guard moved out of phase 0: it is built in psDoom first and re-imported.
