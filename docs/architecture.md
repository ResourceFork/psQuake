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
(`psq_killpolicy 2`; a `-psquake-live` flag and the options page are phase 5).

## Repository layout

```
psproc/            process layer copied from psDoom 436ba83 (MIT)
  include/ src/ src/macos/ tests/ docs/script-backend.md
  UPSTREAM         psDoom commit psproc/ is synced to
third_party/       vendored code (GPL-2 engine and its libraries); README.md
  quakespasm/      QuakeSpasm 0.97.0 subset with [psQuake] hooks
  fteqcc/          FTE's QuakeC compiler (build tool only, never shipped)
  librequake-qc/   LibreQuake v0.09-beta QuakeC with [psQuake] hooks
  licenses/        libogg and libvorbis licence texts
src/
  core/            psq_policy: the engine-free process-monster policy
  engine/          psQuake's engine-side code: psq_data, psq_net, psq_grab
                   (pure, host-tested, also in psq_core), psq_sys,
                   psq_macos and psq_builtins (engine glue), psq_engine.h
                   (hook prototypes)
  qc/              psq_defs.qc, psq.qc (the controller), psquake_qc.cmake
  app/macos/       psquake_app.cmake, Info.plist.in, stage_codecs.sh,
                   assemble_bundle.sh, psq_headless_test.cmake
tests/             psQuake's own host tests
tools/             psproc-drift.sh; fetch-librequake.sh (LibreQuake into data/)
data/              git-ignored: data/librequake from the fetch script
docs/              this file
```

The build compiles `psq_fteqcc`, then the QuakeC into `build/qc/progs.dat`,
which the bundle carries as `Contents/Resources/psquake/progs.dat`.

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

## Phase 1: the engine

QuakeSpasm 0.97.0 is vendored pristine in its own commit, and every psQuake
change to it is marked `[psQuake]` and listed in
[third_party/README.md](../third_party/README.md). The engine calls into
psQuake code at a handful of hooks. The decisions behind them are pure C in
`src/engine` (`psq_core`), host-tested without the engine.

**Bundle.** `psQuake.app/Contents/MacOS/psQuake` is the engine itself; the
Cocoa launcher is not built and `main_sdl.c`'s `main` is the entry point.
`Contents/Frameworks` holds the vendored SDL 2.0.22 framework and the Ogg
Vorbis dylibs, thinned to arm64. The app is arm64 only: SDL's vendored x86_64
slice has 8 bytes of header room, and the 16-byte signature command would
overwrite its code, so `assemble_bundle.sh` refuses to sign any slice
without room. Every non-system load
command is `@executable_path/../Frameworks/...` and there is no `LC_RPATH`.
`assemble_bundle.sh` runs on every build: it copies the frameworks, licences
and LibreQuake data, then ad-hoc signs the frameworks, the dylibs and the app,
and checks the signature.

**Data and the user root** (`psq_data.c`, glue in `psq_sys.c`, hook in
`Sys_Init`). The user root is `~/Library/Application Support/psQuake` (the
home comes from the user database, not `$HOME`), or `-psquake-root DIR`, which
must be absolute. The engine's `basedir` is where `id1` is read from and its
`userdir` is where every write lands:

| Data | basedir | userdir |
| --- | --- | --- |
| Registered Quake (`<root>/id1/pak0.pak` with `pak1.pak`, or with `gfx/pop.lmp` inside) | `<root>` | `<root>` (same pointer, so `id1` is searched once) |
| LibreQuake in the bundle | `Contents/Resources` | `<root>/librequake` |
| Neither | the engine stops with a message naming both options | |

Shareware (`pak0.pak`, no `pak1.pak`, no `gfx/pop.lmp`) is never selected.
`gfx/pop.lmp` is the engine's own registered test; `psq_sys.c` finds it by
reading only the pak header and directory, validated by `psq_pak_header`.
Upstream's `-basedir` is refused, since it would move reads and writes out
of the user root. With LibreQuake, the user's
`<root>/id1` is not on the search path at all, so shareware or partial paks
there cannot mix in. `qconsole.log` moved from `basedir` to `userdir`; config,
saves, screenshots, demos and `history.txt` already used `com_gamedir` or
`userdir`.

