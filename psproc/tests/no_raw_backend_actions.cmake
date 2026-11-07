# psDoom -- static check: game code acts on processes only through the
# pid-reuse-checked wrappers (psd_proc_renice_checked / psd_proc_kill_checked),
# never through the raw backend renice/kill ops, which act by pid alone.
#
# Run by ctest as:  cmake -DPSD_SOURCE_DIR=<repo> -P no_raw_backend_actions.cmake
#
# psQuake: psDoom scans its game directories (src/psdoom, src/app). psQuake
# scans every C/C++/Objective-C source in the repository, so engine and game
# code added later is covered with no list to keep. Skipped: .git, scratch/,
# any top-level directory holding a CMakeCache.txt (a build tree), symlinked
# directories (CMP0009), comment lines (starting with //, or starting with *
# or /* and holding no */) and one-line /* ... */ comments. Three rules, each a textual heuristic a
# determined author can evade (a macro, a pointer passed through a cast), so
# review still matters:
#   1. A raw backend op (->renice, ->kill, .renice, .kill, called or not) only
#      in psproc/src/proc_backend.c, exactly the one renice and one kill inside
#      psd_proc_renice_checked / psd_proc_kill_checked, and in psproc/tests/
#      (the host tests drive the raw ops on the fake and script backends;
#      psproc/tests/native_actions_unlinked.cmake proves no test binary can
#      reach the native setpriority or SIGTERM).
#   2. proc_macos_renice / proc_macos_kill (called, or taken as a pointer) only
#      in psproc/src/macos/, where they are defined.
#   3. A direct setpriority, kill, killpg or proc_terminate call only in
#      psproc/src/macos/proc_macos.c (the native actions),
#      psproc/src/proc_script.c (SIGKILL of its own poll child) and
#      psproc/tests/proc_macos_readonly.c (its aborting stubs).

cmake_minimum_required(VERSION 3.16)   # CMP0009: GLOB_RECURSE skips symlinks

if(NOT PSD_SOURCE_DIR)
    message(FATAL_ERROR "PSD_SOURCE_DIR is not set")
endif()

set(_psd_backend "${PSD_SOURCE_DIR}/psproc/src/proc_backend.c")
if(NOT EXISTS "${_psd_backend}")
    message(FATAL_ERROR "${_psd_backend} not found (is PSD_SOURCE_DIR the psQuake root?)")
endif()

# Top-level directories to skip: .git, scratch/, and build trees, found by
# their CMakeCache.txt rather than by name.
set(_psd_skip_dirs ".git" "scratch")
file(GLOB _psd_top LIST_DIRECTORIES true "${PSD_SOURCE_DIR}/*")
foreach(_d IN LISTS _psd_top)
    if(IS_DIRECTORY "${_d}" AND EXISTS "${_d}/CMakeCache.txt")
        get_filename_component(_n "${_d}" NAME)
        list(APPEND _psd_skip_dirs "${_n}")
    endif()
endforeach()

file(GLOB_RECURSE _psd_all_sources
    "${PSD_SOURCE_DIR}/*.c"
    "${PSD_SOURCE_DIR}/*.h"
    "${PSD_SOURCE_DIR}/*.cc"
    "${PSD_SOURCE_DIR}/*.cpp"
    "${PSD_SOURCE_DIR}/*.hpp"
    "${PSD_SOURCE_DIR}/*.m"
    "${PSD_SOURCE_DIR}/*.mm")

set(_psd_game_sources "")
foreach(_f IN LISTS _psd_all_sources)
    file(RELATIVE_PATH _rel "${PSD_SOURCE_DIR}" "${_f}")
    if(_rel MATCHES "^([^/]+)/")
        if(CMAKE_MATCH_1 IN_LIST _psd_skip_dirs)
            continue()
        endif()
    endif()
    list(APPEND _psd_game_sources "${_f}")
endforeach()

if(NOT _psd_game_sources)
    message(FATAL_ERROR "no sources found under ${PSD_SOURCE_DIR}")
endif()

