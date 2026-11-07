# psQuake -- link-level proof that no host test can reach the native backend's
# process actions: setpriority(2) (proc_macos_renice) or kill(2) with SIGTERM
# (proc_macos_kill), or any other way to act on a process.
#
# Every host test is registered with psproc_add_host_test (psproc/
# CMakeLists.txt), which links psproc_fake for actions and makes the linker
# write <binary>.map, the list of every object it linked. psproc_select_test
# also links the native backend's READ-ONLY build
# (psproc/tests/proc_macos_readonly.c). For each test binary this script reads
# its map and checks every linked object with nm. It fails unless:
#   1. the map exists, is current, and lists at least one object;
#   2. no object, and not the binary, references setpriority;
#   3. no object links the real native backend (an object named proc_macos*
#      other than proc_macos_readonly.c.o);
#   4. a read-only native object references only the symbols in
#      _psq_readonly_allowed (an allowlist: a new upstream call such as
#      proc_terminate fails here until reviewed) and defines
#      proc_macos_lookup, proc_macos_kill and proc_macos_renice, so it is the
#      native backend that was checked;
#   5. no other object references a process action (_psq_acts: kill, killpg,
#      proc_terminate, exec*, system, ...), except psproc_fake's
#      proc_script.c.o, the script backend, which runs the user's responder
#      and SIGKILLs its own unreaped poll child.
#
# Run by ctest as:
#   cmake -DPSQ_NM=<nm> -DPSQ_TEST_BINARIES=<exe|exe|...>
#         -P native_actions_unlinked.cmake

cmake_minimum_required(VERSION 3.16)

foreach(_v PSQ_NM PSQ_TEST_BINARIES)
    if(NOT ${_v})
        message(FATAL_ERROR "${_v} is not set")
    endif()
endforeach()
string(REPLACE "|" ";" PSQ_TEST_BINARIES "${PSQ_TEST_BINARIES}")

# Undefined symbols proc_macos.c may use (leading '_'s removed).
set(_psq_readonly_allowed
    error memcpy_chk memset_chk stack_chk_fail stack_chk_guard
    stderrp strncpy_chk chkstk_darwin
    abort bzero calloc clock_gettime fprintf free getpriority getuid
    mach_timebase_info malloc memcpy memmove memset proc_pid_rusage realloc
    strlen strncpy sysctl)

# Calls that act on another process.
set(_psq_acts
    kill killpg proc_terminate sigqueue pthread_kill system popen
    posix_spawn posix_spawnp execl execle execlp execv execve execvp execvP)

set(_bad "")

# nm -u output -> list of symbols, leading '_'s removed (REGEX REPLACE with
# '^' strips them all). Archive member
# headers ("member.o:" or "lib.a(member.o):") are dropped; `member`, when set,
# keeps only that member's symbols.
function(psq_nm_u file member out)
    execute_process(COMMAND "${PSQ_NM}" -u "${file}"
        OUTPUT_VARIABLE _o ERROR_VARIABLE _e RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "nm -u ${file} failed (${_rc}): ${_e}")
    endif()
    string(REPLACE "\n" ";" _lines "${_o}")
    set(_cur "")
    set(_seen 0)
    set(_syms "")
    foreach(_l IN LISTS _lines)
        if(_l MATCHES "^([^ \t]*\\()?([^ \t():]+)\\)?:$")
            set(_cur "${CMAKE_MATCH_2}")
            if(_cur STREQUAL member)
                set(_seen 1)
            endif()
            continue()
        endif()
        if(member AND NOT _cur STREQUAL member)
            continue()
        endif()
        string(STRIP "${_l}" _l)
        string(REGEX REPLACE "^U[ \t]+" "" _l "${_l}")
        string(REGEX REPLACE "^_" "" _l "${_l}")
        if(_l)
            list(APPEND _syms "${_l}")
        endif()
    endforeach()
    if(member AND NOT _seen)
        message(FATAL_ERROR "member ${member} not found in ${file}")
    endif()
    set(${out} "${_syms}" PARENT_SCOPE)
endfunction()