**Network** (`psq_net.c`, hook in `net_udp.c`). Every UDP socket binds
127.0.0.1 unless `-psquake-bind ADDR` gives a dotted IPv4 address
(`0.0.0.0` for every interface). The address is parsed before any socket
opens; a missing or malformed value stops the engine, and a socket opened
before parsing would bind loopback.

**Mouse grab** (`psq_grab.c`, hook in `in_sdl.c`). Upstream frees the cursor
for the menu and console only in windowed mode: every entry calls
`IN_Deactivate(modestate == MS_WINDOWED)`. psQuake keeps those calls and adds
`IN_UpdateGrab()` after each frame's events. It compares
`psq_grab_wanted()` (grab only in play or chat, with the window focused) with
what was last applied and makes the one call needed. So any path that changes
`key_dest` releases or re-grabs on the same frame, windowed or fullscreen.
The key-binding menu keeps the mouse captured while it waits for a key
(`M_PSQ_BindingKey`), as upstream's `IN_Activate` there intends.

**Window** (`gl_vidsdl.c`, `psq_macos.m`). Fullscreen defaults to SDL desktop
fullscreen (`vid_desktopfullscreen 1`), a macOS Space with no mode switch.
Alt-Enter and the video menu toggle it through upstream's `vid_restart` path.
The window is resizable (minimum 320x200); `VID_PSQ_WindowSizeChanged()`
follows each size change, updates `modestate`, and syncs
`vid_width`/`vid_height`/`vid_fullscreen` into `config.cfg`, which upstream
already writes on quit and reads before the window opens. The GLSL gamma
texture is reallocated when the size changes (`gl_rmain.c`). SDL 2.0.22 never
updates its fullscreen flag when macOS itself enters or leaves the Space (the
green title-bar button, Ctrl-Cmd-F), so `psq_macos.m` observes the window's
did-enter and did-exit notifications and sets SDL's flag to match; SDL then
answers "already there" instead of starting a second transition.

## Phase 2: process monsters

