#
# psQuake's own host tests (psproc/tests holds the test imported from psDoom).
# The C tests link psproc_fake (PSD_NO_NATIVE_BACKEND) for every action; only
# psproc_select_test also links the native backend, built read-only.
#

add_library(psproc_options_test_obj OBJECT
    "${CMAKE_CURRENT_SOURCE_DIR}/psproc_options_test.c")
target_link_libraries(psproc_options_test_obj PRIVATE psproc_fake)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(psproc_options_test_obj PRIVATE -Wall -Wextra)
endif()
add_executable(psproc_options_test $<TARGET_OBJECTS:psproc_options_test_obj>)
target_link_libraries(psproc_options_test PRIVATE psproc_fake)
add_test(NAME psproc_options_test COMMAND psproc_options_test)

# Link-level proof that no test binary can reach the native setpriority or
# SIGTERM (nm over every object a test links; see the script). The native
# backend exists only on macOS; elsewhere the tests link no native code.
if(APPLE)
    if(NOT CMAKE_NM)
        message(FATAL_ERROR "nm not found: psproc_native_actions_unlinked needs it")
    endif()
    add_test(NAME psproc_native_actions_unlinked
        COMMAND ${CMAKE_COMMAND}
            "-DPSQ_NM=${CMAKE_NM}"
            "-DPSQ_ARCHIVE=$<TARGET_FILE:psproc_fake>"
            "-DPSQ_READONLY_OBJECTS=$<JOIN:$<TARGET_OBJECTS:psproc_macos_readonly>,|>"
            "-DPSQ_TEST_OBJECTS=$<JOIN:$<TARGET_OBJECTS:psproc_select_test_obj>,|>|$<JOIN:$<TARGET_OBJECTS:psproc_options_test_obj>,|>"
            "-DPSQ_TEST_BINARIES=$<TARGET_FILE:psproc_select_test>|$<TARGET_FILE:psproc_options_test>"
            -P "${PROJECT_SOURCE_DIR}/psproc/tests/native_actions_unlinked.cmake")
endif()

# tools/psproc-drift.sh against throwaway git repos in the build tree.
find_program(PSQ_BASH bash)
find_program(PSQ_GIT git)
if(PSQ_BASH AND PSQ_GIT)
    add_test(NAME psproc_drift_test
        COMMAND "${PSQ_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/psproc_drift_test.sh"
            "${PROJECT_SOURCE_DIR}/tools/psproc-drift.sh"
            "${CMAKE_CURRENT_BINARY_DIR}/psproc-drift-test")
endif()
