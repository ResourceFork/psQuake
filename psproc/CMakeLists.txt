#
# psproc -- the engine-free process layer shared with psDoom (imported from
# ResourceFork/psDoom 436ba83; see docs/architecture.md and
# tools/psproc-drift.sh).
#
# Targets:
#   psproc       the library a game links: backend vtable, triage, script
#                backend, options and (on macOS) the native backend.
#   psproc_fake  the same layer built with PSD_NO_NATIVE_BACKEND plus the
#                in-memory fake backend, for host tests. It contains no code
#                that can renice or signal a real process.
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

# Host test imported with the layer (psDoom's tests/proc_select_test.c).
add_executable(psproc_select_test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/proc_select_test.c")
target_link_libraries(psproc_select_test PRIVATE psproc_fake)
psproc_common_settings(psproc_select_test)
add_test(NAME psproc_select_test COMMAND psproc_select_test)

# The script round-trip test writes its poll/respond scripts to $TMPDIR; keep
# them in the build tree rather than the system temp directory.
set(_PSPROC_TEST_TMP "${CMAKE_CURRENT_BINARY_DIR}/test-tmp")
file(MAKE_DIRECTORY "${_PSPROC_TEST_TMP}")
set_tests_properties(psproc_select_test PROPERTIES
    ENVIRONMENT "TMPDIR=${_PSPROC_TEST_TMP}")
