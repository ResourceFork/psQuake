#
# psproc -- the engine-free process layer shared with psDoom (imported from
# ResourceFork/psDoom at the commit in psproc/UPSTREAM; see
# docs/architecture.md and tools/psproc-drift.sh).
#
# Targets:
#   psproc       the library a game links: backend vtable, triage, script
#                backend, options and (on macOS) the native backend.
#   psproc_fake  the same layer built with PSD_NO_NATIVE_BACKEND plus the
#                in-memory fake backend, for host tests. It has no native
#                backend: nothing in it calls setpriority, and its only kill()
#                is the script backend's SIGKILL of its own unreaped poll child.
#   psproc_macos_readonly (macOS, tests only)  the native backend with its
#                setpriority/kill bound to aborting stubs, for read-only
#                listing/lookup checks.
#

set(PSPROC_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/proc_backend.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/proc_script.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/proc_select.c"
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
if(APPLE)
    target_sources(psproc PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/macos/proc_macos.c")
    target_include_directories(psproc PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src/macos")
endif()

add_library(psproc_fake STATIC ${PSPROC_CORE_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/proc_fake.c")
psproc_common_settings(psproc_fake)
target_compile_definitions(psproc_fake PUBLIC PSD_NO_NATIVE_BACKEND)

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

# Host test imported with the layer (psDoom's tests/proc_select_test.c).
add_executable(psproc_select_test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/proc_select_test.c")
psproc_common_settings(psproc_select_test)
if(APPLE)
    # The test's listing/lookup checks need the native backend, READ-ONLY:
    # proc_macos.c compiled with its setpriority and kill bound to stubs that
    # abort (tests/proc_macos_readonly.c). Actions still go to the fake.
    add_library(psproc_macos_readonly OBJECT
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/proc_macos_readonly.c")
    target_link_libraries(psproc_macos_readonly PRIVATE psproc_fake)
    psproc_common_settings(psproc_macos_readonly)
    target_include_directories(psproc_macos_readonly PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/macos")
    target_include_directories(psproc_select_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/macos")
    target_sources(psproc_select_test PRIVATE
        $<TARGET_OBJECTS:psproc_macos_readonly>)
endif()
psproc_add_host_test(psproc_select_test)

# Only the pid-reuse-checked wrappers may call the raw renice/kill ops
# (psDoom's tests/no_raw_backend_actions.cmake, scanning all of psQuake).
add_test(NAME psproc_no_raw_backend_actions
    COMMAND ${CMAKE_COMMAND} -DPSD_SOURCE_DIR=${PROJECT_SOURCE_DIR}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/no_raw_backend_actions.cmake")

# The script round-trip test writes its poll/respond scripts to $TMPDIR; keep
# them in the build tree rather than the system temp directory.
set(_PSPROC_TEST_TMP "${CMAKE_CURRENT_BINARY_DIR}/test-tmp")
file(MAKE_DIRECTORY "${_PSPROC_TEST_TMP}")
set_tests_properties(psproc_select_test PROPERTIES
    ENVIRONMENT "TMPDIR=${_PSPROC_TEST_TMP}")
