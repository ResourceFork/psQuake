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

The kill mode is renice with no kill at every launch. Simulate and live
kill (SIGTERM) are chosen on Options > psQuake Options for that session only
(phase 5, below); nothing saves the kill mode and no flag sets it.

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
  NOTICES.txt      the app's third-party notices (shipped in Licenses/)
src/
  core/            psq_policy: the engine-free process-monster policy, the
                   trust guard and the kill-mode rule
  engine/          psQuake's engine-side code. Pure and host-tested (also in
                   psq_core): psq_data, psq_net, psq_grab, psq_arena,
                   psq_grid, psq_labels. Engine glue: psq_sys, psq_macos,
                   psq_builtins (builtins, cvars, kill mode),
                   psq_labels_draw, psq_menu (Options > psQuake),
                   psq_engine.h (hook prototypes)
  qc/              psq_defs.qc, psq.qc (the controller), psquake_qc.cmake
  app/macos/       psquake_app.cmake, Info.plist.in, stage_codecs.sh,
                   assemble_bundle.sh, check_bundle.sh,
                   psq_headless_test.cmake, psq_labels_render.cmake
tests/             psQuake's host, headless and offscreen-render tests
tools/             setup-build.sh and fetch-librequake.sh (LibreQuake and
                   the map compilers into data/); generate_arena.py,
                   compile_arena.py, validate_bsp.py, arena.cmake (the
                   arena); psproc-drift.sh; package-release.sh (the zip)
data/              git-ignored: data/librequake, data/ericw-tools
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
  active with one client, not deathmatch or coop, and not dedicated, and the
  level is the arena or `psq_testarena` is on in a trusted session
  (`psq_level_allowed`): a `psq_arena` entity that another map or a crafted
  save brings stays inert.
