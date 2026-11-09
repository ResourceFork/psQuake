/*
 * psQuake -- process labels: the engine-free maths behind
 * SCR_DrawProcessLabels (psq_labels_draw.c).
 *
 * Every function here is pure: no engine state, no GL. The engine glue
 * fills a psq_label_view_t from the frame it just rendered, and asks this
 * unit where a label goes, how opaque it is and what it says. The host test
 * tests/psq_labels_test.c covers it.
 *
 * Coordinates: world points are Quake units; screen points are framebuffer
 * pixels with y down, the space GL_SetCanvas (CANVAS_DEFAULT) draws in.
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#ifndef PSQ_LABELS_H
#define PSQ_LABELS_H

#include <stddef.h>

/* What a label says. Names are cut to this many characters (psDoom's
 * 16-byte psd_name); the PID row is the decimal pid. */
#define PSQ_LABEL_NAME_CHARS 15
#define PSQ_LABEL_TEXT_MAX   24

/* The console font: 8x8 glyphs, scaled by a whole factor. */
#define PSQ_LABEL_GLYPH      8

/* The anchor sits this far above the top of the model's box, and nothing
 * closer than PSQ_LABEL_NEAR_Z in front of the eye is projected. */
#define PSQ_LABEL_ANCHOR_UP  8.0f
#define PSQ_LABEL_NEAR_Z     1.0f

/* Below this opacity a label is not drawn. */
#define PSQ_LABEL_MIN_ALPHA  (1.0f / 32.0f)

/* The largest pid QuakeC can carry exactly (a float's 24-bit mantissa). */
#define PSQ_LABEL_PID_MAX    16777216

/* The camera of the frame just rendered. */
typedef struct
{
    float origin[3];
    float forward[3];   /* unit vectors, as the renderer used them */
    float right[3];
    float up[3];
    float fov_x;        /* full horizontal and vertical fov, degrees */
    float fov_y;
    int   x, y;         /* the 3D view rectangle, pixels, y down     */
    int   width, height;
} psq_label_view_t;

/* Project a world point. Returns 1 and the screen point (*sx, *sy) and its
 * depth along the view axis when it is at least PSQ_LABEL_NEAR_Z in front of
 * the eye; 0 when it is behind or beside the eye, the view is degenerate
 * (empty rectangle, fov outside (0, 180)), or any input is not finite. The
 * point may still be outside the view rectangle. */
int psq_label_project(const psq_label_view_t *v, const float point[3],
                      float *sx, float *sy, float *depth);

/* 1 when a projected anchor's screen x lies inside the view rectangle
 * (v->x <= sx < v->x + v->width), else 0 (also for a NaN or no view). A
 * label is drawn only then: pinned to the view's edge, the label of a
 * monster beside the view would sit over some other monster. */
int psq_label_anchor_in_view(const psq_label_view_t *v, float sx);

/* The fade for a psq_labelrange value (PSD_LABELS_*): fully opaque up to
 * *full units from the eye, gone from *gone on. PSD_LABELS_ALL never fades
 * (both are 0). An unknown value fades as PSD_LABELS_NORMAL. */
void psq_label_range_bounds(int range, float *full, float *gone);

/* Opacity 0..1 at `distance` units for a psq_labelrange value: 1 up to the
 * full distance, falling linearly to 0 at the gone distance. 0 for a
 * negative or non-finite distance. */
float psq_label_alpha(int range, float distance);

/* A QuakeC psq_pid value as a process pid: the integer pid when it is an
 * exact integer in 2..PSQ_LABEL_PID_MAX, else 0 (ordinary entities, the
 * swarm tag -1, corpses -3 and anything malformed). */
int psq_label_pid(float value);

/* Copy an untrusted name for drawing: at most in_max bytes of `in` are read
 * (it need not be terminated), stopping at a NUL. Printable ASCII is kept;
 * runs of spaces and tabs become one space and are trimmed at both ends;
 * any other control byte, DEL and each UTF-8 sequence (or stray high byte)
 * becomes one '?'. The result is cut to PSQ_LABEL_NAME_CHARS characters and
 * to out_size - 1, and always terminated. Returns its length. The label
 * pass's names come from the process policy, which has already turned each
 * byte outside printable ASCII into '?' (so a UTF-8 name shows one '?' per
 * byte); this is the drawing side's own guarantee, whatever it is given. */
size_t psq_label_sanitize(char *out, size_t out_size, const char *in,
                          size_t in_max);

