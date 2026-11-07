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
| `Quake/gl_rmain.c` | `GLSLGamma_GammaCorrect` reallocates its texture when the window size no longer matches it | Resizable window with brightness not 1.0 |
| `Quake/gl_vidsdl.c` | `vid_desktopfullscreen` defaults to 1; the window is `SDL_WINDOW_RESIZABLE` with a 320x200 minimum and calls `PSQ_MacWatchFullscreenSpaces()` after creation; caption "psQuake"; new `VID_PSQ_WindowSizeChanged()` follows size changes, updates `modestate` and syncs the cvars (includes of `psq_engine.h`, `psq_macos.h`) | Desktop fullscreen, resizable window, persisted window state (`src/engine/psq_macos.m`) |

### Updating

Re-import a new QuakeSpasm tag as its own pristine commit over `quakespasm/`,
then re-apply the rows above. `git diff <pristine-import>..HEAD -- third_party/quakespasm`
shows exactly psQuake's changes.

## Codec licences (`licenses/`)

`libogg-1.3.2-COPYING.txt` and `libvorbis-1.3.5-COPYING.txt`, copied into
`psQuake.app/Contents/Resources/Licenses/` with the GPL-2 text, psQuake's MIT
licence and SDL's zlib licence.
