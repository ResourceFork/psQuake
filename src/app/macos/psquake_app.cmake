#
# psQuake.app (macOS). Included from src/CMakeLists.txt.
#
#   build/psQuake.app/Contents/MacOS/psQuake   the engine
#   build/psQuake.app/Contents/Frameworks/     SDL2.framework, Ogg/Vorbis
#   build/psQuake.app/Contents/Resources/      icon, licences, LibreQuake
#
# Native arm64 only (see PSQ_APP_ARCHS below). Every dependency is vendored: SDL 2.0.22 and the Ogg/Vorbis dylibs come
# from QuakeSpasm's own MacOSX/ folder, so nothing from Homebrew is linked or
# bundled. Install names are @executable_path/../Frameworks/...; the binary
# carries no LC_RPATH.
#

enable_language(OBJC)

set(PSQ_APP_VERSION "0.1.0")
set(PSQ_BUNDLE_ID "com.resourcefork.psquake")

# arm64 only. The vendored SDL2.framework's x86_64 slice has 8 free bytes
# after its load commands and no code signature; ad-hoc signing adds a
# 16-byte LC_CODE_SIGNATURE that overwrites the start of its __text, so a
# universal app would ship corrupted Intel code. x86_64 needs an SDL2 built
# with header room (a later phase).
set(PSQ_APP_ARCHS "arm64" CACHE STRING "Architectures for psQuake.app (arm64)")
if(NOT PSQ_APP_ARCHS STREQUAL "arm64")
    message(FATAL_ERROR
        "psQuake: PSQ_APP_ARCHS must be arm64. The vendored SDL2 x86_64 slice "
        "cannot be signed without corrupting it (src/app/macos/psquake_app.cmake).")
endif()
string(REPLACE ";" "," _psq_archs_csv "${PSQ_APP_ARCHS}")

set(_qs   "${PROJECT_SOURCE_DIR}/third_party/quakespasm")
set(_qsq  "${_qs}/Quake")
set(_qsmac "${_qs}/MacOSX")
set(_app_dir "${CMAKE_CURRENT_SOURCE_DIR}/app/macos")

# Upstream Makefile.darwin's object list, without the Cocoa launcher
# (AppController, QuakeArgument(s), ScreenInfo, SDLApplication) and SDLMain.
# Codec sources compile to nothing unless their USE_CODEC_* is defined.
set(PSQ_ENGINE_SOURCES
    strlcat.c strlcpy.c
    gl_refrag.c gl_rlight.c gl_rmain.c gl_fog.c gl_rmisc.c r_part.c
    r_world.c gl_screen.c gl_sky.c gl_warp.c gl_vidsdl.c gl_draw.c image.c
    gl_texmgr.c gl_mesh.c r_sprite.c r_alias.c r_brush.c gl_model.c
    in_sdl.c
    snd_dma.c snd_mix.c snd_mem.c
    bgmusic.c snd_codec.c snd_flac.c snd_wave.c snd_vorbis.c snd_opus.c
    snd_mp3.c snd_mp3tag.c snd_mikmod.c snd_modplug.c snd_xmp.c snd_umx.c
    snd_sdl.c cd_sdl.c
    net_bsd.c net_udp.c net_dgrm.c net_loop.c net_main.c
    chase.c cl_demo.c cl_input.c cl_main.c cl_parse.c cl_tent.c console.c
    keys.c menu.c sbar.c view.c wad.c cmd.c common.c miniz.c crc.c cvar.c
    cfgfile.c host.c host_cmd.c mathlib.c pr_cmds.c pr_edict.c pr_exec.c
    sv_main.c sv_move.c sv_phys.c sv_user.c world.c zone.c
    pl_osx.m sys_sdl_unix.c main_sdl.c)
list(TRANSFORM PSQ_ENGINE_SOURCES PREPEND "${_qsq}/")

# Stage the codecs: thinned to PSQ_APP_ARCHS, re-pathed into Frameworks.
# One staging folder per architecture list, so changing it re-stages.
set(_codec_out "${CMAKE_CURRENT_BINARY_DIR}/codecs-${_psq_archs_csv}")
set(_codec_libs
    "${_codec_out}/libvorbisfile.dylib"
    "${_codec_out}/libvorbis.dylib"
    "${_codec_out}/libogg.dylib")
