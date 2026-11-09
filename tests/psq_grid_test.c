/*
 * psQuake -- host tests for the arena grid (src/engine/psq_grid.c).
 *
 * The requirement: a process monster teleports in onto a free cell of the
 * arena's grid (origin, psq_cols 11, psq_rows 7, psq_cell 96). One monster
 * per cell, so the hull 2 classes (64 x 64) always get a whole cell; never a
 * cell any solid entity reaches into, and never on or against the player.
 * When every cell is taken, or the keys are not a usable grid, there is no
 * cell (QuakeC then falls back to placing near the player).
 *
 * Returns 0 if every check passes, 1 otherwise.
 */

#include "psq_grid.h"

#include <math.h>
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

/* The box a monster with these mins/maxs has standing in cell idx. */
static psq_grid_box monster_in(const psq_grid *g, int idx, float half, float top)
{
    psq_grid_box b;
    float c[3];

    psq_grid_centre(g, idx, c);
    b.mins[0] = c[0] - half; b.maxs[0] = c[0] + half;
    b.mins[1] = c[1] - half; b.maxs[1] = c[1] + half;
    b.mins[2] = c[2];        b.maxs[2] = c[2] + 24 + top;
    b.pad = 0;
    return b;
}

static psq_grid_box box_at(float x, float y, float z, float half, float pad)
{
    psq_grid_box b;

    b.mins[0] = x - half; b.maxs[0] = x + half;
    b.mins[1] = y - half; b.maxs[1] = y + half;
    b.mins[2] = z - 24;   b.maxs[2] = z + 32;
    b.pad = pad;
    return b;
}

