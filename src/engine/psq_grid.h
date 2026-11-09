/*
 * psQuake -- the arena grid: which floor cell a process monster teleports
 * into (engine-free, host-tested).
 *
 * The arena map's psq_arena entity describes the grid (tools/generate_arena.py,
 * docs/architecture.md): `origin` is the grid centre on the floor, `psq_cols`
 * and `psq_rows` count the cells, `psq_cell` is their size. Cell i is column
 * i % cols, row i / cols, centred at
 *
 *     origin + ((col - (cols - 1) / 2) * cell, (row - (rows - 1) / 2) * cell, 0)
 *
 * One monster per cell: a cell is free when no solid entity's box reaches
 * into its square (within the grid's height), so the largest (hull 2,
 * 64 x 64) monsters always get a whole cell. A player blocks every cell its
 * box comes within PSQ_GRID_PLAYER_PAD of, so nothing arrives on top of or
 * against the player. Walls are not known here: QuakeC still tests each
 * chosen spot (psq_trypos) and asks for the next free cell when it fails.
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#ifndef PSQ_GRID_H
#define PSQ_GRID_H

#define PSQ_GRID_MAX_DIM     32      /* cells along either axis           */
#define PSQ_GRID_MIN_CELL    64.0f   /* hull 2 is 64 x 64                 */
#define PSQ_GRID_MAX_CELL    1024.0f
#define PSQ_GRID_HEIGHT      128.0f  /* the slab above the floor checked  */
#define PSQ_GRID_PLAYER_PAD  32.0f   /* clearance kept around a player    */

typedef struct
{
    float origin[3];
    int   cols;
    int   rows;
    float cell;
} psq_grid;

/* An entity's world-space box, grown by pad on every side in x and y. */
typedef struct
{
    float mins[3];
    float maxs[3];
    float pad;
} psq_grid_box;

/* Fill *g from the entity's keys. 1 when they describe a usable grid:
 * finite origin, cols and rows whole numbers in 1..PSQ_GRID_MAX_DIM, cell in
 * PSQ_GRID_MIN_CELL..PSQ_GRID_MAX_CELL. Else 0, and *g is zeroed. */
int psq_grid_from_keys(psq_grid *g, const float origin[3],
                       float cols, float rows, float cell);

/* Number of cells (0 for a zeroed grid). */
int psq_grid_cells(const psq_grid *g);

/* Floor centre of cell idx into out. 0 (out untouched) if idx is out of
 * range. */
int psq_grid_centre(const psq_grid *g, int idx, float out[3]);

/* 1 if box b reaches into cell idx: its padded x and y ranges overlap the
 * cell's open square, and its z range overlaps the slab from the floor to
 * PSQ_GRID_HEIGHT above it. Touching edges do not block. */
int psq_grid_blocked(const psq_grid *g, int idx, const psq_grid_box *b);

/* The skip-th (0-based) free cell met scanning start, start + 1, ...,
 * wrapping, over every cell once, with boxes[0..n) as the occupants. -1 when
 * there are not that many free cells, the grid is empty, or start or skip is
 * negative. start is taken modulo the cell count. */
int psq_grid_pick(const psq_grid *g, const psq_grid_box *boxes, int n,
                  int start, int skip);

#endif /* PSQ_GRID_H */
