# psQuake -- static check: the arena rules (src/engine/psq_arena.c) are called
# where the engine decides them. A dedicated server has no player, so the
# headless load test cannot exercise the parms reset or New Game; this check
# fails if a call site is removed or moved out of its function.
#
#   cmake -DPSQ_QUAKE_DIR=<third_party/quakespasm/Quake> -P arena_hooks_wired.cmake
#
# Each row: file, the function the call must sit in, and the call's text.

cmake_minimum_required(VERSION 3.16)

if(NOT PSQ_QUAKE_DIR)
    message(FATAL_ERROR "PSQ_QUAKE_DIR is not set")
endif()

set(_sites
    "menu.c|void M_SinglePlayer_Key (int key)|psq_new_game_map(COM_FileExists(PSQ_ARENA_BSP, NULL))"
    "menu.c|void M_Menu_Save_f (void)|if (psq_save_refused(sv.name))"
    "sv_main.c|void SV_SaveSpawnparms (void)|if (psq_changelevel_parms(sv.name) == PSQ_PARMS_NEW)"
    "sv_main.c|void SV_SaveSpawnparms (void)|PR_ExecuteProgram (pr_global_struct->SetNewParms)"
    "host_cmd.c|static void Host_Savegame_f (void)|if (psq_save_refused(sv.name))")

set(_failed 0)
foreach(_row IN LISTS _sites)
    string(REPLACE "|" ";" _parts "${_row}")
    list(GET _parts 0 _file)
    list(GET _parts 1 _func)
    list(GET _parts 2 _call)
    file(READ "${PSQ_QUAKE_DIR}/${_file}" _text)
    string(FIND "${_text}" "\n${_func}\n{" _start)
    if(_start LESS 0)
        message(SEND_ERROR "${_file}: no function '${_func}'")
        set(_failed 1)
        continue()
    endif()
    string(SUBSTRING "${_text}" ${_start} -1 _body)
    # The function ends at the first closing brace in column 0.
    string(FIND "${_body}" "\n}" _end)
    string(SUBSTRING "${_body}" 0 ${_end} _body)
    string(FIND "${_body}" "${_call}" _at)
    if(_at LESS 0)
        message(SEND_ERROR "${_file}: '${_call}' is not in ${_func}")
        set(_failed 1)
    else()
        message(STATUS "ok ${_file} ${_func}: ${_call}")
    endif()
endforeach()
if(_failed)
    message(FATAL_ERROR "arena hooks are not all wired")
endif()