C decides, QuakeC moves entities. QuakeC reaches processes only through
the builtins #1200-#1208 (`src/engine/psq_builtins.c`), which forward to
`src/core/psq_policy.c` (#1209-#1210 only read the arena grid, below):

- **Gate.** Every builtin that lists or acts is a no-op unless the server is
  active with one client, not deathmatch or coop, and not dedicated.
- **Pool.** `psq_poll` refreshes at most once a second from
  `psd_select_collect` (triage, protected names, self and launcher chain
  already excluded), minus processes killed this level and pids above 2^24
  (a QuakeC float would round them). QuakeC names processes only by pid, and
  the policy acts on a pid only while it is in the latest pool, as the
  identity (pid, start token, name) the pool recorded. A pid that changed
  hands is held out for one poll so its old monster retires first.
- **Actions.** `psq_wound` renices by 4 (renice and live policies);
  `psq_kill` remembers the kill for the level and sends SIGTERM only under
  the live policy. Both go through `psd_proc_*_checked`; a gone or reused
  result marks the process gone, so its monster teleports out.
- **Ladder.** dog, army, enforcer, knight, ogre, hell knight, fiend, vore,
  shambler by memory (or CPU with `psq_classify 1`); a class whose model the
  data lacks gives its range to the next lighter one. Promote after 2 polls,
  demote after 5.
- **Swarm.** A parent whose child count jumps by 5 in a poll owes up to 6
  Scrags (`psq_pid -1`, acting on nothing), at most 24 alive, then a 4-poll
  cooldown.

The controller (`src/qc/psq.qc`) is a `psq_arena` entity: the arena map
places one, carrying the grid (phase 4); `psq_testarena 1` makes worldspawn
create one on any other map (on the arena it takes the map's grid, and the
map's entity removes itself). It precaches every available class while the
level loads (the relaxed precache in `pr_cmds.c` lets a cached name pass
afterwards), then each second retires, spawns (up to `psq_cap` live), morphs
and swarms. A new process monster teleports onto a free grid cell: the
engine (`psq_grid_cell`, `src/engine/psq_grid.c`, host-tested) names free
cells from a random start, one monster per cell, so the hull 2 classes always
get a whole cell, and no cell a solid entity reaches into or within 32 units
of the player; QuakeC then tests the spot for walls and floor and asks for
the next cell when it fails. With the grid full, or no grid (another map),
it is placed near the player, in sight, as before. Swarm Scrags still appear
around their parent. Hooks: `T_Damage` calls `psq_wound` for a survivor,
`Killed` calls `psq_died` before `th_die`. A killed process monster leaves an
ordinary corpse (`psq_pid -3`); one whose process left the list teleports
out and leaves the level total.

Process monsters are never saved. One rule (`psq_save_rule_for`,
`src/engine/psq_arena.c`, host-tested) serves `save`, quick save and the Save
menu: refused in the arena whatever it holds ("Can't save in the psQuake
arena."), refused on any other level the controller has run on or that holds
a process tag ("Can't save here: process monsters exist only while their
processes run."), else allowed. Each refusal prints one line; the menu also
shows it in a box. A loaded game has every `psq_pid` cleared. Leaving a level
frees its edicts, so no process monster reaches `lq_e1m1`, and the next
level's reset (`PSQ_LevelReset`) ends the process state, so saves work
there.

`psq_headless_test` links the engine with `psproc_fake` only and drives the
whole path on `lq_e1m1` as a dedicated loopback server, under each kill
policy and with the gate closed; `psq_headless_arena_*` does the same from
New Game in the arena (grid cells, the grid-full fallback, the save refusal,
the exit's parms reset and `lq_e1m1` afterwards).

## Phase 4: the arena

`tools/generate_arena.py` writes `psqarena.map` (Quake standard format,
axis-aligned box brushes) and `psqarena.wad` (miptex copied from LibreQuake's
maps in `pak0.pak`, each pinned by SHA-256). `tools/compile_arena.py` runs it
and ericw-tools `qbsp`, `vis` and `light`; `tools/validate_bsp.py` reads the
BSP with no compiler code and fails the build on any check. The build bundles
only a validated map, as a loose `id1/maps/psqarena.bsp` beside LibreQuake's
paks. `tools/setup-build.sh` provides LibreQuake and the compilers.

Layout: a south antechamber (player start, four `trigger_setskill` pads
across its width), the hall (interior 1536 x 1152 x 256; an 11 x 7 grid of
96-unit cells, 1056 x 672, centred on the origin; pickups and pillars in the
margins), and a north alcove with the exit, `trigger_changelevel` to
`lq_e1m1` with spawnflag 1 (no intermission).

The grid is described for the process controller by one point entity:

| Key | Value | Meaning |
| --- | --- | --- |
| `classname` | `psq_arena` | the process controller (`src/qc/psq.qc`) |
| `origin` | `0 0 0` | grid centre, on the floor (monsters stand 24 units above it) |
| `psq_cols` | `11` | cells along x |
| `psq_rows` | `7` | cells along y |
| `psq_cell` | `96` | cell size; fits hull 2 (64 x 64) |

Cell (c, r) is centred at `origin + ((c - 5) * 96, (r - 3) * 96, 0)`; cell
index i is column i % 11, row i / 11. The keys are `.float` fields in
`src/qc/psq_defs.qc`.

Engine rules (`src/engine/psq_arena.c`, host-tested): New Game opens the arena
when it exists; leaving it runs `SetNewParms` in `SV_SaveSpawnparms`, so the
campaign starts as a new game would, whatever progs.dat runs; `save` is
refused there (the save rule above).

## Phase plan

| Phase | Done when |
| --- | --- |
| 0 | Repo bootstrap; `psproc/` imported with provenance; CMake library and host tests green; drift check with re-import |
| 1 | QuakeSpasm 0.97.0 built by CMake into `psQuake.app` (arm64, bundled SDL2, no Cocoa launcher); data selection: bundled LibreQuake, or a registered `id1` the user supplies; UDP bound to loopback; mouse-grab fix with a predicate test |
| 2 | QuakeC builtins and hooks, relaxed precache, controller, morph, swarm; extracted engine-free core with host tests; PID identity guard re-imported from psDoom |
| 3 | Process labels drawn in the 2D pass (projection, occlusion, fade, PID toggle) |
| 4 | Arena generator and validator; New Game to the arena, exit to `lq_e1m1` with parms reset; no saves in the arena |
| 5 | Options page, cvars, live-kill warning, protected-list review; release `.app` |

Changes from the feasibility plan: psQuake ships LibreQuake data and never
loads its own content on id's shareware data, because id's shareware licence
forbids user levels. The scoped shareware exception is dropped. The PID
identity guard moved out of phase 0: it is built in psDoom first and re-imported.
