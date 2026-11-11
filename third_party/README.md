# Third-party code in psQuake

psQuake's own files are MIT. The engine below is GPL-2.0-or-later, so the
psQuake.app built from this repository is distributed under GPL-2.0 as a whole.

## QuakeSpasm (`quakespasm/`)

| Item | Value |
| --- | --- |
| Source | <https://github.com/sezero/quakespasm> |
| Tag | `quakespasm-0.97.0` |
| Commit | `e8795a79273f2823661727265d0617106e282bea` |
| Licence | GPL-2.0-or-later (`quakespasm/LICENSE.txt`) |
| Imported in | the "Vendor QuakeSpasm 0.97.0 (pristine)" commit, byte-identical to upstream |

### What was imported

- `Quake/`, whole (engine sources, upstream Makefiles, `quakespasm.pak`).
- `LICENSE.txt`, `Quakespasm.txt`, `Quakespasm-Music.txt`.
- `MacOSX/SDL2.framework`: SDL 2.0.22 (zlib licence, in the framework's
  `Resources/License.txt`), universal i386/x86_64/arm64.
- `MacOSX/QuakeSpasm.icns`: the app icon placeholder.
- `MacOSX/codecs/include/{ogg,vorbis}` and `MacOSX/codecs/lib/lib{ogg,vorbis,vorbisfile}.dylib`:
  libogg 1.3.2 and libvorbis 1.3.5 (BSD-3, Xiph.Org; licence texts in
  `licenses/`, taken from the upstream libogg and libvorbis repositories at
  those tags, since QuakeSpasm's tree carries none).

### What was left out

- `Linux/`, `Windows/`, `Misc/`, `Quakespasm.html`.
- `MacOSX/SDL.framework` (SDL 1.2), the Xcode projects, `English.lproj`,
  `Info.plist`, `Launcher-Info.plist`.
- The Cocoa launcher: `AppController`, `QuakeArgument(s)`, `ScreenInfo`,
  `SDLApplication`, `SDLMain` (`.h`/`.m`).
- The other codecs in `MacOSX/codecs` (FLAC, mad, mikmod, mpg123, Opus,
  opusfile, xmp). LibreQuake's music is Ogg Vorbis (`id1/music/track*.ogg`).

`Quake/` files that psQuake does not build (other platforms' Makefiles and
sources, `quakespasm.pak`) stay so the tree diffs cleanly against upstream.
`quakespasm.pak` is not bundled.

### How it is built

`src/app/macos/psquake_app.cmake` compiles `Makefile.darwin`'s object list
without the launcher, with `USE_SDL2`, `USE_CODEC_WAVE` and `USE_CODEC_VORBIS`
only, for arm64 only (the vendored SDL2's x86_64 slice has no room for a code
signature; `assemble_bundle.sh` checks every slice). The codec dylibs are
thinned to arm64 and re-pathed
from `@executable_path/` to `@executable_path/../Frameworks/` by
`src/app/macos/stage_codecs.sh`; `SDL2.framework` already uses that path.
The binary carries no `LC_RPATH`.

### psQuake changes

Every change is marked `[psQuake]` in the code.

| File | Change | Why |
| --- | --- | --- |
| `Quake/main_sdl.c` | Removed the `#define main SDL_main` block for macOS | No Cocoa launcher: the engine's `main` is the app's entry point |
| `Quake/sys_sdl_unix.c` | `Sys_Init` calls `PSQ_InitPaths()` after upstream's basedir/userdir setup (include of `psq_engine.h`); it refuses `-basedir` | Data selection and the user root (`src/engine/psq_sys.c`, `psq_data.c`) |
| `Quake/console.c` | `LOG_Init` writes `qconsole.log` to `userdir`, not `basedir` | `basedir` may be the signed, read-only bundle |
| `Quake/net_udp.c` | UDP sockets bind `psq_bindaddr` (127.0.0.1 unless `-psquake-bind ADDR`) instead of `INADDR_ANY`; the bind address is decided in `UDP_Init` before any socket opens and fails closed to loopback; `myAddr` follows a single-interface bind unless `-ip` is given; a non-loopback bind is logged | Loopback by default (`src/engine/psq_net.c`) |
| `Quake/in_sdl.c` | `IN_Activate`/`IN_Deactivate` record the mouse state; new `IN_UpdateGrab()` runs after each frame's events and applies `psq_grab_wanted()`; `SDL_WINDOWEVENT_SIZE_CHANGED` calls `VID_PSQ_WindowSizeChanged()` | Menu and console release the mouse in fullscreen too; resizable window (`src/engine/psq_grab.c`) |
| `Quake/menu.c` | New `M_PSQ_BindingKey()`: the key-binding menu is waiting for a key | Keeps the mouse captured there so a mouse button can be bound |
| `Quake/menu.c`, `Quake/menu.h` | Options gains a "psQuake Options" entry (`OPT_PSQUAKE`, before Video Options); three menu states `m_psquake`, `m_psq_livewarn`, `m_psq_credits` whose drawing and keys `M_Draw`/`M_Keydown` hand to `psq_menu.c`; `M_TextEntry` is on for the live-kill warning and `M_Charinput` passes its typed character to `M_PSQ_LiveWarn_Char()` | The psQuake options page, its live-kill warning (a typed Y, following the keyboard layout as the quit prompt does) and the credits view (`src/engine/psq_menu.c`) |
| `Quake/menu.c` | New Game runs `map psqarena` when `maps/psqarena.bsp` exists, else `map start`; `M_Menu_Save_f` asks `PSQ_SaveRule()` and, when it refuses, prints its one line and shows a 5-second box instead of the Save menu (includes of `psq_arena.h`, `psq_engine.h`) | The psQuake arena replaces the start hub (`src/engine/psq_arena.c`, `tools/generate_arena.py`) |
| `Quake/sv_main.c` | `SV_SaveSpawnparms` runs QuakeC `SetNewParms` instead of `SetChangeParms` when the level being left is `psqarena` (include of `psq_arena.h`) | Arena pickups do not carry over into the campaign, whatever progs.dat runs |
| `Quake/gl_rmain.c` | `GLSLGamma_GammaCorrect` reallocates its texture when the window size no longer matches it | Resizable window with brightness not 1.0 |
| `Quake/gl_screen.c` | `SCR_UpdateScreen` calls `SCR_DrawProcessLabels()` right after `SCR_DrawCrosshair()` (prototype after the include) | Process labels (`src/engine/psq_labels_draw.c`) |
| `Quake/gl_vidsdl.c` | `vid_desktopfullscreen` defaults to 1; the window is `SDL_WINDOW_RESIZABLE` with a 320x200 minimum and calls `PSQ_MacWatchFullscreenSpaces()` after creation; caption "psQuake"; new `VID_PSQ_WindowSizeChanged()` follows size changes, updates `modestate` and syncs the cvars (includes of `psq_engine.h`, `psq_macos.h`) | Desktop fullscreen, resizable window, persisted window state (`src/engine/psq_macos.m`) |
| `Quake/pr_cmds.c` | `PF_precache_sound`/`PF_precache_model`: after load only a new name is an error, a cached one returns quietly; builtins #1200-#1211 in `pr_builtin[]` (designated initialisers, include of `psq_engine.h`); `PF_localcmd` and `PF_stuffcmd` call `PSQ_QuakeCCommand()` first, which marks the session untrusted for anything but the stock commands; `PF_changelevel` returns without a changelevel when `PSQ_ChangelevelRefused()` finds the name is not a plain map name | Monsters spawn mid-level with stock spawn functions; the process-monster builtins (`src/engine/psq_builtins.c`) |
| `Quake/cl_parse.c` | `svc_stufftext` calls `PSQ_StuffText()` before `Cbuf_AddText` (include of `psq_engine.h`) | With no local server or in a demo, any stuffed text that can run a command (`psq_stuff_text_inert`) marks the session untrusted (`src/engine/psq_builtins.c`) |
| `Quake/pr_exec.c` | A call to a NULL builtin entry is a `PR_RunError` | The table has gaps below #1200 |
| `Quake/host.c` | `Host_Init` calls `PSQ_Init()` after `PR_Init` | psq_* cvars and the process backend |
| `Quake/sv_main.c` | `SV_SpawnServer` calls `PSQ_LevelReset()` after `PR_LoadProgs` | Each level starts with no process state |
| `Quake/host_cmd.c` | `Host_Savegame_f` prints the one line and returns when `PSQ_SaveRule()` refuses (after the stock checks; include of `psq_arena.h`); `Host_Loadgame_f` calls `PSQ_AfterLoad()` after the edicts load | One save rule for the arena and process monsters (`psq_save_rule_for`); no pid from a file is acted on |
| `Quake/common.c` | `COM_InitFilesystem` calls `PSQ_AddGameCode()` right after id1 | psQuake's `progs.dat` (bundle `Resources/psquake`) ahead of the data paks |

### Updating

Re-import a new QuakeSpasm tag as its own pristine commit over `quakespasm/`,
then re-apply the rows above. `git diff <pristine-import>..HEAD -- third_party/quakespasm`
shows exactly psQuake's changes.

## fteqcc (`fteqcc/`)

| Item | Value |
| --- | --- |
| Source | <https://github.com/fte-team/fteqw>, `engine/qclib` |
| Commit | `5662b3a23d3c16f5a3b5fe74efb88e77d8fc4da0` (2025-02-01), the compiler named in LibreQuake v0.09-beta's `progs.dat` |
| Licence | GPL-2 (`fteqcc/LICENSE`) |
| Imported in | the "Vendor fteqcc and LibreQuake QuakeC (pristine)" commit |

Only the files upstream's `make qcc` builds (10 sources, 8 headers), plus
`LICENSE`, `readme.txt` and the upstream `Makefile`. `src/qc/psquake_qc.cmake`
builds it as the host tool `psq_fteqcc` (system zlib and libm only). It is a
build tool: nothing of it is linked into or shipped with psQuake.app.

| File | Change | Why |
| --- | --- | --- |
| `qcc.h` | One comment reworded ("tried to kill, without having been muted") | Its text matched the repo-wide process-action check (`psproc/tests/no_raw_backend_actions.cmake`) |

## LibreQuake QuakeC (`librequake-qc/`)

| Item | Value |
| --- | --- |
| Source | <https://github.com/lavenderdotpet/LibreQuake>, `qcsrc/` |
| Tag | `v0.09-beta` = `d1515ca9bc01e590f2d54dc496f55c9a4ed72369` |
| Licence | GPL-2 (`librequake-qc/docs/misc-docs/COPYING`); derived from id's GPL QuakeC 1.01 via quakec-v1-01-clean |
| Imported in | the same pristine commit, with `docs/README-IMPORTANT-LICENCE-INFO` and `docs/misc-docs/{COPYING,LibreProgsDeluxe.md}` |

Compiled with LibreQuake's own flags (`-D__LIBREQUAKE__ -O3`). Unmodified,
it compiles to a `progs.dat` whose every section matches the one LibreQuake
ships (CRC 5927). The compiled `progs.dat` is GPL-2, like the engine; its
licence text is the bundled GPL-2.

| File | Change | Why |
| --- | --- | --- |
| `qcsrc/progs.src` | Adds `../../../src/qc/psq_defs.qc` after `defs.qc` and `../../../src/qc/psq.qc` after the monsters | psQuake's builtins, fields and controller |
| `qcsrc/combat.qc` | `T_Damage` returns before the damage when `psq_infight_blocked` says so, and calls `psq_wound` for a process monster that survives a hit; `Killed` calls `psq_died` before `th_die` | Infighting option; wound and kill hooks |
| `qcsrc/world.qc` | `worldspawn` ends with `psq_worldspawn()` | `psq_testarena` controller on any map until the arena map exists |

## Codec licences (`licenses/`) and the notices

`libogg-1.3.2-COPYING.txt` and `libvorbis-1.3.5-COPYING.txt`, and the notices
of the small libraries in QuakeSpasm's tree that the engine compiles in, taken
verbatim from their headers (`strlcpy-strlcat-ISC.txt`, `miniz-MIT.txt`,
`lodepng-zlib.txt`, `stb_image_write-MIT.txt`), copied into
`psQuake.app/Contents/Resources/Licenses/` with QuakeSpasm's GPL-2 text,
psQuake's MIT licence, SDL's zlib licence, LibreQuake's QuakeC GPL-2 text and
licence notes, its data's BSD-3 text, fteqcc's GPL-2 text, `NOTICES.txt`
(this folder: every component, its licence, and whether it is shipped or only
builds the app) and `SOURCE.txt` (the commit the app was built from).
