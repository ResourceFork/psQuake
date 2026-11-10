/*
 * psQuake -- the arena map's engine rules (psq_arena.h).
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#include "psq_arena.h"

#include <ctype.h>
#include <string.h>

const char *const psq_arena_maps[] = {
    PSQ_ARENA_MAP, "psqterraces", "psqtowers", PSQ_TEST_ARENA_MAP, NULL
};

/* base (a last path component) is want, ignoring case and a ".bsp". */
static int psq_is_name(const char *base, const char *want)
{
    size_t i, n = strlen(want);

    for (i = 0; i < n; i++) {
        if (base[i] == '\0'
            || tolower((unsigned char)base[i]) != (unsigned char)want[i])
            return 0;
    }
    if (base[n] == '\0')
        return 1;
    return base[n] == '.'
        && tolower((unsigned char)base[n + 1]) == 'b'
        && tolower((unsigned char)base[n + 2]) == 's'
        && tolower((unsigned char)base[n + 3]) == 'p'
        && base[n + 4] == '\0';
}

int psq_arena_is_map(const char *mapname)
{
    const char *base, *slash;
    int i;

    if (mapname == NULL)
        return 0;
    /* The engine finds maps/<name>.bsp on a case-insensitive file system
     * and through any directory spelling ("./psqarena", "PSQARENA"), so
     * judge by the last path component, ignoring case and a ".bsp". */
    base = mapname;
    while ((slash = strchr(base, '/')) != NULL)
        base = slash + 1;
    for (i = 0; psq_arena_maps[i] != NULL; i++)
        if (psq_is_name(base, psq_arena_maps[i]))
            return 1;
    return 0;
}

const char *psq_new_game_map(int arena_present)
{
    return arena_present ? PSQ_ARENA_MAP : PSQ_START_MAP;
}

psq_parms_kind psq_changelevel_parms(const char *mapname)
{
    return psq_arena_is_map(mapname) ? PSQ_PARMS_NEW : PSQ_PARMS_CHANGE;
}

psq_save_rule psq_save_rule_for(const char *mapname, int process_level)
{
    if (psq_arena_is_map(mapname))
        return PSQ_SAVE_REFUSED_ARENA;
    if (process_level)
        return PSQ_SAVE_REFUSED_PROCESSES;
    return PSQ_SAVE_ALLOWED;
}

const char *psq_save_refusal_line(psq_save_rule rule)
{
    switch (rule)
    {
    case PSQ_SAVE_REFUSED_ARENA:     return PSQ_SAVE_ARENA_LINE;
    case PSQ_SAVE_REFUSED_PROCESSES: return PSQ_SAVE_PROCESSES_LINE;
    default:                         return "";
    }
}

const char *psq_save_refusal_box(psq_save_rule rule)
{
    switch (rule)
    {
    case PSQ_SAVE_REFUSED_ARENA:
        return "You can't save in the\npsQuake arena.\n\nPress Esc.\n";
    case PSQ_SAVE_REFUSED_PROCESSES:
        return "You can't save while\nprocess monsters\nare here.\n\nPress Esc.\n";
    default:
        return "";
    }
}
