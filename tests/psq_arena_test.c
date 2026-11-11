/*
 * psQuake -- host tests for the arena map's engine rules (src/engine/psq_arena.c).
 *
 * The requirement: New Game opens the arena when it is present, and the
 * stock start map otherwise. Leaving the arena resets the spawn parms
 * (SetNewParms), so arena pickups do not carry over; every other changelevel
 * keeps them (SetChangeParms). One save rule: refused in the arena whatever
 * it holds, refused elsewhere only while process monsters are there, one
 * message per refusal.
 *
 * Which maps get process monsters (psq_map_allows_processes), a separate
 * question from which maps are psQuake's arenas: a deathmatch map (a name
 * with "dm" then a digit), a psQuake map (a name starting "psq") and any map
 * with "arena" in its name; never any other map, so no campaign map from
 * any pak. A deathmatch map keeps the campaign's parms and save rules: it
 * refuses a save only while process monsters are there.
 *
 * Returns 0 if every check passes, 1 otherwise.
 */

#include "psq_arena.h"

#include <stdio.h>
#include <string.h>

static int checks_run;
static int checks_failed;

#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        checks_run++;                                                     \
        if (!(cond)) {                                                    \
            checks_failed++;                                              \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                   \
            printf(__VA_ARGS__);                                          \
            printf("\n");                                                 \
        }                                                                 \
    } while (0)

