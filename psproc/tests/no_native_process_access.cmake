# psQuake -- static check: psQuake itself never reads, renices or signals real
# processes. Summoner (summoner/, bundled in the app) is the only path to
# them. The single place allowed to call kill() is the script backend's
# teardown of its OWN child process group (a poll or respond command that
# timed out); no source may call setpriority, sysctl's KERN_PROC, proc_pidinfo
# on another pid, proc_pid_rusage or proc_listpids.
#
# Imported from psDoom's tests/no_native_process_access.cmake and adapted:
# psQuake's code is src/ (the game) and psproc/src, psproc/include (the
# shared layer, where proc_script.c lives). The engine in
# third_party/quakespasm is scanned too, for the calls that only ever reach
# another process (its sysctl reads the CPU count, so plain sysctl is allowed
# there). It is a tripwire, not a proof: it does not parse C.
#
# Run by ctest as:  cmake -DPSD_SOURCE_DIR=<repo> -P no_native_process_access.cmake

if(NOT PSD_SOURCE_DIR)
    message(FATAL_ERROR "PSD_SOURCE_DIR is not set")
endif()

file(GLOB_RECURSE _psd_sources
    "${PSD_SOURCE_DIR}/src/*.c" "${PSD_SOURCE_DIR}/src/*.h"
    "${PSD_SOURCE_DIR}/src/*.m"
    "${PSD_SOURCE_DIR}/psproc/src/*.c" "${PSD_SOURCE_DIR}/psproc/src/*.h"
    "${PSD_SOURCE_DIR}/psproc/include/*.h")
file(GLOB_RECURSE _psd_engine
    "${PSD_SOURCE_DIR}/third_party/quakespasm/Quake/*.c"
    "${PSD_SOURCE_DIR}/third_party/quakespasm/Quake/*.h"
    "${PSD_SOURCE_DIR}/third_party/quakespasm/Quake/*.m")
if(NOT _psd_sources OR NOT _psd_engine)
    message(FATAL_ERROR "no sources found under ${PSD_SOURCE_DIR}/src, psproc/ or third_party/quakespasm")
endif()

set(_psd_bad "")
foreach(_f IN LISTS _psd_sources _psd_engine)
    set(_is_engine 0)
    if(_f MATCHES "/third_party/quakespasm/")
        set(_is_engine 1)
    endif()
    file(STRINGS "${_f}" _lines)
    set(_ln 0)
    foreach(_l IN LISTS _lines)
        math(EXPR _ln "${_ln} + 1")
        # Comments are not calls (block-comment lines start with * or /*).
        if(_l MATCHES "^[ \t]*(\\*|/\\*)")
            continue()
        endif()
        string(REGEX REPLACE "/\\*.*\\*/" "" _code "${_l}")
        string(REGEX REPLACE "//.*$" "" _code "${_code}")
        if(_code MATCHES "(setpriority|proc_pid_rusage|proc_listpids|proc_listallpids|KERN_PROC|killpg|sigqueue|proc_macos|/bin/kill|bin/renice)")
            list(APPEND _psd_bad "${_f}:${_ln}: ${_l}")
        endif()
        if(NOT _is_engine AND _code MATCHES "sysctl")
            list(APPEND _psd_bad "${_f}:${_ln}: ${_l}")
        endif()
        # kill(): only the script backend's own child group teardown.
        if(_code MATCHES "(^|[^_a-zA-Z>.])kill[ \t]*\\(")
            if(NOT _f MATCHES "/psproc/src/proc_script\\.c$"
               OR NOT _code MATCHES "kill\\((-pgid|pgid), SIGKILL\\)")
                list(APPEND _psd_bad "${_f}:${_ln}: ${_l}")
            endif()
        endif()
        # proc_pidinfo: only for this process's own descriptor list.
        if(_code MATCHES "proc_pidinfo[ \t]*\\(")
            if(NOT _code MATCHES "proc_pidinfo\\(getpid\\(\\)")
                list(APPEND _psd_bad "${_f}:${_ln}: ${_l}")
            endif()
        endif()
    endforeach()
endforeach()

if(_psd_bad)
    list(JOIN _psd_bad "\n" _psd_report)
    message(FATAL_ERROR
        "psQuake code reaches real processes outside Summoner:\n${_psd_report}")
endif()
message(STATUS "no native process access in src/, psproc/ or the engine (Summoner is the only path)")
