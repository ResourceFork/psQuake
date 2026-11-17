# psQuake -- static check: the player never sees the protocol's words.
#
# The setting reads "Fighting monsters effects": No effect / Wounds only /
# Wounds and kills, and its help box uses the plugin's plain words. "Summoner",
# "dry run", "post", "delete" and "renice" stay in the protocol, the CLI, the
# spec and the logs. This scans the string literals of psq_menu.c, which draws
# the page, its help, the warning and its prompt (any case); the console's
# lines (psq_builtins.c's Con_Printf) and the HUD's "No process monsters: ..."
# status lines (psproc/src/proc_script.c), which the player sees too; and every
# engine and core source for the old setting names. A tripwire, not a proof.
#
# Run by ctest as:  cmake -DPSD_SOURCE_DIR=<repo> -P no_effect_jargon.cmake

if(NOT PSD_SOURCE_DIR)
    message(FATAL_ERROR "PSD_SOURCE_DIR is not set")
endif()

set(_psd_shown
    "${PSD_SOURCE_DIR}/src/engine/psq_menu.c")
file(GLOB_RECURSE _psd_all
    "${PSD_SOURCE_DIR}/src/engine/*.c"
    "${PSD_SOURCE_DIR}/src/core/*.c")

set(_psd_bad "")
# Any string literal in the drawn files naming a protocol word, in any case.
set(_psd_word
    "\"[^\"]*([Ss][Uu][Mm][Mm][Oo][Nn]|[Dd][Rr][Yy][ -]?[Rr][Uu][Nn]|[Pp][Oo][Ss][Tt]|[Dd][Ee][Ll][Ee][Tt][Ee]|[Rr][Ee][Nn][Ii][Cc][Ee]|SIGTERM)[^\"]*\"")
foreach(_f IN LISTS _psd_shown)
    if(NOT EXISTS "${_f}")
        message(FATAL_ERROR "missing ${_f}")
    endif()
    file(STRINGS "${_f}" _hits REGEX "${_psd_word}")
    foreach(_h IN LISTS _hits)
        list(APPEND _psd_bad "${_f}: ${_h}")
    endforeach()
endforeach()
# The console's lines (the string literal on each Con_Printf line, and the
# continuation lines that follow it) and the HUD's status lines.
file(STRINGS "${PSD_SOURCE_DIR}/src/engine/psq_builtins.c" _psq_lines)
set(_psq_in_print 0)
foreach(_l IN LISTS _psq_lines)
    if(_l MATCHES "Con_Printf\\(")
        set(_psq_in_print 1)
    endif()
    if(_psq_in_print AND _l MATCHES "\"[^\"]*([Ss]ummon|[Dd]ry run|[Pp]ost|[Dd]elete)[^\"]*\"")
        list(APPEND _psd_bad "psq_builtins.c console: ${_l}")
    endif()
    if(_l MATCHES ";[ \t]*$")
        set(_psq_in_print 0)
    endif()
endforeach()
file(STRINGS "${PSD_SOURCE_DIR}/psproc/src/proc_script.c" _hits
     REGEX "\"No process monsters[^\"]*([Ss]ummon|[Dd]ry run|[Pp]ost|[Dd]elete)")
foreach(_h IN LISTS _hits)
    list(APPEND _psd_bad "proc_script.c status: ${_h}")
endforeach()
# The old setting names, anywhere in the game.
foreach(_f IN LISTS _psd_all)
    file(STRINGS "${_f}" _hits
         REGEX "\"[^\"]*(dry run\\)|post only|post \\+ delete|\\(KILL\\)|\"Kill mode\")[^\"]*\"")
    foreach(_h IN LISTS _hits)
        list(APPEND _psd_bad "${_f}: ${_h}")
    endforeach()
endforeach()

if(_psd_bad)
    list(JOIN _psd_bad "\n" _psd_report)
    message(FATAL_ERROR "protocol words in text the player sees:\n${_psd_report}")
endif()

list(LENGTH _psd_all _psd_n)
message(STATUS "no protocol words in the menu text (${_psd_n} game files scanned)")
