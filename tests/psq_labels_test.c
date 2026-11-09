/*
 * psQuake -- host tests for the process-label maths (src/engine/psq_labels.c).
 *
 * The requirement: a live process monster's name, and its PID when asked,
 * sits just above it on screen, wherever the view rectangle is and whatever
 * its size and fov; nothing is drawn for a point behind the camera; the
 * label stays inside the view at the edges; it fades out with distance as
 * psq_labelrange says; and untrusted process names are drawn only as short,
 * printable ASCII.
 *
 * Returns 0 if every check passes, 1 otherwise.
 */

#include "psq_labels.h"

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

#define NEAR(a, b) (fabsf((a) - (b)) < 0.01f)

/* A camera at the origin looking down +x (Quake: forward x, right -y, up
 * z), 90 x 90 degrees, over a view rectangle. */
static psq_label_view_t view(int x, int y, int w, int h, float fx, float fy)
{
    psq_label_view_t v;

    memset(&v, 0, sizeof(v));
    v.forward[0] = 1.0f;
    v.right[1] = -1.0f;
    v.up[2] = 1.0f;
    v.fov_x = fx;
    v.fov_y = fy;
    v.x = x;
    v.y = y;
    v.width = w;
    v.height = h;
    return v;
}

static void test_project_centre_and_edges(void)
{
    psq_label_view_t v = view(0, 0, 640, 480, 90.0f, 90.0f);
    float p[3];
    float sx, sy, z;

    p[0] = 100; p[1] = 0; p[2] = 0;
    CHECK(psq_label_project(&v, p, &sx, &sy, &z), "point ahead projects");
    CHECK(NEAR(sx, 320) && NEAR(sy, 240) && NEAR(z, 100),
          "point ahead lands on the view centre (%g,%g depth %g)", sx, sy, z);

    /* 45 degrees right with a 90 degree fov: the right edge. */
    p[0] = 100; p[1] = -100; p[2] = 0;
    CHECK(psq_label_project(&v, p, &sx, &sy, NULL) && NEAR(sx, 640) && NEAR(sy, 240),
          "45 degrees right is the right edge (%g,%g)", sx, sy);
    p[1] = 100;
    CHECK(psq_label_project(&v, p, &sx, &sy, NULL) && NEAR(sx, 0),
          "45 degrees left is the left edge (%g)", sx);
    p[1] = 0; p[2] = 100;
    CHECK(psq_label_project(&v, p, &sx, &sy, NULL) && NEAR(sy, 0) && NEAR(sx, 320),
          "45 degrees up is the top edge, y down (%g,%g)", sx, sy);
    p[2] = -100;
    CHECK(psq_label_project(&v, p, &sx, &sy, NULL) && NEAR(sy, 480),
          "45 degrees down is the bottom edge (%g)", sy);

    /* Beyond the fov: projects, off the view (the caller culls). */
    p[0] = 100; p[1] = -300; p[2] = 0;
    CHECK(psq_label_project(&v, p, &sx, &sy, NULL) && sx > 640,
          "a point outside the fov projects off the right of the view (%g)", sx);
}

static void test_project_behind_and_near(void)
{
    psq_label_view_t v = view(0, 0, 640, 480, 90.0f, 73.74f);
    float p[3] = { -100, 0, 0 };
    float sx = 12345, sy = 12345;

    CHECK(!psq_label_project(&v, p, &sx, &sy, NULL), "point behind the camera is refused");
    CHECK(sx == 12345 && sy == 12345, "a refused point leaves the outputs alone");
    p[0] = 0; p[1] = -50;
    CHECK(!psq_label_project(&v, p, &sx, &sy, NULL), "point level with the eye is refused");
    p[0] = 0.5f; p[1] = 0;
    CHECK(!psq_label_project(&v, p, &sx, &sy, NULL), "point closer than the near limit is refused");
    p[0] = PSQ_LABEL_NEAR_Z;
    CHECK(psq_label_project(&v, p, &sx, &sy, NULL), "point at the near limit projects");
    /* Behind, but far to the side: still refused (no mirrored label). */
    p[0] = -1; p[1] = -10000;
    CHECK(!psq_label_project(&v, p, &sx, &sy, NULL), "point behind and aside is refused");
}