# Code lines of `file` that match `regex`. ';', '[' and ']' (list syntax in
# CMake) become %3B, %5B and %5D first, so each line stays one list entry.
function(psd_grep file regex out)
    set(_hits "")
    if(EXISTS "${file}")   # it may have been removed since the glob
        file(READ "${file}" _text)
        string(REPLACE ";" "%3B" _text "${_text}")
        string(REPLACE "[" "%5B" _text "${_text}")
        string(REPLACE "]" "%5D" _text "${_text}")
        string(REPLACE "\n" ";" _lines "${_text}")
        foreach(_l IN LISTS _lines)
            if(_l MATCHES "^[ \t]*//")
                continue()
            endif()
            if(_l MATCHES "^[ \t]*/?\\*" AND NOT _l MATCHES "\\*/")
                continue()
            endif()
            string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" _l "${_l}")
            if(_l MATCHES "${regex}")
                list(APPEND _hits "${_l}")
            endif()
        endforeach()
    endif()
    set(${out} "${_hits}" PARENT_SCOPE)
endfunction()

set(_psd_raw_re "(->[ \t]*|\\.)(renice|kill)([^A-Za-z0-9_]|$)")
set(_psd_native_re "proc_macos_(renice|kill)([^A-Za-z0-9_]|$)")
set(_psd_sys_re "(^|[^A-Za-z0-9_>.])(setpriority|kill|killpg|proc_terminate)[ \t]*\\(")
set(_psd_sys_ok
    "psproc/src/macos/proc_macos.c"
    "psproc/src/proc_script.c"
    "psproc/tests/proc_macos_readonly.c")

set(_psd_bad "")
foreach(_f IN LISTS _psd_game_sources)
    file(RELATIVE_PATH _rel "${PSD_SOURCE_DIR}" "${_f}")

    # Rule 1.
    psd_grep("${_f}" "${_psd_raw_re}" _hits)
    if(_rel STREQUAL "psproc/src/proc_backend.c")
        set(_nr 0)
        set(_nk 0)
        foreach(_h IN LISTS _hits)
            if(_h MATCHES "^[ \t]*b->renice\\(pid, nice_delta\\)%3B$")
                math(EXPR _nr "${_nr} + 1")
            elseif(_h MATCHES "^[ \t]*return b->kill\\(pid\\) \\? PSD_ACT_DONE : PSD_ACT_FAILED%3B$")
                math(EXPR _nk "${_nk} + 1")
            else()
                list(APPEND _psd_bad "${_rel}: ${_h}")
            endif()
        endforeach()
        if(NOT _nr EQUAL 1 OR NOT _nk EQUAL 1)
            list(APPEND _psd_bad
                "${_rel}: expected exactly one raw renice and one raw kill (in the checked wrappers), found ${_nr} and ${_nk}")
        endif()
    elseif(NOT _rel MATCHES "^psproc/tests/")
        foreach(_h IN LISTS _hits)
            list(APPEND _psd_bad "${_rel}: ${_h}")
        endforeach()
    endif()

    # Rule 2.
    if(NOT _rel MATCHES "^psproc/src/macos/")
        psd_grep("${_f}" "${_psd_native_re}" _hits)
        foreach(_h IN LISTS _hits)
            list(APPEND _psd_bad "${_rel}: ${_h}")
        endforeach()
    endif()

    # Rule 3.
    if(NOT _rel IN_LIST _psd_sys_ok)
        psd_grep("${_f}" "${_psd_sys_re}" _hits)
        foreach(_h IN LISTS _hits)
            list(APPEND _psd_bad "${_rel}: ${_h}")
        endforeach()
    endif()
endforeach()

if(_psd_bad)
    list(JOIN _psd_bad "\n" _psd_report)
    string(REPLACE "%3B" ";" _psd_report "${_psd_report}")
    string(REPLACE "%5B" "[" _psd_report "${_psd_report}")
    string(REPLACE "%5D" "]" _psd_report "${_psd_report}")
    message(FATAL_ERROR
        "process action outside the checked wrappers (use psd_proc_*_checked):\n"
        "${_psd_report}")
endif()

list(LENGTH _psd_game_sources _psd_n)
message(STATUS "no raw backend renice/kill calls in ${_psd_n} source files")