int main(void)
{
    /* Every spelling the engine resolves to maps/psqarena.bsp on macOS's
     * case-insensitive file system, or through a directory component. */
    static const char *const arenas[] = {
        "psqtemple", "PSQTEMPLE.BSP", "maps/psqtemple", "psqruins",
        "psqarena", "PSQARENA", "PsqArena", "./psqarena", "maps/psqarena",
        "../id1/maps/psqarena", "psqarena.bsp", "PSQARENA.BSP",
        /* the other arenas: the exit chain's and the test hall */
        "psqterraces", "PSQTowers.bsp", "maps/psqtowers", "psqtesthall", "PSQTESTHALL.BSP",
        NULL
    };
    static const char *const others[] = {
        "start", "lq_e1m1", "e1m1", "psqarena2", "psqaren", "psqarena.bs",
        "psqarena.bspx", "psqarena/", " psqarena", "psqarena ", "xpsqarena",
        "psqarena.map", "", "/", "psqtempl", "psqtemple2", "psqterrace", "psqtowers2", "psqtest", "psqtesthall.map",
        "psq", NULL
    };
    int i;

    for (i = 0; arenas[i] != NULL; i++) {
        CHECK(psq_arena_is_map(arenas[i]), "'%s' is the arena", arenas[i]);
        CHECK(psq_changelevel_parms(arenas[i]) == PSQ_PARMS_NEW,
              "leaving '%s' must reset the parms", arenas[i]);
        CHECK(psq_save_rule_for(arenas[i], 0) == PSQ_SAVE_REFUSED_ARENA
              && psq_save_rule_for(arenas[i], 1) == PSQ_SAVE_REFUSED_ARENA,
              "saving on '%s' is refused as the arena, monsters or not",
              arenas[i]);
    }

    CHECK(strcmp(psq_new_game_map(1), "psqtemple") == 0,
          "New Game with the arena present loads %s", psq_new_game_map(1));
    CHECK(strcmp(psq_new_game_map(0), "start") == 0,
          "New Game without the arena loads %s", psq_new_game_map(0));
    CHECK(strcmp(psq_arena_maps[0], psq_new_game_map(1)) == 0,
          "the first arena listed is New Game's (%s)", psq_arena_maps[0]);
    for (i = 0; psq_arena_maps[i] != NULL; i++)
        CHECK(psq_arena_is_map(psq_arena_maps[i]), "'%s' is listed and an arena",
              psq_arena_maps[i]);
    CHECK(i == 6, "six arenas listed (%d)", i);
    CHECK(strcmp(PSQ_ARENA_BSP, "maps/psqtemple.bsp") == 0,
          "arena path is %s", PSQ_ARENA_BSP);

    CHECK(psq_arena_is_map("psqarena"), "psqarena is the arena");
    CHECK(psq_changelevel_parms("psqarena") == PSQ_PARMS_NEW,
          "leaving the arena must reset the parms");
    CHECK(psq_save_rule_for("psqarena", 0) == PSQ_SAVE_REFUSED_ARENA,
          "saving in the arena must be refused");

    for (i = 0; others[i] != NULL; i++) {
        CHECK(!psq_arena_is_map(others[i]), "'%s' is not the arena", others[i]);
        CHECK(psq_changelevel_parms(others[i]) == PSQ_PARMS_CHANGE,
              "leaving '%s' keeps the parms", others[i]);
        CHECK(psq_save_rule_for(others[i], 0) == PSQ_SAVE_ALLOWED,
              "saving on '%s' with no process monsters is allowed",
              others[i]);
    }
    CHECK(!psq_arena_is_map(NULL), "NULL is not the arena");
    CHECK(psq_changelevel_parms(NULL) == PSQ_PARMS_CHANGE,
          "no map name keeps the parms");
    CHECK(psq_save_rule_for(NULL, 0) == PSQ_SAVE_ALLOWED,
          "no map name does not refuse saves");

    /* Process monsters (psq_testarena on any map) refuse saves there too,
     * with their own single message; leaving such a level allows saving. */
    CHECK(psq_save_rule_for("lq_e1m1", 1) == PSQ_SAVE_REFUSED_PROCESSES,
          "a level holding process monsters refuses saves");
    CHECK(psq_save_rule_for(NULL, 1) == PSQ_SAVE_REFUSED_PROCESSES,
          "process monsters refuse saves whatever the map");
    CHECK(psq_save_rule_for("lq_e1m1", 0) == PSQ_SAVE_ALLOWED,
          "the campaign map after the exit allows saves");
    CHECK(strcmp(psq_save_refusal_line(PSQ_SAVE_REFUSED_ARENA), PSQ_SAVE_ARENA_LINE) == 0
          && strcmp(psq_save_refusal_line(PSQ_SAVE_REFUSED_PROCESSES),
                    PSQ_SAVE_PROCESSES_LINE) == 0
          && psq_save_refusal_line(PSQ_SAVE_ALLOWED)[0] == '\0',
          "one console line per refusal, none when allowed");
    CHECK(strcmp(PSQ_SAVE_ARENA_LINE, PSQ_SAVE_PROCESSES_LINE) != 0
          && strstr(psq_save_refusal_box(PSQ_SAVE_REFUSED_ARENA), "arena") != NULL
          && strstr(psq_save_refusal_box(PSQ_SAVE_REFUSED_PROCESSES), "process") != NULL
          && psq_save_refusal_box(PSQ_SAVE_ALLOWED)[0] == '\0',
          "the Save menu box names the reason");

    {
        /* Allowed: deathmatch maps, psQuake maps, "arena" maps; any case,
         * any directory, with or without ".bsp". */
        static const char *const allowed[] = {
            "dm1", "dm3", "DM6", "maps/dm4.bsp", "lqdm1", "lqdm10", "LQDM13.BSP",
            "e1dm2", "xdm9y", "mydm0",
            "psq", "psqarena", "psqtemple", "psqruins", "psqforge", "PSQTESTHALL.bsp",
            "psqanything", "maps/psqx", "./psq_custom",
            "arena", "myarena", "ARENA1", "the_arena_of_doom", "maps/castlearena.bsp",
            NULL
        };
        /* Excluded: every campaign map (LibreQuake's and id's), and the
         * near misses. */
        static const char *const excluded[] = {
            "start", "e1m1", "e4m8", "end", "dev", "lq_e0m1", "lq_e1m1", "lq_e4m8",
            "lq_end", "lq_start", "hipdm", "dm", "dmx", "dm_", "d1m", "lqdm",
            "xpsq", "ps", "pqs", "aren", "arenx", "a_rena", "dm3/start",
            "maps/arena/e1m1", "psq/e1m1", "", "/", ".bsp", "start.bsp", "dmx.bsp",
            "dm.bsp3", NULL
        };

        for (i = 0; allowed[i] != NULL; i++)
            CHECK(psq_map_allows_processes(allowed[i]),
                  "'%s' gets process monsters", allowed[i]);
        for (i = 0; excluded[i] != NULL; i++)
            CHECK(!psq_map_allows_processes(excluded[i]),
                  "'%s' gets no process monsters", excluded[i]);
        CHECK(!psq_map_allows_processes(NULL), "NULL gets no process monsters");
        for (i = 0; psq_arena_maps[i] != NULL; i++)
            CHECK(psq_map_allows_processes(psq_arena_maps[i]),
                  "arena '%s' gets process monsters", psq_arena_maps[i]);

        /* A deathmatch or "arena" map is no psQuake arena: its parms carry
         * over, and it refuses a save only while process monsters are there,
         * with the process monsters' message. */
        static const char *const dm[] = { "dm3", "lqdm10", "myarena", NULL };
        for (i = 0; dm[i] != NULL; i++) {
            CHECK(!psq_arena_is_map(dm[i]), "'%s' is not a psQuake arena", dm[i]);
            CHECK(psq_changelevel_parms(dm[i]) == PSQ_PARMS_CHANGE,
                  "leaving '%s' keeps the parms", dm[i]);
            CHECK(psq_save_rule_for(dm[i], 0) == PSQ_SAVE_ALLOWED,
                  "'%s' with no process monsters allows saves", dm[i]);
            CHECK(psq_save_rule_for(dm[i], 1) == PSQ_SAVE_REFUSED_PROCESSES,
                  "'%s' with process monsters refuses saves for them", dm[i]);
        }
    }

    printf("psq_arena_test: %d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