static void test_project_fov_and_rect(void)
{
    psq_label_view_t wide = view(0, 0, 640, 480, 90.0f, 90.0f);
    psq_label_view_t narrow = view(0, 0, 640, 480, 60.0f, 60.0f);
    psq_label_view_t small = view(0, 0, 320, 200, 90.0f, 90.0f);
    psq_label_view_t big = view(0, 0, 3840, 2112, 90.0f, 90.0f);
    psq_label_view_t offset = view(100, 40, 640, 480, 90.0f, 90.0f);
    float p[3] = { 200, -50, 30 };
    float wx = 0, wy = 0, nx = 0, ny = 0, sx = 0, sy = 0, bx = 0, by = 0, ox = 0, oy = 0;

    CHECK(psq_label_project(&wide, p, &wx, &wy, NULL)
          && psq_label_project(&narrow, p, &nx, &ny, NULL),
          "same point under two fovs");
    CHECK(nx > wx && ny < wy,
          "a narrower fov moves an off-centre point outward (%g>%g, %g<%g)",
          nx, wx, ny, wy);
    /* tan(30)/tan(45): the offset from centre grows by 1/tan(30). */
    CHECK(NEAR((nx - 320) * tanf(30.0f * 3.14159265f / 180.0f), (wx - 320)),
          "the fov scales the offset by its tangent");

    /* Window sizes: the same normalised place in every rectangle. */
    CHECK(psq_label_project(&small, p, &sx, &sy, NULL)
          && psq_label_project(&big, p, &bx, &by, NULL),
          "same point in a small and a large view");
    CHECK(NEAR(sx / 320.0f, bx / 3840.0f) && NEAR(sy / 200.0f, by / 2112.0f),
          "the same relative position at 320x200 and 3840x2112");
    CHECK(psq_label_project(&offset, p, &ox, &oy, NULL)
          && NEAR(ox, wx + 100) && NEAR(oy, wy + 40),
          "a view rectangle's origin offsets the point (%g,%g)", ox, oy);
}

static void test_project_degenerate(void)
{
    psq_label_view_t v;
    float p[3] = { 100, 0, 0 };
    float sx, sy;

    v = view(0, 0, 640, 480, 0.0f, 90.0f);
    CHECK(!psq_label_project(&v, p, &sx, &sy, NULL), "fov 0 is refused");
    v = view(0, 0, 640, 480, 90.0f, 180.0f);
    CHECK(!psq_label_project(&v, p, &sx, &sy, NULL), "fov 180 is refused");
    v = view(0, 0, 640, 480, NAN, 90.0f);
    CHECK(!psq_label_project(&v, p, &sx, &sy, NULL), "a NaN fov is refused");
    v = view(0, 0, 0, 480, 90.0f, 90.0f);
    CHECK(!psq_label_project(&v, p, &sx, &sy, NULL), "an empty view is refused");
    v = view(0, 0, 640, 480, 90.0f, 90.0f);
    p[1] = NAN;
    CHECK(!psq_label_project(&v, p, &sx, &sy, NULL), "a NaN point is refused");
    p[1] = INFINITY;
    CHECK(!psq_label_project(&v, p, &sx, &sy, NULL), "an infinite point is refused");
    p[1] = 0;
    v.origin[2] = NAN;
    CHECK(!psq_label_project(&v, p, &sx, &sy, NULL), "a NaN eye is refused");
    CHECK(!psq_label_project(NULL, p, &sx, &sy, NULL), "no view is refused");
}

