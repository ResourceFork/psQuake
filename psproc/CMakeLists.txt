#
# psproc -- the engine-free process layer shared with psDoom (imported from
# ResourceFork/psDoom at the commit in psproc/UPSTREAM; see
# docs/architecture.md and tools/psproc-drift.sh).
#
# There is no native process backend: Summoner (summoner/, bundled in the
# app) is the only path to real processes, through the script backend
# (proc_script.c). With no Summoner the backend is proc_none_backend, which
# lists and acts on nothing.
#
# Targets:
#   psproc       the library the game links: backend vtable, selection,
#                script backend, grade mapping and options.
#   psproc_fake  the same layer plus the in-memory fake backend, for host
#                tests.
#

set(PSPROC_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/proc_backend.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/proc_script.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/proc_select.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/psproc_grade.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/psproc_options.c")

function(psproc_common_settings target)
    target_include_directories(${target} PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
    if(NOT APPLE)
        # strcasestr / strtok_r outside the Darwin default namespace.
        target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
    endif()
endfunction()

add_library(psproc STATIC ${PSPROC_CORE_SOURCES})
psproc_common_settings(psproc)

add_library(psproc_fake STATIC ${PSPROC_CORE_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/proc_fake.c")
psproc_common_settings(psproc_fake)

# psproc_add_host_test(<target>): register an executable as a C host test.
# Every C host test goes through this (tests/CMakeLists.txt fails the
# configure for an executable in psproc/ or tests/ that does not). It links
# psproc_fake for actions, adds the ctest, asks the linker for a map of every
# object it linked (<binary>.map, read by psproc/tests/native_actions_unlinked.cmake)
# and makes the test require the psproc_native_actions_unlinked fixture, so
# it runs only after that nm check passes.
function(psproc_add_host_test target)
    target_link_libraries(${target} PRIVATE psproc_fake)
    add_test(NAME ${target} COMMAND ${target})
    if(APPLE)
        target_link_options(${target} PRIVATE
            "LINKER:-map,$<TARGET_FILE:${target}>.map")
        set_tests_properties(${target} PROPERTIES
            FIXTURES_REQUIRED psproc_native_actions_unlinked)
    endif()
    set_property(GLOBAL APPEND PROPERTY PSPROC_HOST_TESTS ${target})
endfunction()

# Host tests imported with the layer (psDoom's tests/proc_select_test.c and
# the script, grade and Summoner tests it runs in the same binary).
find_package(Python3 3.9 COMPONENTS Interpreter)
add_executable(psproc_select_test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/proc_select_test.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/script_grade_test.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/script_request_test.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/summoner_bundle_test.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/summoner_e2e_test.c")
psproc_common_settings(psproc_select_test)
set(_PSPROC_E2E_PYTHON "${Python3_EXECUTABLE}")
if(NOT Python3_Interpreter_FOUND)
    set(_PSPROC_E2E_PYTHON "/nonexistent/python3")
endif()
target_compile_definitions(psproc_select_test PRIVATE
    # summoner_e2e_test.c: run summoner/ through the script backend on a CSV
    # it writes under the build tree (never the home directory).
    PSD_PYTHON3="${_PSPROC_E2E_PYTHON}"
    PSD_SUMMONER_DIR="${PROJECT_SOURCE_DIR}/summoner"
    PSD_E2E_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/summoner-e2e")
psproc_add_host_test(psproc_select_test)

# Only the pid-reuse-checked wrappers may call the raw renice/kill ops
# (psDoom's tests/no_raw_backend_actions.cmake, scanning all of psQuake).
add_test(NAME psproc_no_raw_backend_actions
    COMMAND ${CMAKE_COMMAND} -DPSD_SOURCE_DIR=${PROJECT_SOURCE_DIR}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/no_raw_backend_actions.cmake")

# psQuake never reads, renices or signals a real process itself: Summoner is
# the only path (static scan of src/, psproc/ and the engine).
add_test(NAME psproc_no_native_process_access
    COMMAND ${CMAKE_COMMAND} -DPSD_SOURCE_DIR=${PROJECT_SOURCE_DIR}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/no_native_process_access.cmake")

# Summoner on its own (Python standard library only), with an explicit
# scratch root in the build tree.
if(Python3_Interpreter_FOUND)
    add_test(NAME psq_summoner_tests
        COMMAND ${Python3_EXECUTABLE} -I
                "${PROJECT_SOURCE_DIR}/summoner/tests/run_tests.py"
                --scratch "${CMAKE_CURRENT_BINARY_DIR}/summoner-tests")
endif()

# The script round-trip test writes its poll/respond scripts to $TMPDIR; keep
# them in the build tree rather than the system temp directory. HOME is a
# scratch folder too, and SUMMONER_ROOT is cleared, so no Summoner run in the
# test can reach ~/.config/summoner.
set(_PSPROC_TEST_TMP "${CMAKE_CURRENT_BINARY_DIR}/test-tmp")
file(MAKE_DIRECTORY "${_PSPROC_TEST_TMP}" "${_PSPROC_TEST_TMP}/home")
set_tests_properties(psproc_select_test PROPERTIES
    ENVIRONMENT "TMPDIR=${_PSPROC_TEST_TMP};HOME=${_PSPROC_TEST_TMP}/home;SUMMONER_ROOT=")
