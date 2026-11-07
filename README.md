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

**Phase 0: repository bootstrap.** There is no game yet. This repository holds
the process layer shared with psDoom (`psproc/`), its host tests, and the tool
that keeps it in step with psDoom. The engine, game data and QuakeC arrive in
later phases; see [the architecture guide](docs/architecture.md).

## Build and test

You need macOS, CMake 3.16 or later and the system C compiler (Xcode Command
Line Tools). Nothing else is installed.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The C tests are built without the native macOS backend
(`PSD_NO_NATIVE_BACKEND`), so no test lists, renices or signals a real process.
They use an in-memory fake backend, plus the script backend driving small shell
scripts the tests write into the build tree. The drift-tool test runs `bash` and
`git` against throwaway repositories in the build tree.

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

psQuake's own files are under the [MIT License](LICENSE). The app psQuake will
ship links QuakeSpasm, which is GPL-2.0, so the distributed app as a whole will
be GPL-2.0. LibreQuake's data carries its own licences (BSD-3 assets, GPL-2
progs), which will ship with it.

psQuake follows the psDooM idea by Dennis Chao (_Doom as an Interface for
Process Management_, LISA 2000).
