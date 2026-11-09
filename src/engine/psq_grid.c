/*
 * psQuake -- the arena grid (psq_grid.h).
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#include "psq_grid.h"

#include <math.h>
#include <string.h>

static int psq_whole_in(float v, int lo, int hi)
{
    return isfinite(v) && v == floorf(v) && v >= (float) lo && v <= (float) hi;
}

int psq_grid_from_keys(psq_grid *g, const float origin[3],
                       float cols, float rows, float cell)
{
    memset(g, 0, sizeof(*g));
    if (origin == NULL
        || !isfinite(origin[0]) || !isfinite(origin[1]) || !isfinite(origin[2]))
        return 0;
    if (!psq_whole_in(cols, 1, PSQ_GRID_MAX_DIM)
        || !psq_whole_in(rows, 1, PSQ_GRID_MAX_DIM))
        return 0;
    if (!isfinite(cell) || cell < PSQ_GRID_MIN_CELL || cell > PSQ_GRID_MAX_CELL)
        return 0;
    g->origin[0] = origin[0];
    g->origin[1] = origin[1];
    g->origin[2] = origin[2];
    g->cols = (int) cols;
    g->rows = (int) rows;
    g->cell = cell;
    return 1;
}

int psq_grid_cells(const psq_grid *g)
{
    return g->cols * g->rows;
}

int psq_grid_centre(const psq_grid *g, int idx, float out[3])
{
    int col, row;

    if (idx < 0 || idx >= psq_grid_cells(g))
        return 0;
    col = idx % g->cols;
    row = idx / g->cols;
    out[0] = g->origin[0] + ((float) col - (float) (g->cols - 1) / 2.0f) * g->cell;
    out[1] = g->origin[1] + ((float) row - (float) (g->rows - 1) / 2.0f) * g->cell;
    out[2] = g->origin[2];
    return 1;
}

/* Open intervals (a0, a1) and (b0, b1) overlap. */
static int psq_overlap(float a0, float a1, float b0, float b1)
{
    return a0 < b1 && b0 < a1;
}

int psq_grid_blocked(const psq_grid *g, int idx, const psq_grid_box *b)
{
    float c[3];
    float h = g->cell / 2.0f;
    float pad = b->pad > 0 ? b->pad : 0;

    if (!psq_grid_centre(g, idx, c))
        return 0;
    return psq_overlap(b->mins[0] - pad, b->maxs[0] + pad, c[0] - h, c[0] + h)
        && psq_overlap(b->mins[1] - pad, b->maxs[1] + pad, c[1] - h, c[1] + h)
        && psq_overlap(b->mins[2], b->maxs[2], c[2], c[2] + PSQ_GRID_HEIGHT);
}

int psq_grid_pick(const psq_grid *g, const psq_grid_box *boxes, int n,
                  int start, int skip)
{
    int cells = psq_grid_cells(g);
    int k, j;

    if (cells <= 0 || start < 0 || skip < 0)
        return -1;
    start %= cells;
    for (k = 0; k < cells; k++) {
        int idx = (start + k) % cells;
        int blocked = 0;

        for (j = 0; j < n && !blocked; j++)
            blocked = psq_grid_blocked(g, idx, &boxes[j]);
        if (blocked)
            continue;
        if (skip == 0)
            return idx;
        skip--;
    }
    return -1;
}