static void test_fade(void)
{
    float full[4], gone[4];
    int r;

    for (r = 0; r < 4; r++)
        psq_label_range_bounds(r, &full[r], &gone[r]);
    CHECK(gone[0] < gone[1] && gone[1] < gone[2],
          "near < normal < far (%g %g %g)", gone[0], gone[1], gone[2]);
    CHECK(full[3] == 0 && gone[3] == 0, "all never fades");
    for (r = 0; r < 3; r++)
    {
        float d;
        float last = 2.0f;
        int monotonic = 1;

        CHECK(full[r] > 0 && full[r] < gone[r], "range %d: full before gone", r);
        CHECK(psq_label_alpha(r, 0) == 1.0f, "range %d: opaque at the eye", r);
        CHECK(psq_label_alpha(r, full[r]) == 1.0f, "range %d: opaque at full", r);
        CHECK(NEAR(psq_label_alpha(r, (full[r] + gone[r]) / 2), 0.5f),
              "range %d: half way is half opaque", r);
        CHECK(psq_label_alpha(r, gone[r]) == 0.0f, "range %d: gone at gone", r);
        CHECK(psq_label_alpha(r, gone[r] * 10) == 0.0f, "range %d: gone beyond", r);
        for (d = 0; d < 5000; d += 7.5f)
        {
            float a = psq_label_alpha(r, d);
            if (a > last || a < 0 || a > 1)
                monotonic = 0;
            last = a;
        }
        CHECK(monotonic, "range %d: alpha stays in 0..1 and never rises with distance", r);
    }
    CHECK(psq_label_alpha(3, 1e9f) == 1.0f, "all: opaque at any distance");
    CHECK(psq_label_alpha(1, -1) == 0 && psq_label_alpha(3, -1) == 0,
          "a negative distance is not drawn");
    CHECK(psq_label_alpha(1, NAN) == 0 && psq_label_alpha(3, INFINITY) == 0,
          "a non-finite distance is not drawn");
    {
        float f, g;
        psq_label_range_bounds(99, &f, &g);
        CHECK(f == full[1] && g == gone[1], "an unknown range fades as normal");
        psq_label_range_bounds(-5, &f, &g);
        CHECK(f == full[1] && g == gone[1], "a negative range fades as normal");
    }
}

static void test_pid(void)
{
    CHECK(psq_label_pid(2) == 2 && psq_label_pid(4242) == 4242, "process pids pass");
    CHECK(psq_label_pid(16777216.0f) == 16777216, "the largest exact float pid passes");
    CHECK(psq_label_pid(0) == 0 && psq_label_pid(1) == 0, "0 and 1 are not process monsters");
    CHECK(psq_label_pid(-1) == 0, "the swarm tag gets no label");
    CHECK(psq_label_pid(-3) == 0, "the corpse tag gets no label");
    CHECK(psq_label_pid(2.5f) == 0, "a fraction is refused");
    CHECK(psq_label_pid(16777218.0f) == 0 && psq_label_pid(1e30f) == 0,
          "beyond a float's exact range is refused");
    CHECK(psq_label_pid(NAN) == 0 && psq_label_pid(INFINITY) == 0, "non-finite is refused");
}

static void sanitize_is(const char *in, size_t in_max, const char *want)
{
    char out[PSQ_LABEL_TEXT_MAX];
    size_t n = psq_label_sanitize(out, sizeof(out), in, in_max);

    CHECK(strcmp(out, want) == 0 && n == strlen(want),
          "sanitize \"%s\" -> \"%s\", want \"%s\"", in, out, want);
}

static void test_sanitize(void)
{
    char out[PSQ_LABEL_TEXT_MAX];
    char unterminated[4] = { 'a', 'b', 'c', 'd' };
    size_t i;
    int printable;

    sanitize_is("Finder", 64, "Finder");
    sanitize_is("WindowServer", 64, "WindowServer");
    sanitize_is("a\tb   c", 64, "a b c");
    sanitize_is("   lead", 64, "lead");
    sanitize_is("trail   ", 64, "trail");
    sanitize_is("", 64, "");
    sanitize_is("   ", 64, "");
    sanitize_is("\x1b[31mred", 64, "?[31mred");
    sanitize_is("bell\x07", 64, "bell?");
    sanitize_is("del\x7f", 64, "del?");
    sanitize_is("caf\xc3\xa9", 64, "caf?");               /* one UTF-8 char */
    sanitize_is("\xe2\x9c\x93ok", 64, "?ok");              /* 3-byte sequence */
    sanitize_is("\xf0\x9f\x98\x80!", 64, "?!");            /* 4-byte sequence */
    sanitize_is("x\x80y", 64, "x?y");                      /* stray byte     */
    sanitize_is("\xff\xfe", 64, "??");
    sanitize_is("%s%n%x", 64, "%s%n%x");                   /* drawn, never formatted */
    sanitize_is("0123456789abcdefghij", 64, "0123456789abcde");
    sanitize_is("abcdefghijklmn xyz", 64, "abcdefghijklmn");   /* no trailing space */
    sanitize_is("abcdefghijklm xyz", 64, "abcdefghijklm x");
    sanitize_is("helper", 3, "hel");

    CHECK(psq_label_sanitize(out, sizeof(out), unterminated, sizeof(unterminated)) == 4
          && strcmp(out, "abcd") == 0,
          "an unterminated name is read only up to in_max");
    CHECK(psq_label_sanitize(out, 4, "abcdef", 64) == 3 && strcmp(out, "abc") == 0,
          "the output size bounds the result");
    CHECK(psq_label_sanitize(out, 1, "abcdef", 64) == 0 && out[0] == '\0',
          "a one-byte output is just the terminator");
    out[0] = 'Z';
    CHECK(psq_label_sanitize(out, 0, "abcdef", 64) == 0 && out[0] == 'Z',
          "a zero-size output is not written");
    CHECK(psq_label_sanitize(out, sizeof(out), NULL, 64) == 0 && out[0] == '\0',
          "no name is empty");

    /* Every byte value: only printable ASCII ever comes out. */
    printable = 1;
    for (i = 1; i < 256; i++)
    {
        char in[2];
        size_t j;

        in[0] = (char) i;
        in[1] = '\0';
        psq_label_sanitize(out, sizeof(out), in, sizeof(in));
        for (j = 0; out[j]; j++)
            if ((unsigned char) out[j] < 0x20 || (unsigned char) out[j] > 0x7e)
                printable = 0;
    }
    CHECK(printable, "every single byte sanitizes to printable ASCII");
}

