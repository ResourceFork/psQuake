/*
 * psQuake -- process labels: projection, fade, text and layout
 * (psq_labels.h). Engine-free; tests/psq_labels_test.c covers it.
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#include "psq_labels.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* psq_labelrange values: psproc_options.h's PSD_LABELS_* (near, normal,
 * far, all). psq_labels_draw.c checks the two agree. */
enum { RANGE_NEAR = 0, RANGE_NORMAL, RANGE_FAR, RANGE_ALL };

/* Fade distances, units. Each "gone" distance is where psDoom's label
 * scale cut-off falls for its option (a Doom unit is about a Quake unit:
 * both players are 56 tall), and the fade starts at three quarters of it. */
static const float range_gone[3] = { 640.0f, 1280.0f, 3584.0f };

#define PSQ_DEG2RAD(d) ((d) * 3.14159265358979323846 / 180.0)

static int finite3(const float p[3])
{
    return isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]);
}

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

int psq_label_project(const psq_label_view_t *v, const float point[3],
                      float *sx, float *sy, float *depth)
{
    float  d[3];
    float  z;
    double tx, ty;
    double nx, ny;

    if (v == NULL || point == NULL || sx == NULL || sy == NULL)
        return 0;
    if (v->width <= 0 || v->height <= 0)
        return 0;
    if (!(v->fov_x > 0.0f && v->fov_x < 180.0f)
        || !(v->fov_y > 0.0f && v->fov_y < 180.0f))
        return 0;
    if (!finite3(point) || !finite3(v->origin) || !finite3(v->forward)
        || !finite3(v->right) || !finite3(v->up))
        return 0;

    d[0] = point[0] - v->origin[0];
    d[1] = point[1] - v->origin[1];
    d[2] = point[2] - v->origin[2];
    z = dot3(d, v->forward);
    if (!(z >= PSQ_LABEL_NEAR_Z))
        return 0;

    tx = tan(PSQ_DEG2RAD(v->fov_x) / 2.0);
    ty = tan(PSQ_DEG2RAD(v->fov_y) / 2.0);
    nx = dot3(d, v->right) / (z * tx);   /* -1 left edge, +1 right edge */
    ny = dot3(d, v->up) / (z * ty);      /* -1 bottom, +1 top           */

    *sx = (float) (v->x + v->width * (0.5 + 0.5 * nx));
    *sy = (float) (v->y + v->height * (0.5 - 0.5 * ny));
    if (depth != NULL)
        *depth = z;
    return isfinite(*sx) && isfinite(*sy);
}

void psq_label_range_bounds(int range, float *full, float *gone)
{
    float g;

    if (range == RANGE_ALL)
    {
        *full = 0.0f;
        *gone = 0.0f;
        return;
    }
    if (range < RANGE_NEAR || range > RANGE_FAR)
        range = RANGE_NORMAL;
    g = range_gone[range];
    *full = g * 0.75f;
    *gone = g;
}

float psq_label_alpha(int range, float distance)
{
    float full, gone;

    if (!(distance >= 0.0f) || !isfinite(distance))
        return 0.0f;
    psq_label_range_bounds(range, &full, &gone);
    if (gone <= 0.0f)
        return 1.0f;            /* PSD_LABELS_ALL */
    if (distance <= full)
        return 1.0f;
    if (distance >= gone)
        return 0.0f;
    return (gone - distance) / (gone - full);
}

int psq_label_pid(float value)
{
    if (!(value >= 2.0f && value <= (float) PSQ_LABEL_PID_MAX))
        return 0;
    if (value != floorf(value))
        return 0;
    return (int) value;
}

size_t psq_label_sanitize(char *out, size_t out_size, const char *in,
                          size_t in_max)
{
    size_t limit;
    size_t n = 0;
    size_t i = 0;
    int    pending_space = 0;

    if (out == NULL || out_size == 0)
        return 0;
    out[0] = '\0';
    if (in == NULL)
        return 0;
    limit = out_size - 1;
    if (limit > PSQ_LABEL_NAME_CHARS)
        limit = PSQ_LABEL_NAME_CHARS;

    while (i < in_max && in[i] != '\0' && n < limit)
    {
        unsigned char c = (unsigned char) in[i++];
        char          shown;

        if (c == ' ' || c == '\t')
        {
            pending_space = n > 0;   /* leading blanks are dropped */
            continue;
        }
        if (c >= 0x21 && c <= 0x7e)
            shown = (char) c;
        else
        {
            shown = '?';
            if (c >= 0xc0)
            {
                /* A UTF-8 lead byte: its continuation bytes are one
                 * character with it. */
                while (i < in_max && ((unsigned char) in[i] & 0xc0) == 0x80)
                    i++;
            }
        }
        if (pending_space)
        {
            if (n + 1 >= limit)
                break;          /* never end on the space */
            out[n++] = ' ';
            pending_space = 0;
        }
        out[n++] = shown;
    }
    out[n] = '\0';
    return n;
}

int psq_label_text_scale(int view_height)
{
    int s = view_height / 360;

    if (s < 1)
        s = 1;
    if (s > 6)
        s = 6;
    return s;
}

/* The nearest int to f, with f first held inside [lo, hi] (so no float out
 * of int range is ever converted). */
static int clamp_round(float f, int lo, int hi)
{
    if (f < (float) lo)
        return lo;
    if (f > (float) hi)
        return hi;
    return (int) floorf(f + 0.5f);
}

int psq_label_layout(const psq_label_view_t *v, float sx, float sy, int scale,
                     const char *pid_text, const char *name,
                     psq_label_layout_t *out)
{
    const char *rows[2];
    int         nrows = 0;
    int         row_h, pitch, height;
    int         top, ax, ay;
    int         r;

    if (out == NULL)
        return 0;
    memset(out, 0, sizeof(*out));
    if (v == NULL || v->width <= 0 || v->height <= 0 || scale < 1)
        return 0;
    if (!isfinite(sx) || !isfinite(sy))
        return 0;

    if (pid_text != NULL && pid_text[0] != '\0')
        rows[nrows++] = pid_text;
    if (name != NULL && name[0] != '\0')
        rows[nrows++] = name;
    if (nrows == 0)
        return 0;

    row_h = PSQ_LABEL_GLYPH * scale;
    pitch = row_h + scale;
    height = nrows * row_h + (nrows - 1) * scale;
    if (height > v->height)
        return 0;

    /* Anchor in pixels, held within a view's width or height of the view
     * so the arithmetic below cannot overflow; the block is pinned to the
     * view edges anyway. */
    ax = clamp_round(sx, v->x - v->width, v->x + 2 * v->width);
    ay = clamp_round(sy, v->y - v->height, v->y + 2 * v->height);

    top = ay - scale - height;
    if (top < v->y)
        top = v->y;
    if (top > v->y + v->height - height)
        top = v->y + v->height - height;

    for (r = 0; r < nrows; r++)
    {
        size_t len = strlen(rows[r]);
        int    w;
        int    x;

        if (len >= PSQ_LABEL_TEXT_MAX)
            return 0;
        w = (int) len * PSQ_LABEL_GLYPH * scale;
        if (w > v->width)
        {
            memset(out, 0, sizeof(*out));
            return 0;
        }
        x = ax - w / 2;
        if (x < v->x)
            x = v->x;
        if (x > v->x + v->width - w)
            x = v->x + v->width - w;
        memcpy(out->text[r], rows[r], len + 1);
        out->x[r] = x;
        out->y[r] = top + r * pitch;
        out->width[r] = w;
    }
    out->rows = nrows;
    out->top = top;
    out->bottom = top + height;
    out->scale = scale;
    return 1;
}
