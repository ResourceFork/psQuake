/*
 * psQuake -- host tests for the arena map's engine rules (src/engine/psq_arena.c).
 *
 * The requirement: New Game opens the arena when it is present, and the
 * stock start map otherwise. Leaving the arena resets the spawn parms
 * (SetNewParms), so arena pickups do not carry over; every other changelevel
 * keeps them (SetChangeParms). Saving is refused only in the arena.
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
        "psqarena", "PSQARENA", "PsqArena", "./psqarena", "maps/psqarena",
        "../id1/maps/psqarena", "psqarena.bsp", "PSQARENA.BSP", NULL
    };
    static const char *const others[] = {
        "start", "lq_e1m1", "e1m1", "psqarena2", "psqaren", "psqarena.bs",
        "psqarena.bspx", "psqarena/", " psqarena", "psqarena ", "xpsqarena",
        "psqarena.map", "", "/", NULL
    };
    int i;

    for (i = 0; arenas[i] != NULL; i++) {
        CHECK(psq_arena_is_map(arenas[i]), "'%s' is the arena", arenas[i]);
        CHECK(psq_changelevel_parms(arenas[i]) == PSQ_PARMS_NEW,
              "leaving '%s' must reset the parms", arenas[i]);
        CHECK(psq_save_refused(arenas[i]), "saving on '%s' must be refused",
              arenas[i]);
    }

    CHECK(strcmp(psq_new_game_map(1), "psqarena") == 0,
          "New Game with the arena present loads %s", psq_new_game_map(1));
    CHECK(strcmp(psq_new_game_map(0), "start") == 0,
          "New Game without the arena loads %s", psq_new_game_map(0));
    CHECK(strcmp(PSQ_ARENA_BSP, "maps/psqarena.bsp") == 0,
          "arena path is %s", PSQ_ARENA_BSP);

    CHECK(psq_arena_is_map("psqarena"), "psqarena is the arena");
    CHECK(psq_changelevel_parms("psqarena") == PSQ_PARMS_NEW,
          "leaving the arena must reset the parms");
    CHECK(psq_save_refused("psqarena"), "saving in the arena must be refused");

    for (i = 0; others[i] != NULL; i++) {
        CHECK(!psq_arena_is_map(others[i]), "'%s' is not the arena", others[i]);
        CHECK(psq_changelevel_parms(others[i]) == PSQ_PARMS_CHANGE,
              "leaving '%s' keeps the parms", others[i]);
        CHECK(!psq_save_refused(others[i]), "saving on '%s' is allowed",
              others[i]);
    }
    CHECK(!psq_arena_is_map(NULL), "NULL is not the arena");
    CHECK(psq_changelevel_parms(NULL) == PSQ_PARMS_CHANGE,
          "no map name keeps the parms");
    CHECK(!psq_save_refused(NULL), "no map name does not refuse saves");

    printf("psq_arena_test: %d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
