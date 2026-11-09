# psQuake

psQuake will be a macOS Quake app that turns running processes into labeled
monsters. It is the sibling of [psDoom](https://github.com/ResourceFork/psDoom),
built on [QuakeSpasm](https://github.com/sezero/quakespasm) 0.97.0 with
[LibreQuake](https://github.com/lavenderdotpet/LibreQuake)'s free game data, so
no id Software data is needed.

> **Before you play:** psQuake will affect real processes. Its default setting
> will only lower a process's scheduling priority when you wound its monster,
> and will not terminate anything. Do not enable live killing unless you
> understand that defeating a monster can send `SIGTERM` to the matching
> process without an in-game confirmation or undo. Save your work first.

## Status

**Phases 1, 2 and 4.** `psQuake.app` is Quake on QuakeSpasm with LibreQuake,
plus psQuake's own QuakeC: New Game opens the arena map (below), where your
running processes teleport in as monsters. Labels over the monsters (phase 3)
and the options page (phase 5) are still to come. This repository also holds
the process layer shared with psDoom (`psproc/`), its host tests, and the tool
that keeps it in step with psDoom; see
[the architecture guide](docs/architecture.md).

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

The C tests are built without the native macOS backend
(`PSD_NO_NATIVE_BACKEND`), so no test lists, renices or signals a real process.
They use an in-memory fake backend, plus the script backend driving small shell
scripts the tests write into the build tree. The drift-tool test runs `bash` and
`git` against throwaway repositories in the build tree. The engine-side tests
cover game data selection, the network bind address and the mouse grab rule.

## Run

```sh
open build/psQuake.app
```

### Game data and where psQuake writes

psQuake keeps everything it writes in one folder, the **user root**:
`~/Library/Application Support/psQuake/` by default.

- **Registered Quake** takes precedence: put its paks in `<user root>/id1/`,
  either `pak0.pak` and `pak1.pak`, or the 2021 re-release's single
  `pak0.pak` (psQuake looks for `gfx/pop.lmp`, as Quake does). Config, saves and logs then go to `<user root>/id1/` and
  `<user root>/`.
- **Otherwise LibreQuake** from inside the app is played. Config and saves go
  to `<user root>/librequake/id1/`, the console log (`-condebug`) and history
  to `<user root>/librequake/`. Nothing is written into the app.
- **Shareware Quake** (`pak0.pak`, no `pak1.pak`, no `gfx/pop.lmp`) is ignored, and LibreQuake
  is used instead. id's shareware licence forbids running it with user-made
  content, which psQuake adds.

To use another folder, for tests or a second profile, pass an absolute path:

```sh
open build/psQuake.app --args -psquake-root /path/to/folder
```

The folder is created if its parent exists. Quake's own `-basedir` is refused,
because it would move reads and writes outside the user root.

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
pads in place) before the build bundles it. Without the map compilers the app
still builds, New Game opens the start map, and the arena's compiled-map
tests report Skipped. With registered Quake data
there is no arena yet, and New Game opens id's start map.

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
QuakeSpasm, which is GPL-2.0-or-later, so the app as a whole is GPL-2.0. It
also bundles SDL 2.0.22 (zlib) and libogg and libvorbis (BSD-3). Their licence
texts are in `psQuake.app/Contents/Resources/Licenses/`, and
[third_party/README.md](third_party/README.md) records the vendored code and
every change to it. LibreQuake's data carries its own licences (BSD-3 assets,
GPL-2 progs), shipped in `Contents/Resources/LibreQuake/`.

psQuake follows the psDooM idea by Dennis Chao (_Doom as an Interface for
Process Management_, LISA 2000).
