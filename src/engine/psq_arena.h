/*
 * psQuake -- the arena map's engine rules (engine-free, host-tested).
 *
 * New Game opens the Temple (maps/psqtemple.bsp, tools/generate_map.py)
 * instead of the stock start map, when it is present. Its exit leads to
 * Ruins (psqruins), whose exit leads to the Pit (psqarena) until Forge
 * exists; the Pit's leads on to
 * psqterraces, then psqtowers, and theirs to the campaign. The Pit,
 * Terraces and Towers (tools/generate_arena.py) are banked: still shipped
 * and loadable with `map`. psqtesthall, the test arena, is loaded only by
 * name. The rules below hold in every one of them. Leaving
 * an arena resets the player's spawn parms, as a new game does, so arena
 * pickups never reach the next arena or the campaign. Saving is refused
 * in the arena and on any level holding process monsters (psq_save_rule_for):
 * they carry process ids that a saved game would restore after the
 * processes are gone.
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#ifndef PSQ_ARENA_H
#define PSQ_ARENA_H

#define PSQ_ARENA_MAP  "psqtemple"   /* New Game's: Temple */
#define PSQ_ARENA_BSP  "maps/" PSQ_ARENA_MAP ".bsp"
#define PSQ_START_MAP  "start"
#define PSQ_TEST_ARENA_MAP "psqtesthall"   /* the tests' one-grid hall */

/* The arenas' map names, NULL-terminated: New Game's first. */
extern const char *const psq_arena_maps[];

/* 1 if mapname (the server's sv.name) names an arena, else 0: its last
 * path component is one of psq_arena_maps, in any case, with or without
 * ".bsp". NULL is 0. */
int psq_arena_is_map(const char *mapname);

/* 1 if process monsters may come to mapname (sv.name), judged by its last
 * path component, in any case, without a ".bsp":
 *   - a deathmatch map: "dm" followed by a digit anywhere (dm3, lqdm10);
 *   - a psQuake map: it starts with "psq" (the arenas, the test hall);
 *   - a map with "arena" anywhere in its name.
 * Every other map, so every campaign map from any pak, gets none. NULL is 0.
 * This is not psq_arena_is_map: only psQuake's own arenas get the arena
 * rules (the parms reset, the save refusal whatever the level holds, the
 * start room, resupply). On any other allowed map the controller places
 * process monsters off the grid (QuakeC psq_findspot), and saves are
 * refused only while it or a process monster is there. */
int psq_map_allows_processes(const char *mapname);

/* The map New Game loads: the arena when arena_present, else the stock
 * start map. */
const char *psq_new_game_map(int arena_present);

/* Which QuakeC function SV_SaveSpawnparms runs for each client before a
 * changelevel from mapname: the new-game parms when leaving the arena, the
 * usual carried-over parms otherwise. */
typedef enum
{
    PSQ_PARMS_CHANGE,  /* SetChangeParms: keep health, weapons, ammo */
    PSQ_PARMS_NEW      /* SetNewParms: start fresh */
} psq_parms_kind;

psq_parms_kind psq_changelevel_parms(const char *mapname);

/* psQuake's one save rule, for `save` (and quick save) and the Save menu
 * alike. Process monsters stand for processes that are running now, so a
 * level holding them is never saved:
 *   - the arena (mapname) is always refused, whatever it holds;
 *   - any other level is refused while process_level is set: the process
 *     controller has run there or an entity carries a process tag (the
 *     engine's psq_* state, src/engine/psq_builtins.c);
 *   - anything else is allowed.
 * Exactly one refusal applies, so the player sees one message. A game that is
 * loaded never brings a process tag back (PSQ_AfterLoad clears them). */
typedef enum
{
    PSQ_SAVE_ALLOWED,
    PSQ_SAVE_REFUSED_ARENA,
    PSQ_SAVE_REFUSED_PROCESSES
} psq_save_rule;

psq_save_rule psq_save_rule_for(const char *mapname, int process_level);

/* The console line for a refusal ("" for PSQ_SAVE_ALLOWED), and the Save
 * menu's message box text. */
const char *psq_save_refusal_line(psq_save_rule rule);
const char *psq_save_refusal_box(psq_save_rule rule);

#define PSQ_SAVE_ARENA_LINE "Can't save in the psQuake arena.\n"
#define PSQ_SAVE_PROCESSES_LINE                                          \
    "Can't save here: process monsters exist only while their processes run.\n"

#endif /* PSQ_ARENA_H */
