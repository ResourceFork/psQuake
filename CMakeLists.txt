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
# Every test links psproc_fake (PSD_NO_NATIVE_BACKEND), so none can renice or
# signal a real process.
#

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

add_subdirectory(psproc)
