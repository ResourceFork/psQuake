# psQuake -- link-level proof that no host test can reach the native backend's
# process actions: setpriority(2) (proc_macos_renice) or kill(2) with SIGTERM
# (proc_macos_kill).
#
# Every test links psproc_fake (PSD_NO_NATIVE_BACKEND), whose action ops only
# log. psproc_select_test also links the native backend's READ-ONLY build
# (psproc/tests/proc_macos_readonly.c) for its listing/lookup checks. This
# script reads the symbol tables with nm and fails unless:
#   1. no archive member, object or test binary references setpriority;
#   2. the only object that references kill is psproc_fake's proc_script.c.o
#      (the script backend's SIGKILL of its own unreaped poll child); the
#      read-only native object and every test's own object reference none;
#   3. psproc_fake carries no native backend object (proc_macos*);
#   4. the read-only object really is the native backend: it defines
#      proc_macos_lookup and proc_macos_kill, so the checks above examined it.
#
# Run by ctest as:
#   cmake -DPSQ_NM=<nm> -DPSQ_ARCHIVE=<libpsproc_fake.a>
#         -DPSQ_READONLY_OBJECTS=<obj|...> -DPSQ_TEST_OBJECTS=<obj|...>
#         -DPSQ_TEST_BINARIES=<exe|...> -P native_actions_unlinked.cmake
# ('|'-separated lists).

cmake_minimum_required(VERSION 3.16)

foreach(_v PSQ_NM PSQ_ARCHIVE PSQ_READONLY_OBJECTS PSQ_TEST_OBJECTS PSQ_TEST_BINARIES)
    if(NOT ${_v})
        message(FATAL_ERROR "${_v} is not set")
    endif()
endforeach()
foreach(_v PSQ_READONLY_OBJECTS PSQ_TEST_OBJECTS PSQ_TEST_BINARIES)
    string(REPLACE "|" ";" ${_v} "${${_v}}")
endforeach()

set(_bad "")

# Undefined symbols of `file`, one per list entry, with the leading '_' of the
# Mach-O C namespace removed.
function(psq_undefined file out)
    execute_process(COMMAND "${PSQ_NM}" -u "${file}"
        OUTPUT_VARIABLE _o ERROR_VARIABLE _e RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "nm -u ${file} failed (${_rc}): ${_e}")
    endif()
    string(REGEX REPLACE "\n$" "" _o "${_o}")
    string(REPLACE "\n" ";" _lines "${_o}")
    set(_syms "")
    foreach(_l IN LISTS _lines)
        string(STRIP "${_l}" _l)
        string(REGEX REPLACE "^U[ \t]+" "" _l "${_l}")
        string(REGEX REPLACE "^_" "" _l "${_l}")
        if(_l)
            list(APPEND _syms "${_l}")
        endif()
    endforeach()
    set(${out} "${_syms}" PARENT_SCOPE)
endfunction()

# 1 and 2 for single objects and binaries.
foreach(_f IN LISTS PSQ_READONLY_OBJECTS PSQ_TEST_OBJECTS PSQ_TEST_BINARIES)
    if(NOT EXISTS "${_f}")
        message(FATAL_ERROR "missing ${_f}")
    endif()
    psq_undefined("${_f}" _u)
    if("setpriority" IN_LIST _u)
        list(APPEND _bad "${_f} references setpriority")
    endif()
    if(NOT _f IN_LIST PSQ_TEST_BINARIES AND "kill" IN_LIST _u)
        list(APPEND _bad "${_f} references kill")
    endif()
endforeach()

# 1, 2 and 3 for the archive, member by member. `nm -u lib.a` prints a
# "member.o:" (llvm-nm, Apple nm) or "lib.a(member.o):" (some others) header
# before each member's symbols.
execute_process(COMMAND "${PSQ_NM}" -u "${PSQ_ARCHIVE}"
    OUTPUT_VARIABLE _o ERROR_VARIABLE _e RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "nm -u ${PSQ_ARCHIVE} failed (${_rc}): ${_e}")
endif()
string(REPLACE "\n" ";" _lines "${_o}")
set(_member "")
set(_members "")
set(_kill_members "")
foreach(_l IN LISTS _lines)
    if(_l MATCHES "^([^ \t]*\\()?([^ \t():]+)\\)?:$")
        set(_member "${CMAKE_MATCH_2}")
        list(APPEND _members "${_member}")
        continue()
    endif()
    string(STRIP "${_l}" _l)
    string(REGEX REPLACE "^U[ \t]+" "" _l "${_l}")
    if(_l STREQUAL "_setpriority" OR _l STREQUAL "setpriority")
        list(APPEND _bad "${PSQ_ARCHIVE}(${_member}) references setpriority")
    elseif(_l STREQUAL "_kill" OR _l STREQUAL "kill")
        list(APPEND _kill_members "${_member}")
    endif()
endforeach()
if(NOT _members)
    list(APPEND _bad "no members read from ${PSQ_ARCHIVE}")
endif()
foreach(_m IN LISTS _members)
    if(_m MATCHES "^proc_macos")
        list(APPEND _bad "${PSQ_ARCHIVE} carries the native backend (${_m})")
    endif()
endforeach()
if(NOT _kill_members STREQUAL "proc_script.c.o")
    list(APPEND _bad
        "kill referenced by [${_kill_members}] in ${PSQ_ARCHIVE}; only proc_script.c.o may (SIGKILL of its own poll child)")
endif()

# 4: the read-only object is the native backend.
foreach(_f IN LISTS PSQ_READONLY_OBJECTS)
    execute_process(COMMAND "${PSQ_NM}" -g "${_f}"
        OUTPUT_VARIABLE _o RESULT_VARIABLE _rc)
    foreach(_s proc_macos_lookup proc_macos_kill proc_macos_renice)
        if(NOT _o MATCHES "[ \t]T[ \t]+_?${_s}\n")
            list(APPEND _bad "${_f} does not define ${_s}; is it the native backend?")
        endif()
    endforeach()
endforeach()

if(_bad)
    list(JOIN _bad "\n" _report)
    message(FATAL_ERROR "a host test can reach a native process action:\n${_report}")
endif()

list(LENGTH PSQ_TEST_BINARIES _n)
message(STATUS
    "no setpriority in ${_n} test binaries; kill only in proc_script.c.o; "
    "native backend linked read-only")
