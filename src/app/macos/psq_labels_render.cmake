#
# psq_labels_render: the engine with its renderer, psQuake's glue, policy
# and label pass and the compiled progs.dat, linked with psproc_fake (no
# native process backend) and the offscreen SDL video layer
# (tests/psq_offscreen_sdl.c: a windowless CGL context, no window, no
# NSApplication). tests/psq_labels_render_driver.c plays lq_e1m1 with fake
# process monsters, checks the label pass frame by frame and writes PNGs of
# the frames. Included from psquake_app.cmake after psq_headless_test.cmake,
# whose data layout (build/headless/Resources) it shares. Registered with
# psproc_add_host_test, so psproc_native_actions_unlinked checks every
# object it links with nm before it runs.
#
#   build/src/psq_labels_render               the binary
#   build/headless/root-render/librequake/id1  its user root: labels-*.png,
#                                              psqlabels.dem
#   build/headless/root-render-options/librequake/id1
#                                              --options and --options-restart
#                                              (the psQuake options page):
#                                              options-*.png, config.cfg
#

set(_lr_engine ${PSQ_ENGINE_SOURCES})
# The driver includes main_sdl.c; the offscreen layer replaces pl_osx.m's
# platform hooks (no alert dialog on Sys_Error).
list(REMOVE_ITEM _lr_engine "${_qsq}/main_sdl.c" "${_qsq}/pl_osx.m")

