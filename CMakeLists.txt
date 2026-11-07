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
# Every C test links psproc_fake (PSD_NO_NATIVE_BACKEND: fake and script
# backends only) for its actions. On macOS psproc_select_test also lists and
# looks up real processes through a read-only build of the native backend;
# the ctest psproc_native_actions_unlinked proves with nm that no test can
# renice or SIGTERM a real process.
#

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

add_subdirectory(psproc)

add_subdirectory(tests)
