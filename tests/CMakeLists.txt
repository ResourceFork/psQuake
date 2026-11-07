#
# psQuake's own host tests (psproc/tests holds the test imported from psDoom).
# Every C host test is registered with psproc_add_host_test (psproc/), which
# links psproc_fake (PSD_NO_NATIVE_BACKEND) for every action; only
# psproc_select_test also links the native backend, built read-only.
#

add_executable(psproc_options_test
    "${CMAKE_CURRENT_SOURCE_DIR}/psproc_options_test.c")
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(psproc_options_test PRIVATE -Wall -Wextra)
endif()
psproc_add_host_test(psproc_options_test)

# Checks over every registered host test, run once the whole project has been
# configured (deferred to the end of the top-level directory on CMake 3.19+,
# so an executable or test added later in any CMakeLists.txt is still seen):
#  - every executable in psproc/ and tests/ is a registered host test, and
#    each links psproc_fake and not the native library psproc;
#  - on macOS, the ctest psproc_native_actions_unlinked: nm over every object
#    each test's linker map lists (psproc/tests/native_actions_unlinked.cmake).
#    It is a fixture every host test requires, so a test binary runs only
#    after the check passes. The native backend exists only on macOS;
#    elsewhere the tests link no native code.
function(psq_finish_host_tests)
    get_property(_tests GLOBAL PROPERTY PSPROC_HOST_TESTS)
    foreach(_dir "${PROJECT_SOURCE_DIR}/psproc" "${PROJECT_SOURCE_DIR}/tests")
        get_property(_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_t IN LISTS _targets)
            get_target_property(_type ${_t} TYPE)
            if(_type STREQUAL "EXECUTABLE" AND NOT _t IN_LIST _tests)
                message(FATAL_ERROR
                    "${_t} is an executable in ${_dir} but not registered with psproc_add_host_test")
            endif()
        endforeach()
    endforeach()
    foreach(_t IN LISTS _tests)
        get_target_property(_libs ${_t} LINK_LIBRARIES)
        if(NOT "psproc_fake" IN_LIST _libs OR "psproc" IN_LIST _libs)
            message(FATAL_ERROR
                "host test ${_t} must link psproc_fake and not psproc (links: ${_libs})")
        endif()
    endforeach()

    if(APPLE)
        if(NOT CMAKE_NM)
            message(FATAL_ERROR "nm not found: psproc_native_actions_unlinked needs it")
        endif()
        set(_bins "")
        foreach(_t IN LISTS _tests)
            list(APPEND _bins "$<TARGET_FILE:${_t}>")
        endforeach()
        list(JOIN _bins "|" _bins)
        add_test(NAME psproc_native_actions_unlinked
            COMMAND ${CMAKE_COMMAND}
                "-DPSQ_NM=${CMAKE_NM}"
                "-DPSQ_TEST_BINARIES=${_bins}"
                "-DPSQ_BUILD_DIR=${CMAKE_BINARY_DIR}"
                -P "${PROJECT_SOURCE_DIR}/psproc/tests/native_actions_unlinked.cmake")
        set_tests_properties(psproc_native_actions_unlinked PROPERTIES
            FIXTURES_SETUP psproc_native_actions_unlinked)
    endif()
endfunction()
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.19)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL psq_finish_host_tests)
else()
    psq_finish_host_tests()
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

# psQuake engine-side decisions (src/engine), linked from psq_core. They are
# registered like every host test here, so they also link psproc_fake and are
# covered by psproc_native_actions_unlinked.
foreach(_psq_test psq_data_test psq_grab_test psq_net_test)
    add_executable(${_psq_test} "${CMAKE_CURRENT_SOURCE_DIR}/${_psq_test}.c")
    target_link_libraries(${_psq_test} PRIVATE psq_core)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${_psq_test} PRIVATE -Wall -Wextra)
    endif()
    psproc_add_host_test(${_psq_test})
endforeach()