static void test_text_scale(void)
{
    CHECK(psq_label_text_scale(200) == 1 && psq_label_text_scale(359) == 1,
          "small views draw at 1x");
    CHECK(psq_label_text_scale(720) == 2 && psq_label_text_scale(1032) == 2,
          "720p draws at 2x");
    CHECK(psq_label_text_scale(1080) == 3, "1080p draws at 3x");
    CHECK(psq_label_text_scale(2160) == 6 && psq_label_text_scale(9000) == 6,
          "4K draws at 6x, and that is the most");
    CHECK(psq_label_text_scale(0) == 1 && psq_label_text_scale(-5) == 1,
          "a degenerate height still gets 1x");
}

static void test_layout(void)
{
    psq_label_view_t v = view(0, 0, 640, 480, 90.0f, 90.0f);
    psq_label_view_t off = view(100, 50, 640, 480, 90.0f, 90.0f);
    psq_label_layout_t l;
    char longtext[PSQ_LABEL_TEXT_MAX + 4];

    CHECK(psq_label_layout(&v, 320, 200, 1, NULL, "abc", &l) && l.rows == 1,
          "a name alone is one row");
    CHECK(l.x[0] == 308 && l.width[0] == 24, "centred on the anchor (%d)", l.x[0]);
    CHECK(l.bottom == 199 && l.y[0] == 191 && l.top == 191,
          "its bottom one step above the anchor (y %d bottom %d)", l.y[0], l.bottom);
    CHECK(strcmp(l.text[0], "abc") == 0, "the row says the name");

    CHECK(psq_label_layout(&v, 320, 200, 2, "4242", "editor", &l) && l.rows == 2,
          "pid and name are two rows");
    CHECK(strcmp(l.text[0], "4242") == 0 && strcmp(l.text[1], "editor") == 0,
          "the pid row is on top (psDoom's order)");
    CHECK(l.y[1] - l.y[0] == 18 && l.bottom == 198 && l.top == 198 - 34,
          "rows 16 px tall at 2x with a 2 px gap (%d %d %d)", l.y[0], l.y[1], l.bottom);
    CHECK(l.x[0] == 320 - 32 && l.x[1] == 320 - 48, "each row centred on its own width");

    CHECK(psq_label_layout(&v, 320, 200, 1, "", "", &l) == 0 && l.rows == 0,
          "nothing to say draws nothing");
    CHECK(psq_label_layout(&v, 320, 200, 1, "77", NULL, &l) && l.rows == 1
          && strcmp(l.text[0], "77") == 0, "a pid alone is one row");

    /* Edges: pinned inside the view. */
    CHECK(psq_label_layout(&v, -50, 200, 1, NULL, "abcdef", &l) && l.x[0] == 0,
          "off the left: pinned to the left edge (%d)", l.x[0]);
    CHECK(psq_label_layout(&v, 700, 200, 1, NULL, "abcdef", &l) && l.x[0] == 640 - 48,
          "off the right: pinned to the right edge (%d)", l.x[0]);
    CHECK(psq_label_layout(&v, 320, -100, 1, NULL, "abc", &l) && l.top == 0,
          "above the top: pinned to the top (%d)", l.top);
    CHECK(psq_label_layout(&v, 320, 4, 1, "1", "abc", &l) && l.top == 0 && l.y[1] == 9,
          "close to the top: the block keeps both rows on screen");
    CHECK(psq_label_layout(&v, 320, 9000, 1, NULL, "abc", &l) && l.bottom == 480,
          "below the bottom: pinned to the bottom (%d)", l.bottom);
    CHECK(psq_label_layout(&off, 0, 0, 1, NULL, "abc", &l) && l.x[0] == 100 && l.top == 50,
          "a view rectangle's own origin is its top-left edge");
    CHECK(psq_label_layout(&off, 9999, 9999, 1, NULL, "abc", &l)
          && l.x[0] + l.width[0] == 740 && l.bottom == 530,
          "and its far corner");

    /* No overflow on wild input. */
    CHECK(psq_label_layout(&v, 1e30f, -1e30f, 1, NULL, "abc", &l)
          && l.x[0] == 640 - 24 && l.top == 0, "huge anchors are clamped");
    CHECK(!psq_label_layout(&v, NAN, 10, 1, NULL, "abc", &l) && l.rows == 0,
          "a NaN anchor draws nothing");
    CHECK(!psq_label_layout(&v, 10, INFINITY, 1, NULL, "abc", &l), "an infinite anchor draws nothing");

    /* Too little room. */
    CHECK(!psq_label_layout(&v, 320, 200, 6, NULL, "0123456789abcde", &l) && l.rows == 0,
          "a row wider than the view is not drawn");
    {
        psq_label_view_t tiny = view(0, 0, 640, 12, 90.0f, 90.0f);
        CHECK(!psq_label_layout(&tiny, 320, 6, 1, "1", "abc", &l),
              "a block taller than the view is not drawn");
        CHECK(psq_label_layout(&tiny, 320, 6, 1, NULL, "abc", &l), "one row fits in 12 px");
    }
    memset(longtext, 'a', sizeof(longtext) - 1);
    longtext[sizeof(longtext) - 1] = '\0';
    CHECK(!psq_label_layout(&v, 320, 200, 1, NULL, longtext, &l), "over-long text is refused");
    CHECK(!psq_label_layout(&v, 320, 200, 0, NULL, "abc", &l), "scale 0 is refused");
    CHECK(!psq_label_layout(NULL, 320, 200, 1, NULL, "abc", &l), "no view is refused");
}

