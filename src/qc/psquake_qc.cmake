#
# psQuake game code (QuakeC). Included from src/CMakeLists.txt.
#
#   psq_fteqcc        the QuakeC compiler (third_party/fteqcc, GPL-2), a
#                     build-time host tool; never bundled.
#   psquake_progs     compiles third_party/librequake-qc/qcsrc (LibreQuake
#                     v0.09-beta with the [psQuake] hooks) plus src/qc/*.qc
#                     into ${PSQ_PROGS_DIR}/progs.dat, with LibreQuake's own
#                     flags (its build.py: -D__LIBREQUAKE__ -O3).
#
# The engine loads that progs.dat ahead of the one inside the data paks
# (PSQ_AddGameCode, src/engine/psq_sys.c).
#

set(_fteqcc_dir "${PROJECT_SOURCE_DIR}/third_party/fteqcc")

# Upstream's `make qcc` object list (engine/qclib/Makefile: QCC_OBJS,
# COMMON_OBJS, TUI_OBJS) and flags. Upstream warnings are not psQuake's to fix.
add_executable(psq_fteqcc
    "${_fteqcc_dir}/qccmain.c"
    "${_fteqcc_dir}/qcc_pr_comp.c"
    "${_fteqcc_dir}/qcc_pr_lex.c"
    "${_fteqcc_dir}/packager.c"
    "${_fteqcc_dir}/decomp.c"
    "${_fteqcc_dir}/comprout.c"
    "${_fteqcc_dir}/hash.c"
    "${_fteqcc_dir}/qcc_cmdlib.c"
    "${_fteqcc_dir}/qcd_main.c"
    "${_fteqcc_dir}/qcctui.c")
set_target_properties(psq_fteqcc PROPERTIES
    C_STANDARD 99
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tools")
target_compile_options(psq_fteqcc PRIVATE -w -O2)
find_package(ZLIB REQUIRED)   # the system zlib (macOS SDK)
target_link_libraries(psq_fteqcc PRIVATE ZLIB::ZLIB m)

set(_qcsrc "${PROJECT_SOURCE_DIR}/third_party/librequake-qc/qcsrc")
set(PSQ_PROGS_DIR "${PROJECT_BINARY_DIR}/qc")
set(PSQ_PROGS_DAT "${PSQ_PROGS_DIR}/progs.dat")
file(MAKE_DIRECTORY "${PSQ_PROGS_DIR}")
file(GLOB_RECURSE _qc_sources CONFIGURE_DEPENDS
    "${_qcsrc}/*.qc" "${_qcsrc}/progs.src"
    "${CMAKE_CURRENT_LIST_DIR}/*.qc")

# fteqcc writes only -o; it runs in its own folder so nothing else lands in
# the source tree. A failed compile leaves no stale progs.dat behind.
add_custom_command(
    OUTPUT "${PSQ_PROGS_DAT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PSQ_PROGS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E remove -f "${PSQ_PROGS_DAT}"
    COMMAND $<TARGET_FILE:psq_fteqcc>
        -src "${_qcsrc}"
        -D__LIBREQUAKE__ -O3 -Werror
        -o "${PSQ_PROGS_DAT}"
    WORKING_DIRECTORY "${PSQ_PROGS_DIR}"
    DEPENDS psq_fteqcc ${_qc_sources}
    COMMENT "Compiling psQuake QuakeC into progs.dat"
    VERBATIM)
add_custom_target(psquake_progs ALL DEPENDS "${PSQ_PROGS_DAT}")
