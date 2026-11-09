#
# psq_headless_test: the engine, psQuake's glue and policy and the compiled
# progs.dat, linked with psproc_fake (no native process backend), driven by
# tests/psq_headless_driver.c as a dedicated loopback server. Included from
# psquake_app.cmake. Registered with psproc_add_host_test, so
# psproc_native_actions_unlinked checks every object it links with nm before
# it runs.
#
#   build/src/psq_headless_test             the binary
#   build/Frameworks/SDL2.framework         the app's signed SDL2 (../Frameworks)
#   build/headless/Resources/              id1 -> psQuake.app's id1 (LibreQuake
#                                          and the arena map),
#                                          psquake/progs.dat -> build/qc
#   build/headless/root-<run>/             each run's user root
#

set(_hl "${PROJECT_BINARY_DIR}/headless")
set(_hl_res "${_hl}/Resources")

set(_hl_engine ${PSQ_ENGINE_SOURCES})
list(REMOVE_ITEM _hl_engine "${_qsq}/main_sdl.c")   # the driver includes it

add_executable(psq_headless_test
    ${_hl_engine}
    ${PSQ_CORE_SOURCES}
    ${PSQ_POLICY_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/psq_sys.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/psq_macos.m"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/psq_builtins.c"
    "${PROJECT_SOURCE_DIR}/tests/psq_headless_driver.c")
set_target_properties(psq_headless_test PROPERTIES
    # Where it links: the nm check resolves the map's relative object
    # paths from the binary's folder.
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    OSX_ARCHITECTURES "${PSQ_APP_ARCHS}"
    SKIP_BUILD_RPATH ON
    INSTALL_RPATH "")
target_include_directories(psq_headless_test PRIVATE
    "${_qsq}"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine"
    "${CMAKE_CURRENT_SOURCE_DIR}/core")
target_compile_definitions(psq_headless_test PRIVATE
    USE_SDL2 SDL_FRAMEWORK NO_SDL_CONFIG
    _GNU_SOURCE=1 _THREAD_SAFE GL_SILENCE_DEPRECATION=1
    USE_CODEC_WAVE
    PSQ_HEADLESS_TEST
    "PSQ_TEST_RESOURCES=\"${_hl_res}\""
    "PSQ_TEST_ROOT=\"${_hl}/root-renice\"")
target_compile_options(psq_headless_test PRIVATE "-F${_qsmac}" -w -O1 -g)
target_link_libraries(psq_headless_test PRIVATE
    "${_qsmac}/SDL2.framework"
    "-framework Cocoa" "-framework IOKit" "-framework OpenGL")
psproc_add_host_test(psq_headless_test)

# Its folder: the signed SDL2 from the assembled app, the data and progs.
add_custom_target(psq_headless_layout ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PROJECT_BINARY_DIR}/Frameworks" "${_hl_res}/psquake"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        "$<TARGET_BUNDLE_DIR:psQuake>/Contents/Frameworks/SDL2.framework"
        "${PROJECT_BINARY_DIR}/Frameworks/SDL2.framework"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        "$<TARGET_BUNDLE_DIR:psQuake>/Contents/Resources/id1" "${_hl_res}/id1"
    COMMAND ${CMAKE_COMMAND} -E create_symlink "${PSQ_PROGS_DAT}"
        "${_hl_res}/psquake/progs.dat"
    COMMENT "Laying out build/headless for psq_headless_test"
    VERBATIM)
add_dependencies(psq_headless_layout psquake_bundle psquake_progs)
add_dependencies(psq_headless_test psq_headless_layout)

set_tests_properties(psq_headless_test PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 60)

# The same scenario under the other kill policies, and with the gate closed
# (two clients: not single player).
foreach(_run simulate live gate_closed)
    if(_run STREQUAL "simulate")
        set(_args -dedicated 1 +psq_killpolicy 0)
    elseif(_run STREQUAL "live")
        set(_args -dedicated 1 +psq_killpolicy 2)
    else()
        # -dedicated 2 also turns deathmatch on, which removes the
        # controller in QuakeC; with it off, only the C gate is left.
        set(_args -dedicated 2 +deathmatch 0)
    endif()
    add_test(NAME psq_headless_${_run}
        COMMAND psq_headless_test ${_args}
            -psquake-root "${_hl}/root-${_run}"
            -condebug +developer 1 +psq_debug 1 +psq_testarena 1 +map lq_e1m1)
    set_tests_properties(psq_headless_${_run} PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 60
        FIXTURES_REQUIRED psproc_native_actions_unlinked)
endforeach()

# New Game in the arena map (no psq_testarena): grid placement, wound, kill,
# retire, swarm and the grid-full fallback, the save refusal, the exit's
# parms reset, and lq_e1m1 afterwards. Under each kill policy.
if(PSQ_ARENA_BSP)
    foreach(_policy 0 1 2)
        if(_policy EQUAL 0)
            set(_run simulate)
        elseif(_policy EQUAL 1)
            set(_run renice)
        else()
            set(_run live)
        endif()
        add_test(NAME psq_headless_arena_${_run}
            COMMAND psq_headless_test -dedicated 1 +psq_killpolicy ${_policy}
                -psquake-root "${_hl}/root-arena-${_run}"
                -condebug +developer 1 +psq_debug 1 +psq_test_newgame)
        set_tests_properties(psq_headless_arena_${_run} PROPERTIES
            SKIP_RETURN_CODE 77 TIMEOUT 60
            FIXTURES_REQUIRED psproc_native_actions_unlinked)
    endforeach()
    # psq_testarena on as well: worldspawn makes a controller first, and it
    # takes the map's grid (the map's own psq_arena then removes itself).
    add_test(NAME psq_headless_arena_testarena
        COMMAND psq_headless_test -dedicated 1 +psq_killpolicy 1
            -psquake-root "${_hl}/root-arena-testarena"
            -condebug +developer 1 +psq_debug 1 +psq_testarena 1 +psq_test_newgame)
    set_tests_properties(psq_headless_arena_testarena PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 60
        FIXTURES_REQUIRED psproc_native_actions_unlinked)
endif()