foreach(_bin IN LISTS PSQ_TEST_BINARIES)
    if(NOT EXISTS "${_bin}")
        message(FATAL_ERROR "missing test binary ${_bin}")
    endif()
    set(_map "${_bin}.map")
    if(NOT EXISTS "${_map}")
        list(APPEND _bad "${_bin}: no linker map (register it with psproc_add_host_test)")
        continue()
    endif()
    file(TIMESTAMP "${_bin}" _tb "%s")
    file(TIMESTAMP "${_map}" _tm "%s")
    math(EXPR _age "${_tb} - ${_tm}")
    if(_age GREATER 5)
        list(APPEND _bad "${_map} is older than its binary (stale map)")
    endif()

    psq_nm_u("${_bin}" "" _u)
    if("setpriority" IN_LIST _u)
        list(APPEND _bad "${_bin} references setpriority")
    endif()

    # "[  n] path" lines between "# Object files:" and the next "# " header.
    file(STRINGS "${_map}" _lines)
    get_filename_component(_bindir "${_bin}" DIRECTORY)
    set(_in 0)
    set(_nobj 0)
    foreach(_l IN LISTS _lines)
        if(_l STREQUAL "# Object files:")
            set(_in 1)
            continue()
        elseif(_l MATCHES "^# ")
            set(_in 0)
        endif()
        if(NOT _in OR NOT _l MATCHES "^\\[ *[0-9]+\\] (.+)$")
            continue()
        endif()
        set(_path "${CMAKE_MATCH_1}")
        if(_path STREQUAL "linker synthesized" OR _path MATCHES "\\.(tbd|dylib)$")
            continue()
        endif()
        set(_member "")
        if(_path MATCHES "^(.+)\\(([^()]+)\\)$")
            set(_path "${CMAKE_MATCH_1}")
            set(_member "${CMAKE_MATCH_2}")
        endif()
        if(NOT IS_ABSOLUTE "${_path}")
            if(EXISTS "${_bindir}/${_path}")
                set(_path "${_bindir}/${_path}")
            elseif(PSQ_BUILD_DIR AND EXISTS "${PSQ_BUILD_DIR}/${_path}")
                set(_path "${PSQ_BUILD_DIR}/${_path}")
            endif()
        endif()
        if(NOT EXISTS "${_path}")
            list(APPEND _bad "${_map}: linked object not found: ${_path}")
            continue()
        endif()
        math(EXPR _nobj "${_nobj} + 1")
        if(_member)
            set(_name "${_member}")
            set(_label "${_path}(${_member})")
        else()
            get_filename_component(_name "${_path}" NAME)
            set(_label "${_path}")
        endif()
        psq_nm_u("${_path}" "${_member}" _u)

        if("setpriority" IN_LIST _u)
            list(APPEND _bad "${_label} references setpriority")
        endif()
        if(_name STREQUAL "proc_macos_readonly.c.o")
            foreach(_s IN LISTS _u)
                if(NOT _s IN_LIST _psq_readonly_allowed)
                    list(APPEND _bad "${_label} references ${_s} (not in the read-only allowlist)")
                endif()
            endforeach()
            execute_process(COMMAND "${PSQ_NM}" -g "${_path}"
                OUTPUT_VARIABLE _o RESULT_VARIABLE _rc)
            foreach(_s proc_macos_lookup proc_macos_kill proc_macos_renice)
                if(NOT _o MATCHES "[ \t]T[ \t]+_?${_s}\n")
                    list(APPEND _bad "${_label} does not define ${_s}; is it the native backend?")
                endif()
            endforeach()
        elseif(_name MATCHES "^proc_macos")
            list(APPEND _bad "${_bin} links the real native backend (${_label})")
        elseif(NOT (_member AND _name STREQUAL "proc_script.c.o"))
            foreach(_s IN LISTS _psq_acts)
                if(_s IN_LIST _u)
                    list(APPEND _bad "${_label} references ${_s}")
                endif()
            endforeach()
        endif()
    endforeach()
    if(_nobj EQUAL 0)
        list(APPEND _bad "${_map} lists no objects")
    endif()
endforeach()

if(_bad)
    list(JOIN _bad "\n" _report)
    message(FATAL_ERROR "a host test can reach a native process action:\n${_report}")
endif()

list(LENGTH PSQ_TEST_BINARIES _n)
message(STATUS
    "${_n} test binaries: no setpriority; process actions only in proc_script.c.o; "
    "native backend linked read-only")
