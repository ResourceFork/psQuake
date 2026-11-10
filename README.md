# psQuake

psQuake is a macOS Quake app that turns your running processes into labeled
monsters. It is the sibling of [psDoom](https://github.com/ResourceFork/psDoom),
built on [QuakeSpasm](https://github.com/sezero/quakespasm) 0.97.0 with
[LibreQuake](https://github.com/lavenderdotpet/LibreQuake)'s free game data, so
no id Software data is needed.

> **Before you play:** psQuake acts on real processes. Every launch starts in
> **renice (no kill)**: wounding a process monster lowers that process's
> scheduling priority, and defeating it terminates nothing. **Live kill**,
> where defeating a monster sends `SIGTERM` to its process with no in-game
> confirmation and no undo, is only available on the options page, behind a
> warning, and lasts until you quit. Save your work before you turn it on.

## Status

**Phases 1 to 5 are built.** `psQuake.app` is Quake on QuakeSpasm with
LibreQuake, plus psQuake's own QuakeC: New Game opens the arena map, where
your running processes teleport in as monsters, each labelled with its process
name, and Options > psQuake Options sets how psQuake acts on them. The
windowed check of the whole game on a real desktop is still to do. The app is
arm64 only and ad-hoc signed: there is no Intel build, Developer ID signature
or notarization yet. This repository also holds the process layer shared with
psDoom (`psproc/`), its host tests, and the tool that keeps it in step with
psDoom; see [the architecture guide](docs/architecture.md).

## Install

From a release zip (`psQuake-<version>-arm64.zip`, made by
`tools/package-release.sh`, below):

1. Unzip it and move `psQuake.app` to `/Applications` (or anywhere).
2. Open it. The app is ad-hoc signed, not notarized, so the first time macOS
   says it cannot check it. Open **System Settings > Privacy & Security** and
   choose **Open Anyway** for psQuake, then open it again.

You need macOS 11 or later on Apple silicon. LibreQuake and the arena are
inside the app; nothing else is installed. psQuake writes only to its user
root (below).

## Build and test

You need macOS 11 or later, CMake 3.16 or later, Python 3.9 or later and the
system C compiler (Xcode Command Line Tools). SDL2 and the Ogg Vorbis codec
are vendored with the engine (`third_party/`). The arena map also needs the
ericw-tools map compilers, whose `light` needs Embree and TBB from
[Homebrew](https://brew.sh); `tools/setup-build.sh` sets everything up:

```sh
tools/setup-build.sh             # once: LibreQuake data and the map compilers (below)
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

This builds `build/psQuake.app` (native arm64, ad-hoc signed).

`tools/setup-build.sh` runs `tools/fetch-librequake.sh`, then
`brew install` for whichever of `embree` and `tbb` is missing (Homebrew's
auto-update, cleanup and upgrades of installed formulae off), then builds
ericw-tools 2.0.0-alpha11 (its tag must point at the pinned commit) into the
git-ignored `data/ericw-tools/bin/`, keeping only `qbsp`, `vis` and `light`.
`data/ericw-tools/STAMP` records the pin, the Embree and TBB versions and the
binaries' SHA-256s; every configure runs `tools/setup-build.sh --check` and
builds the arena only when they all still match, so a Homebrew upgrade or a
replaced binary means running `tools/setup-build.sh` again. `--check` only
reports; `--no-data` skips the LibreQuake download. ericw-tools is
GPL-3.0-or-later; it only compiles the map and is neither linked nor shipped.
`tools/fetch-librequake.sh` checks the archive and both paks against pinned
SHA-256 hashes and extracts into the git-ignored `data/librequake/`; each build
copies it into the app. Without it the app still builds, with a warning, and
then runs only with registered Quake (below). `--from-zip FILE` uses a copy of
`full.zip` you already have. There is no Intel build yet: the vendored SDL2's
x86_64 code cannot be signed without corrupting it, and the build refuses to.

No test renices or signals a real process. The C tests are built without the
native macOS backend's actions: the policy, headless and render tests use an
in-memory fake backend (or the script backend driving small shell scripts the
tests write into the build tree), and `psproc_select_test` lists the real
process table read-only, through a copy of the native backend whose renice
and kill calls are replaced by stubs that abort. `psproc_native_actions_unlinked`
checks every test binary with `nm` first and fails if any of them could reach
`setpriority` or `kill`. The drift-tool test runs `bash` and `git` against
throwaway repositories in the build tree. The engine-side tests cover game data
selection, the network bind address, the mouse grab rule, the arena, the
labels and the options page, the last two in an offscreen OpenGL context (no
window).

### Release zip

```sh
tools/package-release.sh          # build-release/dist/psQuake-<version>-arm64.zip
tools/package-release.sh --test   # also run ctest in build-release first
```

It configures `build-release` with `CMAKE_BUILD_TYPE=Release`, builds it,
checks the app (`src/app/macos/check_bundle.sh --release`: arm64 only,
bundled libraries only, ad-hoc signed with no entitlements, every licence
text, LibreQuake against its manifest, the arena, no test-only console
commands, and the commit it was built from, which must have no uncommitted
or untracked changes) and writes the zip, its SHA-256, a list of every file
in the app and the check's report into `build-release/dist/`. It uploads
nothing. Before sharing a zip, publish that commit: the zip's
`Licenses/SOURCE.txt` names it as the app's GPL-2 source.

## Run

```sh
open build/psQuake.app
```

### Game data and where psQuake writes

psQuake keeps everything it writes in one folder, the **user root**:
`~/Library/Application Support/psQuake/` by default.

- **Registered Quake** takes precedence: put its paks in `<user root>/id1/`,
  either `pak0.pak` and `pak1.pak`, or the 2021 re-release's single
  `pak0.pak` (psQuake looks for `gfx/pop.lmp`, as Quake does). Config, saves
  and logs then go to `<user root>/id1/` and `<user root>/`. There is no
  psQuake arena for registered data: New Game opens id's own `start` map,
  where no process monsters appear.
- **Otherwise LibreQuake** from inside the app is played. Config and saves go
  to `<user root>/librequake/id1/`, the console log (`-condebug`) and history
  to `<user root>/librequake/`. Nothing is written into the app.
- **Shareware Quake** (`pak0.pak`, no `pak1.pak`, no `gfx/pop.lmp`) is
  ignored, and LibreQuake is used instead. id's shareware licence forbids
  running it with user-made content, which psQuake adds.

LibreQuake is the default because it is free to redistribute (BSD-3 assets,
GPL-2 game code), so the app can ship complete.

To use another folder, for tests or a second profile, pass an absolute path:

```sh
open build/psQuake.app --args -psquake-root /path/to/folder
```

The folder is created if its parent exists. Quake's own `-basedir` is refused,
because it would move reads and writes outside the user root.

### Options

**Options > psQuake Options**:

| Row | Values | Default | Saved |
| --- | --- | --- | --- |
| Kill mode | simulate, renice (no kill), renice + KILL | renice (no kill) | never |
| Max live monsters | 5 to 35 (a slider) | 6 | yes |
| Monster size by | memory, CPU load | memory | yes |
| Monster infighting | off, on | off | yes |
| Target all users | off, on | off | yes |
| Process labels | off, on | on | yes |
| Show PID | off, on | off | yes |
| Label distance | near, normal, far, all | normal | yes |
| Credits, licences | the parts of psQuake and their licences | | |

Left and right change a row; Enter steps it forward. A box under the rows
says what the highlighted row does. A saved setting keeps its value when a
new version changes its default: a config from before 6 was the default
keeps its max live monsters (10 unless you changed it).

- **Kill mode.** *Simulate* touches no process. *Renice (no kill)* lowers a
  process's priority each time you wound its monster; defeating the monster
  signals nothing. *Renice + KILL* also sends `SIGTERM` when a monster is
  defeated. Choosing it opens a warning: only typing **Y** turns it on;
  Escape, Enter, a click, a gamepad button or typing anything else cancels.
  (The function keys keep their bindings, and with a keyboard layout whose
  Y key types no Latin Y, switch layout to confirm.) The kill mode is never saved: every launch starts
  in renice (no kill), and there is no command-line flag, console command or
  config line that sets it (`psq_killpolicy` only shows it).
- **Monster infighting.** Off: other monsters may attack process monsters,
  but do them no damage, so only you wound and kill them. On: they can hurt
  and kill each other, and that acts on the processes as your hits do.
- **Target all users.** Off: only your own processes. On: other users'
  processes too; macOS usually refuses to renice or signal them.
- **Labels.** Each process monster shows its process name, and its PID with
  Show PID; Label distance sets how far away labels still show.

The saved options are ordinary console variables in `config.cfg`
(`psq_cap`, `psq_classify`, `psq_infighting`, `psq_allusers`,
`psq_labels`, `psq_showpid`, `psq_labelrange`).

### Safety model

- **What can be touched.** Only processes that appear as monsters in the
  psQuake arena, during a local single-player game (one player, not
  deathmatch or coop, not a dedicated server). Everywhere else, including
  LibreQuake's campaign maps, saved games, demos and network games, nothing
  is listed or acted on. (The console setting `psq_testarena 1`, for
  testing, also lets process monsters onto any other map until you quit,
  unless the session is untrusted, below.)
- **Built-in exclusions.** psQuake itself, the shell or launcher chain that
  started it, `launchd` and the kernel, and the session-critical processes
  `loginwindow`, `WindowServer`, `Dock`, `Finder` and `SystemUIServer` never
  become monsters. Other users' processes are left out unless Target all
  users is on.
- **Identity.** A process is acted on only while it is still the one its
  monster stands for (same PID, start time and name); a PID reused by a new
  process is never signalled.
- **Trust guard.** Once a network server or a demo sends console commands
  (text that is only `//` comments runs nothing and does not count:
  LibreQuake's menu demos send one), or game code runs any command beyond
  the stock game's own, psQuake treats
  the session as untrusted until you quit: the kill mode can only go down,
  Target all users and Monster infighting can only be turned off, and the
  options page says so. If those two are on, the live-kill warning says so
  too. Game code can never set these options.
- **Network.** Sockets bind to `127.0.0.1` unless you widen them (below), so
  no other machine can join a game here.

These safeguards reduce risk; they do not replace care with live kill.

### Window and fullscreen

The game starts in an 800x600 window you can resize. Alt-Enter, or Options >
Video Options > Fullscreen, switches to fullscreen on the current desktop
(no display mode change) and back. The window size and the fullscreen state
are saved in `config.cfg` on quit and restored at the next launch. The
title-bar green button and Ctrl-Cmd-F also switch fullscreen, and the saved
state follows them.

The menu and the console free the mouse and show the cursor, in a window or
fullscreen; closing them captures the mouse again.

### Network

psQuake binds its UDP sockets to `127.0.0.1` only, so single player and local
games are not reachable from other machines. To host or join over a network,
widen the bind explicitly:

```sh
open build/psQuake.app --args -psquake-bind 0.0.0.0        # every interface
open build/psQuake.app --args -psquake-bind 192.168.1.20   # one interface
```

`-noudp` opens no socket at all. Quake's `-ip` still sets only the address the
game advertises. On the default loopback bind, the multiplayer menu's search
for local games finds nothing and the console may show a `UDP_Write` error:
searching broadcasts to the network, which a loopback socket cannot reach.
Process monsters never appear in network games.

### The arena

New Game opens the psQuake arena (`maps/psqarena.bsp`), not LibreQuake's start
map. You start in a small room, cross one of four skill pads (easy to
nightmare, left to right) into the hall, where your processes teleport in
as monsters, one to a cell of the floor grid, and leave through the
teleporter in the north alcove for LibreQuake's first map, `lq_e1m1`. The
process monsters stay behind: the campaign has only its own monsters.
Pickups found in the arena do not carry over: the campaign starts with a new
game's weapons and health. You cannot save in the arena (the Save menu says
so), nor on any level that holds process monsters; saving works as usual
once you leave it.

The arena is generated, not drawn: `tools/generate_arena.py` writes the map
and a texture WAD (textures copied from LibreQuake's own maps), ericw-tools
compiles it, and `tools/validate_bsp.py` checks the result (sealed, lit, every
texture embedded, the grid clear for the largest monsters, the exit and skill
pads in place, every cell walkable from the start) before the build bundles
it. Without the map compilers the app still builds, New Game opens the start
map, and the arena's compiled-map tests report Skipped.

Three multi-level prototypes of the hall sit behind a build option, for
comparison only: `cmake -S . -B build -DPSQ_ARENA_LAYOUT=terraces` (or `pit`,
`towers`; `hall` is the default and the arena that ships). They spawn
monsters on several grids, one per floor (`psq_grid` entities beside
`psq_arena`). `build/src/psq_labels_render --arena-shots` renders one offscreen
from the camera stands in `build/arena/work/psqarena.shots` (copied to
`build/headless/root-render-arena-shots/librequake/id1/shots.txt`) and logs a
chase; `psqarena.plan.svg` beside it is the top-down plan.

## The shared process layer

`psproc/` is a copy of psDoom's engine-free process layer, imported from
psDoom commit `436ba83`. `psproc/UPSTREAM` records the psDoom commit it is
synced to. Safety fixes are made in psDoom first and then re-imported:

```sh
tools/psproc-drift.sh                     # report local adaptations and upstream changes
tools/psproc-drift.sh --commit <rev>      # what psDoom <rev> would change
tools/psproc-drift.sh --apply <rev>       # re-import <rev> through the path map (no commit)
tools/psproc-drift.sh --mark <rev>        # record <rev> after finishing an apply by hand
```

The tool reads psDoom (default `../psDoom`, or `--psdoom DIR`) only through
`git`, and never commits. Run it with `--help` for details.

## License

psQuake's own files are under the [MIT License](LICENSE). `psQuake.app` links
QuakeSpasm, which is GPL-2.0-or-later, so the app as a whole is GPL-2.0. Its
game code is compiled from LibreQuake's QuakeC (GPL-2), and its game data is
LibreQuake's (BSD-3 assets). It also bundles SDL 2.0.22 (zlib) and libogg and
libvorbis (BSD-3). fteqcc (GPL-2) and ericw-tools (GPL-3.0-or-later) only
build the game code and the arena and are not shipped. Every licence text,
the notices ([third_party/NOTICES.txt](third_party/NOTICES.txt)) and the
commit the app was built from are in
`psQuake.app/Contents/Resources/Licenses/`, and the options page's credits
view lists them. [third_party/README.md](third_party/README.md) records the
vendored code and every change to it.

psQuake follows the psDooM idea by Dennis Chao (_Doom as an Interface for
Process Management_, LISA 2000).
