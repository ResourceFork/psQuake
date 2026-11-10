#
# psQuake arena maps (maps/<arena>.bsp), included from the top-level
# CMakeLists.txt before src/.
#
#   tools/generate_arena.py   writes an arena's .map and .wad (textures
#                             copied from LibreQuake's pak0.pak)
#   tools/compile_arena.py    runs it, then ericw-tools qbsp, vis and light
#   tools/validate_bsp.py     checks the result; a failing map is never used
#
# The arenas (generate_arena.py ARENAS): psqarena, the Pit, which New Game
# opens; psqterraces and psqtowers, which its exits lead to in turn, the last
# exit leading to the campaign; and psqtesthall, the original one-floor hall,
# which the tests use and `map psqtesthall` loads, but no exit leads to.
#
# Needs LibreQuake (data/librequake) and the pinned ericw-tools build in
# data/ericw-tools/bin; tools/setup-build.sh provides both, and its --check,
# run here at every configure, confirms the compilers still match their pin
# (data/ericw-tools/STAMP: tag, commit, Embree/TBB versions, binary hashes).
# Without them the app builds with no arena, New Game opens the stock start
# map, and the arenas' compiled-map tests report Skipped.
#
# Sets PSQ_ARENA_BSP to New Game's validated map, PSQ_ARENA_BSPS to every
# shipped arena's (New Game's first), PSQ_TEST_ARENA_BSP to the test hall's,
# and PSQ_ARENA_NAMES to their map names; all "" when they are not built.
#

find_package(Python3 3.9 COMPONENTS Interpreter)

set(PSQ_ERICW_DIR "${PROJECT_SOURCE_DIR}/data/ericw-tools")
set(PSQ_ERICW_BIN "${PSQ_ERICW_DIR}/bin")
set(PSQ_ARENA_PAK "${PROJECT_SOURCE_DIR}/data/librequake/id1/pak0.pak")
set(PSQ_ARENA_EXIT_MAP "lq_e1m1")
# map name, then where its exit leads (generate_arena.py ARENAS).
set(PSQ_ARENA_CHAIN psqarena psqterraces psqtowers)
set(_psq_arena_exit_psqarena psqterraces)
set(_psq_arena_exit_psqterraces psqtowers)
set(_psq_arena_exit_psqtowers ${PSQ_ARENA_EXIT_MAP})
set(_psq_arena_exit_psqtesthall ${PSQ_ARENA_EXIT_MAP})
set(PSQ_TEST_ARENA psqtesthall)
set(PSQ_ARENA_TOOLS
    "${PROJECT_SOURCE_DIR}/tools/generate_arena.py"
    "${PROJECT_SOURCE_DIR}/tools/compile_arena.py"
    "${PROJECT_SOURCE_DIR}/tools/validate_bsp.py")
set(PSQ_ARENA_BSP "")
set(PSQ_ARENA_BSPS "")
set(PSQ_TEST_ARENA_BSP "")
set(PSQ_ARENA_NAMES "")

set(_psq_arena_why "")
if(NOT Python3_Interpreter_FOUND)
    set(_psq_arena_why "no Python 3")
elseif(NOT EXISTS "${PSQ_ARENA_PAK}")
    set(_psq_arena_why "no LibreQuake data in data/librequake")
else()
    execute_process(
        COMMAND /bin/bash "${PROJECT_SOURCE_DIR}/tools/setup-build.sh" --check --no-data
        RESULT_VARIABLE _psq_check_rc
        OUTPUT_VARIABLE _psq_check_out
        ERROR_VARIABLE _psq_check_out
        INPUT_FILE /dev/null)
    if(NOT _psq_check_rc EQUAL 0)
        string(STRIP "${_psq_check_out}" _psq_check_out)
        set(_psq_arena_why "the pinned ericw-tools are not ready:\n${_psq_check_out}")
    endif()
endif()

if(_psq_arena_why)
    message(WARNING
        "psQuake: not building the arena map (${_psq_arena_why}).\n"
        "The app builds, and New Game opens the stock start map. Run "
        "tools/setup-build.sh, then configure again, to build the arena.")
else()
    set(_out "${PROJECT_BINARY_DIR}/arena")
    foreach(_a IN LISTS PSQ_ARENA_CHAIN PSQ_TEST_ARENA)
        set(_work "${_out}/work-${_a}")
        set(_bsp "${_out}/${_a}.bsp")
        set(_lit "${_out}/${_a}.lit")   # its coloured lightmap, bundled beside it
        add_custom_command(
            OUTPUT "${_bsp}" "${_lit}"
            COMMAND ${CMAKE_COMMAND} -E rm -f "${_bsp}" "${_lit}"
            COMMAND /bin/bash "${PROJECT_SOURCE_DIR}/tools/setup-build.sh" --check --no-data
            COMMAND ${Python3_EXECUTABLE} -I -B "${PROJECT_SOURCE_DIR}/tools/compile_arena.py"
                --tools "${PSQ_ERICW_BIN}" --pak "${PSQ_ARENA_PAK}" --out "${_work}"
                --arena "${_a}"
            COMMAND ${Python3_EXECUTABLE} -I -B "${PROJECT_SOURCE_DIR}/tools/validate_bsp.py"
                --bsp "${_work}/psqarena.bsp" --map "${_work}/psqarena.map"
                --exit-map "${_psq_arena_exit_${_a}}"
            COMMAND ${CMAKE_COMMAND} -E copy "${_work}/psqarena.lit" "${_lit}"
            COMMAND ${CMAKE_COMMAND} -E copy "${_work}/psqarena.bsp" "${_bsp}"
            DEPENDS ${PSQ_ARENA_TOOLS} "${PROJECT_SOURCE_DIR}/tools/setup-build.sh"
                "${PSQ_ARENA_PAK}" "${PSQ_ERICW_DIR}/STAMP"
                "${PSQ_ERICW_BIN}/qbsp" "${PSQ_ERICW_BIN}/vis" "${PSQ_ERICW_BIN}/light"
            COMMENT "Generating, compiling and validating the psQuake arena ${_a}"
            VERBATIM)
        list(APPEND PSQ_ARENA_BSPS "${_bsp}")
        list(APPEND PSQ_ARENA_NAMES "${_a}")
    endforeach()
    set(PSQ_ARENA_BSP "${_out}/psqarena.bsp")
    set(PSQ_TEST_ARENA_BSP "${_out}/${PSQ_TEST_ARENA}.bsp")
    add_custom_target(psquake_arena ALL DEPENDS ${PSQ_ARENA_BSPS})
    message(STATUS "psQuake: will build the arenas (${PSQ_ARENA_NAMES}) with the pinned ericw-tools in ${PSQ_ERICW_BIN}")