add_custom_command(
    OUTPUT ${_codec_libs}
    COMMAND /bin/bash "${_app_dir}/stage_codecs.sh"
        "${_qsmac}/codecs/lib" "${_codec_out}" "${_psq_archs_csv}"
    DEPENDS
        "${_app_dir}/stage_codecs.sh"
        "${_qsmac}/codecs/lib/libogg.dylib"
        "${_qsmac}/codecs/lib/libvorbis.dylib"
        "${_qsmac}/codecs/lib/libvorbisfile.dylib"
    COMMENT "Staging the Ogg/Vorbis codecs for psQuake.app"
    VERBATIM)
add_custom_target(psquake_codecs DEPENDS ${_codec_libs})

add_executable(psQuake MACOSX_BUNDLE
    ${PSQ_ENGINE_SOURCES}
    ${PSQ_CORE_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/psq_sys.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/psq_macos.m")
add_dependencies(psQuake psquake_codecs)

set_target_properties(psQuake PROPERTIES
    OUTPUT_NAME "psQuake"
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
    OSX_ARCHITECTURES "${PSQ_APP_ARCHS}"
    MACOSX_BUNDLE_INFO_PLIST "${_app_dir}/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "psQuake"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${PSQ_BUNDLE_ID}"
    MACOSX_BUNDLE_ICON_FILE "psQuake"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PSQ_APP_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PSQ_APP_VERSION}"
    # No LC_RPATH: every bundled library is named by @executable_path.
    SKIP_BUILD_RPATH ON
    INSTALL_RPATH ""
    LINK_DEPENDS "${_codec_out}/libvorbisfile.dylib")

target_include_directories(psQuake PRIVATE
    "${_qsq}"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine"
    "${_qsmac}/codecs/include")

# Makefile.darwin's defines with USE_SDL2=1, and only the codecs LibreQuake
# needs: its music is id1/music/track*.ogg (Ogg Vorbis). Sound effects are
# WAV inside the paks and need no codec.
target_compile_definitions(psQuake PRIVATE
    USE_SDL2
    SDL_FRAMEWORK NO_SDL_CONFIG
    _GNU_SOURCE=1 _THREAD_SAFE
    GL_SILENCE_DEPRECATION=1
    USE_CODEC_WAVE USE_CODEC_VORBIS
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
    $<$<CONFIG:Debug>:DEBUG>)
target_compile_options(psQuake PRIVATE
    "-F${_qsmac}"
    -Wall
    $<IF:$<CONFIG:Debug>,-g,-O2>)
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    target_compile_options(psQuake PRIVATE -mmacosx-version-min=11.0)
    target_link_options(psQuake PRIVATE -mmacosx-version-min=11.0)
endif()

target_link_libraries(psQuake PRIVATE
    "${_qsmac}/SDL2.framework"
    ${_codec_libs}
    "-framework Cocoa"
    "-framework IOKit"
    "-framework OpenGL")

# Assemble and sign the bundle on every build, so data fetched after the
# first build is picked up and a stale signature never survives.
set(PSQ_DATA_DIR "${PROJECT_SOURCE_DIR}/data/librequake")
add_custom_target(psquake_bundle ALL
    COMMAND /bin/bash "${_app_dir}/assemble_bundle.sh"
        "$<TARGET_BUNDLE_DIR:psQuake>" "${_psq_archs_csv}"
        "${_qsmac}/SDL2.framework" "${_codec_out}"
        "${PROJECT_SOURCE_DIR}" "${PSQ_DATA_DIR}"
    DEPENDS psQuake "${_app_dir}/assemble_bundle.sh"
    COMMENT "Assembling and ad-hoc signing psQuake.app"
    VERBATIM)
add_dependencies(psquake_bundle psQuake)

if(EXISTS "${PSQ_DATA_DIR}/id1/pak0.pak")
    message(STATUS "psQuake: will bundle LibreQuake from ${PSQ_DATA_DIR}")
else()
    message(WARNING
        "psQuake: no LibreQuake data in ${PSQ_DATA_DIR}.\n"
        "The app will build, but will only run with registered Quake in "
        "~/Library/Application Support/psQuake/id1. Run "
        "tools/fetch-librequake.sh, then build again, to bundle LibreQuake.")
endif()
