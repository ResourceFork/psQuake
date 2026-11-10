#
# psQuake arena map (maps/psqarena.bsp), included from the top-level
# CMakeLists.txt before src/.
#
#   tools/generate_arena.py   writes psqarena.map and psqarena.wad (textures
#                             copied from LibreQuake's pak0.pak)
#   tools/compile_arena.py    runs it, then ericw-tools qbsp, vis and light
#   tools/validate_bsp.py     checks the result; a failing map is never used
#
# Needs LibreQuake (data/librequake) and the pinned ericw-tools build in
# data/ericw-tools/bin; tools/setup-build.sh provides both, and its --check,
# run here at every configure, confirms the compilers still match their pin
# (data/ericw-tools/STAMP: tag, commit, Embree/TBB versions, binary hashes).
# Without them the app builds with no arena, New Game opens the stock start
# map, and the arena's compiled-map tests report Skipped.
#
# Sets PSQ_ARENA_BSP to the validated map, or to "" when it is not built.
#
# PSQ_ARENA_LAYOUT picks the hall: "hall" (the default) is the arena the app
# ships; "terraces", "pit" and "towers" are the multi-level prototypes
# (generate_arena.py --layout), built only to be looked at and compared.
#

find_package(Python3 3.9 COMPONENTS Interpreter)

set(PSQ_ERICW_DIR "${PROJECT_SOURCE_DIR}/data/ericw-tools")
set(PSQ_ERICW_BIN "${PSQ_ERICW_DIR}/bin")
set(PSQ_ARENA_PAK "${PROJECT_SOURCE_DIR}/data/librequake/id1/pak0.pak")
set(PSQ_ARENA_EXIT_MAP "lq_e1m1")
set(PSQ_ARENA_TOOLS
    "${PROJECT_SOURCE_DIR}/tools/generate_arena.py"
    "${PROJECT_SOURCE_DIR}/tools/compile_arena.py"
    "${PROJECT_SOURCE_DIR}/tools/validate_bsp.py")
set(PSQ_ARENA_BSP "")
set(PSQ_ARENA_LAYOUT "hall" CACHE STRING
    "Arena hall layout: hall (shipped), or a prototype: terraces, pit, towers")
set_property(CACHE PSQ_ARENA_LAYOUT PROPERTY STRINGS hall terraces pit towers)
if(NOT PSQ_ARENA_LAYOUT MATCHES "^(hall|terraces|pit|towers)$")
    message(FATAL_ERROR "PSQ_ARENA_LAYOUT is ${PSQ_ARENA_LAYOUT}: hall, terraces, pit or towers")
endif()

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
    set(_work "${_out}/work")
    set(PSQ_ARENA_BSP "${_out}/psqarena.bsp")
    # A changed layout must rebuild the map: its name sits in a stamp the
    # command depends on, rewritten only when it changes.
    set(_layout_stamp "${_out}/layout.txt")
    set(_old_layout "")
    if(EXISTS "${_layout_stamp}")
        file(READ "${_layout_stamp}" _old_layout)
    endif()
    if(NOT _old_layout STREQUAL "${PSQ_ARENA_LAYOUT}\n")
        file(WRITE "${_layout_stamp}" "${PSQ_ARENA_LAYOUT}\n")
    endif()
    add_custom_command(
        OUTPUT "${PSQ_ARENA_BSP}"
        COMMAND ${CMAKE_COMMAND} -E rm -f "${PSQ_ARENA_BSP}"
        COMMAND /bin/bash "${PROJECT_SOURCE_DIR}/tools/setup-build.sh" --check --no-data
        COMMAND ${Python3_EXECUTABLE} -I -B "${PROJECT_SOURCE_DIR}/tools/compile_arena.py"
            --tools "${PSQ_ERICW_BIN}" --pak "${PSQ_ARENA_PAK}" --out "${_work}"
            --layout "${PSQ_ARENA_LAYOUT}"
        COMMAND ${Python3_EXECUTABLE} -I -B "${PROJECT_SOURCE_DIR}/tools/validate_bsp.py"
            --bsp "${_work}/psqarena.bsp" --map "${_work}/psqarena.map"
            --exit-map "${PSQ_ARENA_EXIT_MAP}"
        COMMAND ${CMAKE_COMMAND} -E copy "${_work}/psqarena.bsp" "${PSQ_ARENA_BSP}"
        DEPENDS ${PSQ_ARENA_TOOLS} "${PROJECT_SOURCE_DIR}/tools/setup-build.sh"
            "${PSQ_ARENA_PAK}" "${PSQ_ERICW_DIR}/STAMP" "${_layout_stamp}"
            "${PSQ_ERICW_BIN}/qbsp" "${PSQ_ERICW_BIN}/vis" "${PSQ_ERICW_BIN}/light"
        COMMENT "Generating, compiling and validating the psQuake arena (${PSQ_ARENA_LAYOUT})"
        VERBATIM)
    add_custom_target(psquake_arena ALL DEPENDS "${PSQ_ARENA_BSP}")
    message(STATUS "psQuake: will build the arena with the pinned ericw-tools in ${PSQ_ERICW_BIN}")
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
            "${PROJECT_SOURCE_DIR}/tools" "${PSQ_ARENA_BSP}")
endif()
# The engine call sites of the arena rules (menu.c, sv_main.c, host_cmd.c).
add_test(NAME psq_arena_hooks_wired
    COMMAND ${CMAKE_COMMAND}
        "-DPSQ_QUAKE_DIR=${PROJECT_SOURCE_DIR}/third_party/quakespasm/Quake"
        -P "${PROJECT_SOURCE_DIR}/tests/arena/arena_hooks_wired.cmake")

set(_psq_arena_compiled_tests psq_arena_validate psq_arena_rejects_broken psq_arena_load)
if(PSQ_ARENA_BSP)
    add_test(NAME psq_arena_validate
        COMMAND ${Python3_EXECUTABLE} -I -B "${PROJECT_SOURCE_DIR}/tools/validate_bsp.py"
            --bsp "${PSQ_ARENA_BSP}" --exit-map "${PSQ_ARENA_EXIT_MAP}")
    add_test(NAME psq_arena_rejects_broken
        COMMAND ${Python3_EXECUTABLE} -I -B
            "${PROJECT_SOURCE_DIR}/tests/arena/test_rejects_broken.py"
            --tools-dir "${PROJECT_SOURCE_DIR}/tools" --ericw "${PSQ_ERICW_BIN}"
            --pak "${PSQ_ARENA_PAK}" --work "${PROJECT_BINARY_DIR}/arena-broken"
            --exit-map "${PSQ_ARENA_EXIT_MAP}")
    # The built app loads the arena headless (dedicated server, loopback UDP,
    # scratch user root in the build tree), refuses a save there, and changes
    # level to the exit map. The psQuake target is defined later, in src/.
    if(APPLE)
        add_test(NAME psq_arena_load
            COMMAND /bin/bash "${PROJECT_SOURCE_DIR}/tests/arena/arena_load_test.sh"
                "$<TARGET_BUNDLE_DIR:psQuake>"
                "${PROJECT_BINARY_DIR}/psq-arena-load-root"
                "${PSQ_ARENA_EXIT_MAP}")
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
