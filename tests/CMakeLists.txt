#
# psQuake's own host tests (psproc/tests holds the test imported from psDoom).
# The C tests link psproc_fake (PSD_NO_NATIVE_BACKEND), never the native
# backend.
#

add_executable(psproc_options_test
    "${CMAKE_CURRENT_SOURCE_DIR}/psproc_options_test.c")
target_link_libraries(psproc_options_test PRIVATE psproc_fake)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(psproc_options_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME psproc_options_test COMMAND psproc_options_test)

# tools/psproc-drift.sh against throwaway git repos in the build tree.
find_program(PSQ_BASH bash)
find_program(PSQ_GIT git)
if(PSQ_BASH AND PSQ_GIT)
    add_test(NAME psproc_drift_test
        COMMAND "${PSQ_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/psproc_drift_test.sh"
            "${PROJECT_SOURCE_DIR}/tools/psproc-drift.sh"
            "${CMAKE_CURRENT_BINARY_DIR}/psproc-drift-test")
endif()