int main(void)
{
    static const float zero[3] = {0, 0, 0};
    psq_grid g, bad;
    psq_grid_box boxes[128];
    float c[3];
    int i, n;

    /* --- the keys */
    CHECK(psq_grid_from_keys(&g, zero, 11, 7, 96), "the arena's keys are a grid");
    CHECK(psq_grid_cells(&g) == 77, "77 cells, got %d", psq_grid_cells(&g));
    {
        const float nan_origin[3] = {NAN, 0, 0};
        struct { float cols, rows, cell; const char *why; } bads[] = {
            {0, 7, 96, "no columns"}, {11, 0, 96, "no rows"},
            {-1, 7, 96, "negative columns"}, {11.5f, 7, 96, "fractional columns"},
            {11, 7.25f, 96, "fractional rows"}, {33, 7, 96, "too many columns"},
            {11, 33, 96, "too many rows"}, {11, 7, 63, "cell smaller than hull 2"},
            {11, 7, 0, "no cell size"}, {11, 7, 2000, "cell too large"},
            {NAN, 7, 96, "NaN columns"}, {11, 7, INFINITY, "infinite cell"},
        };
        for (i = 0; i < (int) (sizeof(bads) / sizeof(bads[0])); i++) {
            CHECK(!psq_grid_from_keys(&bad, zero, bads[i].cols, bads[i].rows, bads[i].cell),
                  "rejected: %s", bads[i].why);
            CHECK(psq_grid_cells(&bad) == 0 && psq_grid_pick(&bad, NULL, 0, 0, 0) == -1,
                  "a rejected grid has no cells: %s", bads[i].why);
        }
        CHECK(!psq_grid_from_keys(&bad, nan_origin, 11, 7, 96), "rejected: NaN origin");
        CHECK(!psq_grid_from_keys(&bad, NULL, 11, 7, 96), "rejected: no origin");
        CHECK(psq_grid_from_keys(&bad, zero, 1, 1, 64), "smallest grid accepted");
        CHECK(psq_grid_from_keys(&bad, zero, 32, 32, 1024), "largest grid accepted");
    }

    /* --- cell centres: (c, r) at origin + ((c - 5) * 96, (r - 3) * 96, 0) */
    CHECK(psq_grid_centre(&g, 0, c) && c[0] == -480 && c[1] == -288 && c[2] == 0,
          "cell 0 at (-480, -288, 0), got (%g, %g, %g)", c[0], c[1], c[2]);
    CHECK(psq_grid_centre(&g, 38, c) && c[0] == 0 && c[1] == 0,
          "cell 38 (5, 3) is the centre, got (%g, %g)", c[0], c[1]);
    CHECK(psq_grid_centre(&g, 76, c) && c[0] == 480 && c[1] == 288,
          "cell 76 at (480, 288), got (%g, %g)", c[0], c[1]);
    CHECK(!psq_grid_centre(&g, 77, c) && !psq_grid_centre(&g, -1, c),
          "no centre outside the grid");
    {
        const float o[3] = {100, -50, 32};
        psq_grid moved;
        psq_grid_from_keys(&moved, o, 4, 3, 128);
        CHECK(psq_grid_centre(&moved, 0, c) && c[0] == 100 - 192 && c[1] == -50 - 128
              && c[2] == 32, "even grid: cell 0 at (%g, %g, %g)", c[0], c[1], c[2]);
    }
    /* Every cell lies inside the 1056 x 672 grid floor, a whole cell apart. */
    for (i = 0; i < 77; i++) {
        psq_grid_centre(&g, i, c);
        CHECK(fabsf(c[0]) + 48 <= 528 && fabsf(c[1]) + 48 <= 336,
              "cell %d inside the grid floor", i);
    }

    /* --- free cells */
    CHECK(psq_grid_pick(&g, NULL, 0, 0, 0) == 0, "empty grid: cell 0 first");
    CHECK(psq_grid_pick(&g, NULL, 0, 40, 0) == 40, "scan starts at start");
    CHECK(psq_grid_pick(&g, NULL, 0, 77 + 5, 0) == 5, "start wraps");
    CHECK(psq_grid_pick(&g, NULL, 0, 76, 1) == 0, "the scan wraps past the last cell");
    CHECK(psq_grid_pick(&g, NULL, 0, 0, 76) == 76, "skip counts free cells");
    CHECK(psq_grid_pick(&g, NULL, 0, 0, 77) == -1, "no 78th cell");
    CHECK(psq_grid_pick(&g, NULL, 0, -1, 0) == -1 && psq_grid_pick(&g, NULL, 0, 0, -1) == -1,
          "negative start or skip has no cell");

    /* A shambler (hull 2: 64 x 64, 88 tall) in cell 10 takes cell 10 only. */
    boxes[0] = monster_in(&g, 10, 32, 64);
    CHECK(psq_grid_pick(&g, boxes, 1, 10, 0) == 11, "a hull 2 monster takes its whole cell");
    CHECK(psq_grid_pick(&g, boxes, 1, 9, 0) == 9, "and does not reach its neighbours");
    for (i = 0; i < 77; i++)
        CHECK(psq_grid_blocked(&g, i, &boxes[0]) == (i == 10),
              "shambler in cell 10 blocks cell %d: %d", i,
              psq_grid_blocked(&g, i, &boxes[0]));

    /* A monster placed near the player straddling cells 0 and 1 blocks both. */
    boxes[0] = box_at(-432, -288, 24, 16, 0);
    CHECK(psq_grid_blocked(&g, 0, &boxes[0]) && psq_grid_blocked(&g, 1, &boxes[0])
          && !psq_grid_blocked(&g, 2, &boxes[0]) && !psq_grid_blocked(&g, 11, &boxes[0]),
          "a box on a cell edge blocks both cells");
    CHECK(psq_grid_pick(&g, boxes, 1, 0, 0) == 2, "so the first free cell is 2");

    /* Edges that only touch do not block: a 96-wide box filling cell 1. */
    boxes[0] = box_at(-384, -288, 24, 48, 0);
    CHECK(!psq_grid_blocked(&g, 0, &boxes[0]) && psq_grid_blocked(&g, 1, &boxes[0])
          && !psq_grid_blocked(&g, 2, &boxes[0]), "touching edges do not block");

    /* The player: its 32 x 32 box plus the pad blocks its cell and the
     * cells it stands within PSQ_GRID_PLAYER_PAD of. */
    boxes[0] = box_at(0, 0, 24, 16, PSQ_GRID_PLAYER_PAD);   /* centre of cell 38 */
    CHECK(psq_grid_blocked(&g, 38, &boxes[0]), "the player's own cell is blocked");
    CHECK(!psq_grid_blocked(&g, 37, &boxes[0]) && !psq_grid_blocked(&g, 39, &boxes[0]),
          "a player mid-cell leaves the neighbours free (16 + 32 = 48)");
    CHECK(psq_grid_pick(&g, boxes, 1, 38, 0) == 39, "never the player's cell");
    boxes[0] = box_at(40, 0, 24, 16, PSQ_GRID_PLAYER_PAD);  /* near cell 39's edge */
    CHECK(psq_grid_blocked(&g, 38, &boxes[0]) && psq_grid_blocked(&g, 39, &boxes[0]),
          "a player near an edge blocks the cell beyond it too");
    CHECK(psq_grid_pick(&g, boxes, 1, 38, 0) == 40, "so the pick moves past both");
    CHECK(!psq_grid_blocked(&g, 40, &boxes[0]), "the pad does not reach cell 40");

    /* Height: something well above the floor slab, or below it, does not
     * block; something standing on the floor does. */
    boxes[0] = box_at(0, 0, 400, 16, 0);
    CHECK(!psq_grid_blocked(&g, 38, &boxes[0]), "a box high above does not block");
    boxes[0] = box_at(0, 0, -200, 16, 0);
    CHECK(!psq_grid_blocked(&g, 38, &boxes[0]), "a box far below does not block");
    boxes[0] = box_at(0, 0, 100, 16, 0);   /* a Scrag hovering low */
    CHECK(psq_grid_blocked(&g, 38, &boxes[0]), "a low flier blocks its cell");

    /* Full grid: 77 monsters, one per cell -> no cell; one leaves -> its cell. */
    for (i = 0; i < 77; i++)
        boxes[i] = monster_in(&g, i, 32, 64);
    CHECK(psq_grid_pick(&g, boxes, 77, 0, 0) == -1, "a full grid has no cell");
    CHECK(psq_grid_pick(&g, boxes, 77, 50, 0) == -1, "from any start");
    n = 0;
    for (i = 0; i < 77; i++)
        if (i != 23)
            boxes[n++] = monster_in(&g, i, 32, 64);
    CHECK(psq_grid_pick(&g, boxes, n, 60, 0) == 23, "the one free cell is found");
    CHECK(psq_grid_pick(&g, boxes, n, 60, 1) == -1, "and only that one");

    /* Filling the grid one pick at a time gives 77 distinct cells. */
    {
        int seen[77];
        int k;
        memset(seen, 0, sizeof(seen));
        n = 0;
        for (k = 0; k < 80; k++) {
            int idx = psq_grid_pick(&g, boxes, n, (k * 37) % 77, 0);
            if (k >= 77) {
                CHECK(idx == -1, "pick %d after a full fill has no cell", k);
                continue;
            }
            CHECK(idx >= 0 && idx < 77 && !seen[idx], "pick %d gives a new cell (%d)", k, idx);
            if (idx < 0 || idx >= 77)
                break;
            seen[idx] = 1;
            boxes[n++] = monster_in(&g, idx, 32, 64);
        }
    }

    printf("psq_grid_test: %d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