/* Whole glyph scale: the status bar's scale (hud_scale, as QuakeSpasm
 * draws it: scr_sbarscale held to 1..width/320), rounded down, so labels
 * match the HUD the player chose. At least 1, and at most one step per 120
 * view pixels so a label never fills a small view. A non-finite hud_scale
 * counts as 1. */
int psq_label_text_scale(int view_height, float hud_scale);

/* Where a label's rows go. Row 0 is the PID when shown (psDoom's order),
 * then the name. */
typedef struct
{
    int  rows;
    char text[2][PSQ_LABEL_TEXT_MAX];
    int  x[2], y[2];     /* top-left of each row, pixels         */
    int  width[2];       /* each row's width, pixels             */
    int  top, bottom;    /* the whole block, pixels (bottom excl) */
    int  scale;
} psq_label_layout_t;

/* Lay a label out above the anchor's screen point (sx, sy): the block's
 * bottom one scale step above it, each row centred on sx, and the block,
 * with the one-step drop shadow each row casts right and down, moved
 * inside the view rectangle (pinned to the nearest edge). pid_text
 * and name may be NULL or empty; each must fit PSQ_LABEL_TEXT_MAX. Returns
 * 0 (and out->rows 0) when there is nothing to show, the anchor is not
 * finite, or a row is wider (or the block taller) than the view. */
int psq_label_layout(const psq_label_view_t *v, float sx, float sy, int scale,
                     const char *pid_text, const char *name,
                     psq_label_layout_t *out);

/* Keep labels apart. blocks[0..n) are laid-out labels, nearest monster
 * first. Each in turn keeps its place unless it overlaps (within one scale
 * step) a label already kept; then it moves straight up, just above that
 * one, until it is clear. A label that would leave the top of the view, or
 * rise more than PSQ_LABEL_MAX_LIFT block heights, is dropped: kept[i] is
 * 1 for a label to draw, 0 for one dropped. Returns the number kept. The
 * nearest label never moves. */
#define PSQ_LABEL_MAX_LIFT 3
int psq_label_declutter(const psq_label_view_t *v, psq_label_layout_t *blocks,
                        int n, int *kept);

/* ----------------------------------------------------- per-frame trace */

/* What the label pass decided for one process monster in view. */
typedef enum
{
    PSQ_LABEL_DRAWN = 0,
    PSQ_LABEL_DEAD,       /* health <= 0                                   */
    PSQ_LABEL_UNLISTED,   /* its pid is not in the current poll            */
    PSQ_LABEL_FADED,      /* beyond psq_labelrange                         */
    PSQ_LABEL_CULLED,     /* box outside the view frustum, or the anchor
                             beside the view (psq_label_anchor_in_view)    */
    PSQ_LABEL_BEHIND,     /* anchor behind the eye                         */
    PSQ_LABEL_OCCLUDED,   /* world or a door/wall model hides it           */
    PSQ_LABEL_NO_ROOM,    /* the view is too small for the text            */
    PSQ_LABEL_CROWDED     /* no room clear of nearer monsters' labels      */
} psq_label_verdict_t;

/* Why the whole pass drew nothing this frame (PSQ_LABELS_OPEN: it ran). */
typedef enum
{
    PSQ_LABELS_OPEN = 0,
    PSQ_LABELS_OFF,          /* psq_labels 0                               */
    PSQ_LABELS_NO_SERVER,    /* no local server: remote game or no game    */
    PSQ_LABELS_DEMO,         /* demo playback                              */
    PSQ_LABELS_NOT_IN_GAME,  /* not fully signed on, or another map        */
    PSQ_LABELS_INTERMISSION, /* level end or finale                        */
    PSQ_LABELS_MENU          /* a menu is open over the game               */
} psq_labels_gate_t;

typedef struct
{
    int                 entity;    /* edict number                  */
    int                 pid;
    float               distance;  /* eye to anchor, units          */
    float               alpha;
    float               sx, sy;    /* anchor on screen (if projected) */
    psq_label_verdict_t verdict;
    psq_label_layout_t  layout;    /* rows 0 unless drawn            */
} psq_label_trace_t;

#define PSQ_LABEL_TRACE_MAX 64

/* psq_labels_draw.c: the last pass's gate and per-monster decisions, for
 * the offscreen render test. *count gets the number of entries, *frame the
 * host frame (host_framecount) the pass last ran in. */
const psq_label_trace_t *PSQ_LabelsLastFrame(int *count,
                                             psq_labels_gate_t *gate,
                                             int *frame);

#endif /* PSQ_LABELS_H */
