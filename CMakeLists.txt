cmake_minimum_required(VERSION 3.16)

project(psQuake C)

#
# psQuake build. Phase 0 builds only the shared process layer (psproc/) and
# its host tests; the engine, game data and QuakeC arrive in later phases
# (docs/architecture.md).
#
#   cmake -S . -B build
#   cmake --build build -j
#   ctest --test-dir build --output-on-failure
#
# Every C test is registered with psproc_add_host_test and links psproc_fake
# (fake and script backends; there is no native backend: Summoner, bundled
# in the app, is the only path to real processes) for its actions. The ctest
# psproc_native_actions_unlinked reads each test's linker map and checks
# every linked object with nm: no setpriority, and no process action outside
# the script backend. The tests run only after it passes.
#

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

add_subdirectory(psproc)

# The arena map New Game opens (generated, compiled and validated).
include(tools/arena.cmake)

add_subdirectory(src)

add_subdirectory(tests)