endif()

#
# Tests. The generator and validator unit tests need only Python; the
# compiled-map tests need the arena, and report Skipped (exit 77) without it.
#
if(Python3_Interpreter_FOUND)
    add_test(NAME psq_arena_generator_test
        COMMAND ${Python3_EXECUTABLE} -I -B
            "${PROJECT_SOURCE_DIR}/tests/arena/test_generate_arena.py"
            "${PROJECT_SOURCE_DIR}/tools")
    add_test(NAME psq_validate_bsp_test
        COMMAND ${Python3_EXECUTABLE} -I -B
            "${PROJECT_SOURCE_DIR}/tests/arena/test_validate_bsp.py"
            "${PROJECT_SOURCE_DIR}/tools" "${PSQ_TEST_ARENA_BSP}")
endif()
# The engine call sites of the arena rules (menu.c, sv_main.c, host_cmd.c).
add_test(NAME psq_arena_hooks_wired
    COMMAND ${CMAKE_COMMAND}
        "-DPSQ_QUAKE_DIR=${PROJECT_SOURCE_DIR}/third_party/quakespasm/Quake"
        -P "${PROJECT_SOURCE_DIR}/tests/arena/arena_hooks_wired.cmake")

set(_psq_arena_compiled_tests psq_arena_validate psq_arena_rejects_broken psq_arena_load)
if(PSQ_ARENA_BSP)
    # Every built arena, against its own exit (psqarena: psq_arena_validate).
    foreach(_a IN LISTS PSQ_ARENA_NAMES)
        if(_a STREQUAL "psqarena")
            continue()
        endif()
        add_test(NAME psq_arena_validate_${_a}
            COMMAND ${Python3_EXECUTABLE} -I -B "${PROJECT_SOURCE_DIR}/tools/validate_bsp.py"
                --bsp "${PROJECT_BINARY_DIR}/arena/${_a}.bsp" --exit-map "${_psq_arena_exit_${_a}}")
    endforeach()
    # psq_arena_validate: New Game's arena (the name the other tests know).
    add_test(NAME psq_arena_validate
        COMMAND ${Python3_EXECUTABLE} -I -B "${PROJECT_SOURCE_DIR}/tools/validate_bsp.py"
            --bsp "${PSQ_ARENA_BSP}" --exit-map "${_psq_arena_exit_psqarena}")
    add_test(NAME psq_arena_rejects_broken
        COMMAND ${Python3_EXECUTABLE} -I -B
            "${PROJECT_SOURCE_DIR}/tests/arena/test_rejects_broken.py"
            --tools-dir "${PROJECT_SOURCE_DIR}/tools" --ericw "${PSQ_ERICW_BIN}"
            --pak "${PSQ_ARENA_PAK}" --work "${PROJECT_BINARY_DIR}/arena-broken"
            --exit-map "${PSQ_ARENA_EXIT_MAP}")
    # The built app loads every arena headless (dedicated server, loopback
    # UDP, scratch user root in the build tree) along the exit chain to the
    # campaign, then the test hall by name, and refuses a save in each. The
    # psQuake target is defined later, in src/.
    if(APPLE)
        add_test(NAME psq_arena_load
            COMMAND /bin/bash "${PROJECT_SOURCE_DIR}/tests/arena/arena_load_test.sh"
                "$<TARGET_BUNDLE_DIR:psQuake>"
                "${PROJECT_BINARY_DIR}/psq-arena-load-root"
                "${PSQ_ARENA_EXIT_MAP}" "${PSQ_TEST_ARENA}" ${PSQ_ARENA_CHAIN})
    else()
        list(REMOVE_ITEM _psq_arena_compiled_tests psq_arena_load)
        add_test(NAME psq_arena_load COMMAND ${CMAKE_COMMAND} -E echo
            "psq_arena_load: the app is macOS-only; skipped")
        set_tests_properties(psq_arena_load PROPERTIES DISABLED ON)
    endif()
else()
    foreach(_t IN LISTS _psq_arena_compiled_tests)
        add_test(NAME ${_t} COMMAND ${CMAKE_COMMAND}
            "-DPSQ_SKIP_MESSAGE=${_t}: no arena was built (${_psq_arena_why}); run tools/setup-build.sh"
            -P "${PROJECT_SOURCE_DIR}/tests/arena/skip.cmake")
        set_tests_properties(${_t} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
endif()