add_executable(psq_labels_render
    ${_lr_engine}
    ${PSQ_CORE_SOURCES}
    ${PSQ_POLICY_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/psq_sys.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/psq_builtins.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/psq_labels_draw.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/psq_menu.c"
    "${PROJECT_SOURCE_DIR}/tests/psq_offscreen_sdl.c"
    "${PROJECT_SOURCE_DIR}/tests/psq_labels_render_driver.c")
set_target_properties(psq_labels_render PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    OSX_ARCHITECTURES "${PSQ_APP_ARCHS}"
    SKIP_BUILD_RPATH ON
    INSTALL_RPATH "")
target_include_directories(psq_labels_render PRIVATE
    "${_qsq}"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine"
    "${CMAKE_CURRENT_SOURCE_DIR}/core"
    "${PROJECT_SOURCE_DIR}/tests")
target_compile_definitions(psq_labels_render PRIVATE
    USE_SDL2 SDL_FRAMEWORK NO_SDL_CONFIG
    _GNU_SOURCE=1 _THREAD_SAFE GL_SILENCE_DEPRECATION=1
    USE_CODEC_WAVE
    PSQ_HEADLESS_TEST
    "PSQ_VERSION=\"${PSQ_APP_VERSION}\""
    "PSQ_TEST_RESOURCES=\"${_hl_res}\""
    "PSQ_TEST_ROOT=\"${_hl}/root-render\"")
target_compile_options(psq_labels_render PRIVATE "-F${_qsmac}" -w -O1 -g)
target_link_libraries(psq_labels_render PRIVATE
    "${_qsmac}/SDL2.framework"
    "-framework CoreFoundation" "-framework OpenGL")
# No IOKit: in_sdl.c's IOKit calls bind to the failing stubs in
# psq_offscreen_sdl.c; this fails the link if anything else needs IOKit.
psproc_add_host_test(psq_labels_render)
add_dependencies(psq_labels_render psq_headless_layout)

set_tests_properties(psq_labels_render PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 120)

# The same binary on the test arena (psqtesthall, the one-grid hall, loaded
# by name): its grid monsters and their labels, then lq_e1m1, where the gate
# is shut and nothing is labelled. Its user root is
# build/headless/root-render-arena.
if(PSQ_ARENA_BSP)
    add_test(NAME psq_labels_render_arena COMMAND psq_labels_render --arena)
    set_tests_properties(psq_labels_render_arena PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 120
        FIXTURES_REQUIRED psproc_native_actions_unlinked)
else()
    # No arena built: reported Skipped, like the arena's compiled-map tests.
    add_test(NAME psq_labels_render_arena COMMAND ${CMAKE_COMMAND}
        "-DPSQ_SKIP_MESSAGE=psq_labels_render_arena: no arena was built; run tools/setup-build.sh"
        -P "${PROJECT_SOURCE_DIR}/tests/arena/skip.cmake")
    set_tests_properties(psq_labels_render_arena PROPERTIES SKIP_RETURN_CODE 77)
endif()

# --arena-shots <arena>: each arena of the exit chain (psqarena, New Game's,
# then psqterraces and psqtowers) loaded with the six fake process monsters:
# each must arrive alone on a cell centre of one of the arena's grids; then
# they stand at the script's pins, a 1280x720 frame with labels is written
# from every camera stand (shot-<stand>.png: start, a high point, a cover
# spot, the overview), and two chases are logged. The stands come from
# generate_arena.py --shots, copied into the run's user root first:
# build/headless/root-render-shots-<arena>/librequake/id1.
if(PSQ_ARENA_BSP)
    foreach(_a IN LISTS PSQ_ARENA_CHAIN)
        set(_shots_dir "${_hl}/root-render-shots-${_a}/librequake/id1")
        add_test(NAME psq_arena_shots_${_a}_script
            COMMAND /bin/bash -c "mkdir -p \"$1\" && cp -f \"$2\" \"$1/shots.txt\"" _
                "${_shots_dir}" "${PROJECT_BINARY_DIR}/arena/work-${_a}/psqarena.shots")
        set_tests_properties(psq_arena_shots_${_a}_script PROPERTIES
            FIXTURES_SETUP psq_arena_shots_${_a})
        add_test(NAME psq_arena_shots_${_a} COMMAND psq_labels_render --arena-shots ${_a})
        set_tests_properties(psq_arena_shots_${_a} PROPERTIES
            SKIP_RETURN_CODE 77 TIMEOUT 180
            FIXTURES_REQUIRED "psq_arena_shots_${_a};psproc_native_actions_unlinked")
    endforeach()
endif()

# The Options > psQuake page (the same binary, --options): the live-kill
# warning gates live kill, the console and the command line cannot set the
# kill mode, and config.cfg keeps the other settings but no kill mode. Then
# --options-restart launches again on the same user root: renice again,
# the other settings back, and the trust guard on the page. The root is
# emptied first, so the first run starts as a new install.
add_test(NAME psq_options_clean
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_hl}/root-render-options")
set_tests_properties(psq_options_clean PROPERTIES
    FIXTURES_SETUP psq_options_clean)
add_test(NAME psq_options_render COMMAND psq_labels_render --options)
set_tests_properties(psq_options_render PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 120
    FIXTURES_SETUP psq_options_first_run
    FIXTURES_REQUIRED "psq_options_clean;psproc_native_actions_unlinked")
add_test(NAME psq_options_restart COMMAND psq_labels_render --options-restart)
set_tests_properties(psq_options_restart PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 120
    FIXTURES_REQUIRED "psq_options_first_run;psproc_native_actions_unlinked")

# --options-scale: Options > Scale at 3 (the menu canvas as wide as the
# window) at 960x660 and 800x600: frames of the psQuake page, the warning,
# the credits and the stock Options menu. Its own root, emptied first.
add_test(NAME psq_options_scale_clean
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_hl}/root-render-options-scale")
set_tests_properties(psq_options_scale_clean PROPERTIES
    FIXTURES_SETUP psq_options_scale_clean)
add_test(NAME psq_options_scale COMMAND psq_labels_render --options-scale)
set_tests_properties(psq_options_scale PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 120
    FIXTURES_REQUIRED "psq_options_scale_clean;psproc_native_actions_unlinked")

# --options-demo: a normal launch (no +map): the attract demo plays, and its
# comment-only stuffed text must leave live kill choosable; a stuffed
# command still marks the session. Its own root, emptied first.
add_test(NAME psq_options_demo_clean
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_hl}/root-render-options-demo")
set_tests_properties(psq_options_demo_clean PROPERTIES
    FIXTURES_SETUP psq_options_demo_clean)
add_test(NAME psq_options_demo COMMAND psq_labels_render --options-demo)
set_tests_properties(psq_options_demo PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 120
    FIXTURES_REQUIRED "psq_options_demo_clean;psproc_native_actions_unlinked")