- **Trust.** Once a remote server or a demo stuffs commands (anything but
  comment-only text, `psq_stuff_text_inert`: no `;` and every line empty
  or `//`; LibreQuake's attract demos stuff `//st 15 0` behind the main
  menu at every launch), or QuakeC's
  `localcmd`/`stuffcmd` sends anything but the stock game code's own
  commands (`psq_qc_command_trusted`: `bf`, `-attack`, `restart`), the
  session is untrusted until restart: the kill mode can only go down,
  `psq_allusers` and `psq_infighting` only off, and `psq_testarena` no
  longer opens the gate. QuakeC's `cvar_set` may never touch those four.
  QuakeC's `changelevel`, whose name is usually a map's own `map` key and
  goes into the console buffer, is refused for anything but a plain map
  name (`psq_map_name_plain`: no `;`, quote, space or control character),
  and the session is then marked the same way.
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
the next cell when it fails. A map may add further grids as `psq_grid`
entities with the same keys (the multi-level arenas put one on each floor
region): QuakeC picks a grid by its share of all the cells, passes a full
one to the next, and numbers `psq_gridcell` across them in map order (on a
one-grid map it is that grid's cell). Grids never overlap in plan, so one
floor's monsters never take another floor's cells (an entity on a bridge less
than 128 units above a cell, as in Towers, can still hold it while it stands
there). With every grid full, or no grid (another map),
it is placed near the player, in sight, as before. Swarm Scrags still appear
around their parent. Hooks: `T_Damage` calls `psq_wound` for a survivor,
`Killed` calls `psq_died` before `th_die`. A killed process monster leaves an
ordinary corpse (`psq_pid -3`); one whose process left the list teleports
out and leaves the level total.

Infighting is the user's option, `psq_infighting` (archived, default 0).
At 0 a monster's attack on a process monster or Scrag, or theirs on another
monster, does no damage: they may still turn on each other, but only the
player wounds and kills process monsters. At 1 they hurt and can kill each
other, and the policy acts on those processes as for the player's hits
(still only where the gate is open). Ordinary monsters keep stock
infighting; lava, slime and crushers still count. After a server, demo or
game code stuffs commands it can only be turned off, and QuakeC can never
set it.

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
policy and with the gate closed (two clients; a stray controller; an
untrusted test arena), and checks that QuakeC cannot raise the policy;
`psq_headless_arena_*` does the same from
New Game in the arena (grid cells, the grid-full fallback, the save refusal,
the exit's parms reset and `lq_e1m1` afterwards).

## Phase 3: process labels

`SCR_DrawProcessLabels` (`src/engine/psq_labels_draw.c`) runs in
`SCR_UpdateScreen`'s 2D pass right after the crosshair. It draws each live
process monster's name, and its PID above the name when `psq_showpid` is on,
just above the top of the frame the monster shows. It draws nothing unless a
local server runs the level the client shows: not in demo playback, an
intermission or finale, or while a menu is open, and not with `psq_labels 0`.

- Which monsters: the client's visible entities, whose index is the server
  edict. Only `psq_pid` (offset from `GetEdictFieldValue`, bounds-checked)
  and `health` are read from the edict. The name comes from the process
  policy's own sanitized pool entry for that pid, never from a QuakeC string,
  and is sanitized again for drawing. Swarm members (`psq_pid` -1), corpses
  and monsters whose pid left the poll get none.
- The arena gate: each frame the pass asks `PSQ_ProcessGateAllows()`, the
  predicate every builtin sets the policy's gate from, so on a level where
  process monsters may not act (any map but the arena, unless
  `psq_testarena` is on in a trusted session) nothing is labelled, even
  while paused or with the console down, when no builtin runs.
- Hidden: outside the frustum (the renderer's own cull), anchor beside the
  view (left or right of it; the box can still graze the frustum) or behind
  the eye, or both the anchor and the monster's centre cut off from the eye
  by the world or a solid brush model such as a door (`SV_Move`). An anchor
  above or below the view is kept and the label pinned to that edge: that
  monster is right there, filling the view.
- Fade: `psq_labelrange` 0-2 fade out between three quarters of and the
  full 640, 1280 or 3584 units (psDoom's cut-offs); 3 never fades.
- Layout: the status bar's scale, centred, kept inside the view; nearer
  labels keep their place and farther ones move up out of their way, or are
  dropped after three block heights.

The maths is engine-free (`src/engine/psq_labels.c`, `tests/psq_labels_test.c`).
`psq_labels_render` is the real engine and renderer linked with
`psproc_fake` and an offscreen SDL video layer (`tests/psq_offscreen_sdl.c`:
a windowless CGL context and framebuffer object, no window or
NSApplication, IOKit mouse calls stubbed to fail). It stages fake process
monsters on `lq_e1m1`, checks the label pass frame by frame at several
window sizes and fullscreen, through a real intermission and a recorded
demo, and writes a PNG of each frame to its user root.
`psq_labels_render_arena` runs the same binary on the test arena,
`psqtesthall` (loaded by name; New Game is checked to open `psqarena`): the six
monsters on their own grid cells, then moved to fixed cells (one just off
the right edge: at 1280x720 its box reaches into the view and the anchor
rule drops its label; the narrower 16:10 view culls the whole box),
labelled at 1280x720 and 2560x1600 fullscreen with and without PIDs;
then the console's `changelevel lq_e1m1`, where a level monster given a
listed pid's tag stays unlabelled. Without an arena it reports Skipped.
`psq_arena_shots_<arena>` runs it with `--arena-shots <arena>` for each arena
of the exit chain: each fake process monster must arrive alone on a cell
centre of one of the grids; then frames from the stands `generate_arena.py
--shots` writes, and two logged chases.

## Phase 4: the arenas

`tools/generate_arena.py --arena <name>` writes an arena's map (Quake
standard format: box brushes, plus ramp brushes with sloped faces) and its
WAD (miptex copied from LibreQuake's own maps in `pak0.pak`, each pinned by
SHA-256). `tools/compile_arena.py` runs it and ericw-tools `qbsp`, `vis` and
`light`; `tools/validate_bsp.py` reads the BSP with no compiler code and
fails the build on any check. `tools/arena.cmake` builds every arena and the
build bundles only validated maps, as loose `id1/maps/<name>.bsp` files,
each with its coloured lightmap `<name>.lit`, beside LibreQuake's paks. `tools/setup-build.sh` provides LibreQuake and the
compilers.

The arenas (`ARENAS` in the generator):

| Map | Layout | Exit leads to |
| --- | --- | --- |
| `psqarena` | the Pit: a ring at 0 round a pit at -128, balconies at 128 with a stair up each | `psqterraces` |
| `psqterraces` | three tiers at 0, 96 and 192 | `psqtowers` |
| `psqtowers` | an open court, towers at 192, a plinth at 96 | `lq_e1m1` |
| `psqtesthall` | the original one-floor hall, for the tests | `lq_e1m1` |

New Game opens `psqarena`; no exit leads to `psqtesthall`. The three
shipped arenas share a start room and an exit (the test hall keeps the old
antechamber with floor pads, and an open alcove):

- the start room (`START_ROOM`, 832 x 704): the player start at its south
  end; an arch wall with four arches, each a `psq_skill_arch` (0 to 3,
  left to right: `psq.qc`'s `trigger_setskill` that also tells the
  player, once per choice, what the skill changes) and its name above it
  in LibreQuake's 16 x 16 font textures (one box per letter, on the texture
  grid); a clear space after them; a 32-unit lip in the doorway to the
  hall, which the player jumps and a walking monster (steps of 18) cannot
  climb;
- the escape (`ESC_*`, each layout's `balcony`): a hallway from the clear
  space through the room's east wall, round the hall's south-east corner
  and up a stair outside its east wall, to a balcony set into that wall
  behind a window, 128 to 192 over the floor below (out of every climb,
  fiend leap and melee reach; no fall damage), 769 or more from the hall
  entry, with low cover at the window's ends. `psq_safe` zones cover it as
  they cover the room;
- the exit: a 320-deep north alcove (at the level of the top tier for
  Terraces), a `trigger_changelevel` with spawnflag 1 (no intermission) at
  its back, behind `psq_exit_door`, a stock `func_door` with health 1 that
  stays open. Its `th_die` (`psq_exit_door_die` in `psq.qc`) opens it only
  when `damage_attacker` is the player: a process monster's shot leaves it
  shut.

Worldspawn `_psq_rules` (`start-room exit-door balcony`) and
`_psq_hall_entry` tell `validate_bsp.py` to check them (`startroom`, with
the balcony's drop and leap rules, and `exitdoor`) and to walk reach from
the hall entry; the engine skips keys that begin with `_`.
The multi-level arenas have their own texture themes (banded walls, a floor
and spawn-area inlay per level, trims), hazard bands on raised floors'
edges, low cover (40 units: the player's eye, 46 up, sees over it) and
crates (64), railings on the raised floors' edges (`_Builder.rail`: a kick
plate, slim posts, a mid and a top rail, and a `clip` brush over the whole
footprint, so the walking hulls meet the same solid block (the 32-unit ones
a jump for the player and a wall for monsters), while shots and sight pass between the
rails), pillar bases and caps, ceiling lamps and wall light panels with
light entities, and LibreQuake's wall torches and flames, each two lights: a bright, short-throw yellow core (torch 340, flame 460) and a dimmer, medium-throw red-orange glow (170, 230; Terraces 1.35 times both),
each a warm light of its own (`LIGHT_FIRE_COLOR`; every other light is
white). ericw-tools' `light -lit` writes each arena's coloured lightmap,
`<arena>.lit`, which the app bundles beside the `.bsp` in `id1/maps` and
`check_bundle.sh --release` requires: a QLIT version 1, three bytes for
each of the map's lightmap bytes, or the engine drops it and the arena
renders white. `validate_bsp.py`'s `firelight` check holds each fire to a
warm light on the face just in front of it: within a luxel of the spot
behind it, red less blue (what the fire adds over the white lamps) of at
least 64, and at least 0.3 of the red. Framed textures (`FRAMED`: light
panels, lamps, the exit door, crates, the teleporter's pad and frame) are
fitted to their faces with offsets and a scale, so each shows its picture
whole from the face's corner; a room's floor fits to the room. Decoration stays
off the grids and out of the walking hulls' way; the validator's grid and
reach checks hold it to that.

Spawn grids are point entities. The first is the controller itself:

| Key | Value | Meaning |
| --- | --- | --- |
| `classname` | `psq_arena` | the process controller (`src/qc/psq.qc`) |
| `origin` | e.g. `0 0 -128` | grid centre, on the floor (monsters stand 24 units above it) |
| `psq_cols` | e.g. `5` | cells along x |
| `psq_rows` | e.g. `5` | cells along y |
| `psq_cell` | `96` | cell size; fits hull 2 (64 x 64) |

Each further grid is a `psq_grid` entity with the same keys, one per floor
region; no two overlap in plan. Cell (c, r) of a grid is centred at
`origin + ((c - (cols - 1) / 2) * 96, (r - (rows - 1) / 2) * 96, 0)`, index i
being column i % cols, row i / cols; `psq_gridcell` numbers the cells of
every grid in turn, the controller's first. The keys are `.float` fields in
`src/qc/psq_defs.qc`. The test hall has one 11 x 7 grid centred on the
origin.

The validator's `reach` check floods a 32-unit lattice of hull-2 stand
points from the player start, joining neighbours that differ by at most a
step (18 units) with no wall between, and fails on any clear cell it does
not reach: a spawn pocket a walking monster could not leave.

Engine rules (`src/engine/psq_arena.c`, host-tested): New Game opens
`psqarena` when it exists; every arena (`psq_arena_maps`: `psqarena`,
`psqterraces`, `psqtowers`, `psqtesthall`, any case, with or without `.bsp`)
runs `SetNewParms` in `SV_SaveSpawnparms` when left, so the next arena and the
campaign start as a new game would, whatever progs.dat runs; `save` is
refused in every arena (the save rule above).

## Phase 5: options, safety, packaging

**Options page** (`src/engine/psq_menu.c`; `menu.c` adds the Options entry
and hands it three menu states). Rows: kill mode, max live monsters (a
slider, 5 to 35, default 6), monster size by (memory or CPU), monster
infighting, target all users, process labels, show PID, label distance, and
a credits view naming every component and its licence. A box under the rows explains the row under the
cursor. Every row but the kill mode sets an archived cvar (`psq_cap`,
`psq_classify`, `psq_infighting`, `psq_allusers`, `psq_labels`,
`psq_showpid`, `psq_labelrange`), so they come back at the next launch.

**Kill mode.** `psq_builtins.c` holds it, starting at renice on every
launch, and `PSQ_SetKillMode` is the only thing that changes it. It asks
`psq_killmode_change` (`src/core/psq_policy.c`, host-tested): lowering is
always allowed; raising is refused once the session is untrusted (above);
raising to live needs `live_confirmed`. The page's kill row cycles simulate,
renice, live; moving to live opens the live-kill warning, which turns on
text input and passes `live_confirmed` only for a typed Y (the character,
so it follows the keyboard layout, as the quit prompt does); any other
typed character and every key that types none (Escape, Enter, arrows,
mouse and gamepad buttons) cancel. Printable keys whose character never
arrives (Cmd or Ctrl held, a layout typing non-ASCII) leave it up, and F1
to F12 run their bindings as in every menu; none of these confirms. The
warning adds a line each when Monster infighting or Target all users is
on. `psq_killpolicy` is an unarchived, read-only mirror for
the console and QuakeC: any change to it that did not come from
`PSQ_SetKillMode`, from the console, `config.cfg`, `autoexec.cfg`, a
`+psq_killpolicy` on the command line, a server, a demo or QuakeC, is put
back with one console line. psproc's `-psquake-live`-style flags are not
parsed (`psproc_options_parse_args` is never called).

Tests: `psq_policy_test` covers the rule exhaustively; the headless runs set
the mode with the test-only `psq_test_killmode` (a dedicated server has no
menu) and check that the console and the untrusted page cannot raise it;
`psq_options_render` and `psq_options_restart` drive the real page through
`Key_Event` and `Char_Event` offscreen, across a restart on one user root
(`build/headless/root-render-options`, frames `options-*.png`);
`psq_options_demo` makes a normal launch (no `+map`, the attract demo
playing) and chooses live kill over it; `psq_options_scale` frames the page,
the warning and the credits at Options > Scale 3 at 960x660 and 800x600,
where they stay inside the menu's 320 units (the stock Options menu's slider
readouts do not, as upstream). `psq_bundle_check` fails if a `psq_test_`
or `psq_render_` command is in the app's engine, and
`psq_bundle_check_catches_*` prove that check fails on binaries that have
them.

**Packaging.** `assemble_bundle.sh` copies every licence text into
`Contents/Resources/Licenses`, with `third_party/NOTICES.txt` and a
`SOURCE.txt` naming the commit built. `check_bundle.sh [--release]` checks
an app read-only (arm64, bundled libraries only, ad-hoc signature with no
entitlements, the licences, no test commands; with `--release` also the
LibreQuake files against their manifest and the arena; with
`--clean-source`, which the packaging script passes, a `SOURCE.txt` naming a
clean commit, so a tree with changes cannot be packaged); ctest runs it as
`psq_bundle_check`. `tools/package-release.sh` builds `build-release`
(`CMAKE_BUILD_TYPE=Release`), checks it and writes
`build-release/dist/psQuake-<version>-arm64.zip` with its SHA-256, a file
list and the check report. No Developer ID signing or notarization.

## Phase plan

| Phase | Done when |
| --- | --- |
| 0 | Repo bootstrap; `psproc/` imported with provenance; CMake library and host tests green; drift check with re-import |
| 1 | QuakeSpasm 0.97.0 built by CMake into `psQuake.app` (arm64, bundled SDL2, no Cocoa launcher); data selection: bundled LibreQuake, or a registered `id1` the user supplies; UDP bound to loopback; mouse-grab fix with a predicate test |
| 2 | QuakeC builtins and hooks, relaxed precache, controller, morph, swarm; extracted engine-free core with host tests; PID identity guard re-imported from psDoom |
| 3 | Process labels drawn in the 2D pass (projection, occlusion, fade, PID toggle) |
| 4 | Arena generator and validator; New Game to the arena, exit to `lq_e1m1` with parms reset; no saves in the arena |
| 5 | Options page with the live-kill warning; the kill mode never saved; safety review; release `.app` and zip with every notice |

Changes from the feasibility plan: psQuake ships LibreQuake data and never
loads its own content on id's shareware data, because id's shareware licence
forbids user levels. The scoped shareware exception is dropped. The PID
identity guard moved out of phase 0: it is built in psDoom first and re-imported.
