# psDoom -- static check: game code acts on processes only through the
# pid-reuse-checked wrappers (psd_proc_renice_checked / psd_proc_kill_checked),
# never through the raw backend renice/kill ops, which act by pid alone.
#
# Run by ctest as:  cmake -DPSD_SOURCE_DIR=<repo> -P no_raw_backend_actions.cmake
#
# psQuake: psDoom scans its game directories (src/psdoom, src/app). psQuake
# scans every C/C++/Objective-C source in the repository (build trees, scratch/
# and .git excluded), so engine and game code added later is covered with no
# list to keep. Two calls are allowed:
#   - psproc/src/proc_backend.c: exactly one raw renice and one raw kill, the
#     ones inside psd_proc_renice_checked / psd_proc_kill_checked;
#   - psproc/tests/: the host tests drive the raw ops on the fake and script
#     backends (psproc/tests/native_actions_unlinked.cmake proves no test
#     binary can reach the native setpriority or SIGTERM).
# The native action functions (proc_macos_renice / proc_macos_kill) may appear
# only in psproc/src/macos/, where they are defined.

cmake_minimum_required(VERSION 3.16)   # CMP0009: GLOB_RECURSE skips symlinks

if(NOT PSD_SOURCE_DIR)
    message(FATAL_ERROR "PSD_SOURCE_DIR is not set")
endif()

set(_psd_backend "${PSD_SOURCE_DIR}/psproc/src/proc_backend.c")
if(NOT EXISTS "${_psd_backend}")
    message(FATAL_ERROR "${_psd_backend} not found (is PSD_SOURCE_DIR the psQuake root?)")
endif()

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
    if(_rel MATCHES "^(build[^/]*|scratch|\\.git)/")
        continue()
    endif()
    list(APPEND _psd_game_sources "${_f}")
endforeach()

if(NOT _psd_game_sources)
    message(FATAL_ERROR "no sources found under ${PSD_SOURCE_DIR}")
endif()

# Lines of `file` that match `regex`. ';', '[' and ']' (list syntax in CMake)
# become %3B, %5B and %5D first, so each line stays one list entry.
function(psd_grep file regex out)
    file(READ "${file}" _text)
    string(REPLACE ";" "%3B" _text "${_text}")
    string(REPLACE "[" "%5B" _text "${_text}")
    string(REPLACE "]" "%5D" _text "${_text}")
    string(REPLACE "\n" ";" _lines "${_text}")
    set(_hits "")
    foreach(_l IN LISTS _lines)
        if(_l MATCHES "${regex}")
            list(APPEND _hits "${_l}")
        endif()
    endforeach()
    set(${out} "${_hits}" PARENT_SCOPE)
endfunction()

set(_psd_raw_re "(->|\\.)[ \t]*(renice|kill)[ \t]*\\(")
set(_psd_native_re "proc_macos_(renice|kill)[ \t]*\\(")

set(_psd_bad "")
foreach(_f IN LISTS _psd_game_sources)
    file(RELATIVE_PATH _rel "${PSD_SOURCE_DIR}" "${_f}")

    psd_grep("${_f}" "${_psd_raw_re}" _hits)
    if(_rel STREQUAL "psproc/src/proc_backend.c")
        # Only the two calls inside the checked wrappers.
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

    if(NOT _rel MATCHES "^psproc/src/macos/")
        psd_grep("${_f}" "${_psd_native_re}" _hits)
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
        "raw backend renice/kill call outside the checked wrappers (use psd_proc_*_checked):\n"
        "${_psd_report}")
endif()

list(LENGTH _psd_game_sources _psd_n)
message(STATUS "no raw backend renice/kill calls in ${_psd_n} source files")