/* Projection and layout together, as the draw pass uses them: a monster
 * ahead and right, at three window sizes, keeps its label above itself. */
static void test_end_to_end_sizes(void)
{
    static const int sizes[][2] = { { 640, 432 }, { 1280, 672 }, { 1920, 1032 }, { 2560, 1552 } };
    size_t i;

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
        int w = sizes[i][0], h = sizes[i][1];
        float fy = 2.0f * atanf(tanf(45.0f * 3.14159265f / 180.0f) * h / w) * 180.0f / 3.14159265f;
        psq_label_view_t v = view(0, 0, w, h, 90.0f, fy);
        float head[3] = { 400, -100, 40 };
        float feet[3] = { 400, -100, -24 };
        float hx = 0, hy = 0, fx = 0, fyy = 0;
        psq_label_layout_t l;
        int scale = psq_label_text_scale(h);

        CHECK(psq_label_project(&v, head, &hx, &hy, NULL)
              && psq_label_project(&v, feet, &fx, &fyy, NULL), "%dx%d: projects", w, h);
        CHECK(psq_label_layout(&v, hx, hy, scale, "4242", "editor", &l),
              "%dx%d: lays out", w, h);
        CHECK(l.bottom <= (int) hy && l.bottom < (int) fyy,
              "%dx%d: the label sits above the monster", w, h);
        CHECK(l.x[1] <= (int) hx && l.x[1] + l.width[1] >= (int) hx,
              "%dx%d: the name spans the anchor", w, h);
        CHECK(NEAR(hx / w, 0.5f + 0.5f * 100.0f / 400.0f),
              "%dx%d: the same relative x (%g)", w, h, hx / w);
    }
}

int main(void)
{
    test_project_centre_and_edges();
    test_project_behind_and_near();
    test_project_fov_and_rect();
    test_project_degenerate();
    test_fade();
    test_pid();
    test_sanitize();
    test_text_scale();
    test_layout();
    test_end_to_end_sizes();

    printf("psq_labels_test: %d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
