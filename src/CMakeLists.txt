#
# psQuake's own sources (MIT).
#
#   psq_core    engine-free decisions shared by the engine and the host
#               tests: data and user-root selection, the UDP bind address,
#               and the mouse grab predicate.
#   psQuake     the app (src/app/macos): the vendored QuakeSpasm engine plus
#               the psQuake hooks, built into psQuake.app.
#

set(PSQ_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/psq_data.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/psq_grab.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/psq_net.c")

add_library(psq_core STATIC ${PSQ_CORE_SOURCES})
target_include_directories(psq_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/engine")
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(psq_core PRIVATE -Wall -Wextra)
endif()

# psq_policy: the process-monster policy behind the QuakeC builtins
# (src/core). It links no psproc library itself: the app links the native
# psproc, the host tests psproc_fake.
set(PSQ_POLICY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/core/psq_policy.c")
add_library(psq_policy STATIC ${PSQ_POLICY_SOURCES})
target_include_directories(psq_policy PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/core"
    "${PROJECT_SOURCE_DIR}/psproc/include")
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(psq_policy PRIVATE -Wall -Wextra)
endif()

#
# psQuake.app: the vendored QuakeSpasm 0.97.0 engine (third_party/quakespasm,
# GPL-2.0-or-later) with the [psQuake] hooks listed in third_party/README.md.
# There is no Cocoa launcher: the engine is the bundle executable.
#
if(NOT APPLE)
    message(STATUS "psQuake: the psQuake.app target is macOS-only; building host tests only")
    return()
endif()

# The QuakeC compiler and progs.dat (src/qc/psquake_qc.cmake), for the app.
include("${CMAKE_CURRENT_SOURCE_DIR}/qc/psquake_qc.cmake")

include("${CMAKE_CURRENT_SOURCE_DIR}/app/macos/psquake_app.cmake")
