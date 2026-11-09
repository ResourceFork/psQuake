/*
 * psQuake -- offscreen render test of the process labels.
 *
 * Built only into psq_labels_render: the real QuakeSpasm engine and
 * renderer, psQuake's glue, policy and label pass, the compiled progs.dat
 * and psproc_fake (no native process backend: psproc_native_actions_unlinked
 * checks the binary with nm). The video layer is tests/psq_offscreen_sdl.c:
 * frames go to a framebuffer object on a windowless CGL context, so no
 * window, Dock icon or focus change ever reaches the screen.
 *
 * It plays LibreQuake's lq_e1m1 with psq_testarena and six fake processes,
 * then, one engine frame at a time (the console command psq_render_tick):
 *
 *   setup     the player looks down the longest open line; five process
 *             monsters stand along it at 200..1150 units; a sixth stands
 *             where the player's PVS reaches it but walls hide it
 *   demo      two seconds of real time recorded as a demo, then the clock
 *             is all but stopped so frames compare pixel for pixel
 *   scenes    window and fullscreen sizes, PID on and off, each label
 *             range, labels off, the menu open, the status bar hidden; for
 *             each it writes a PNG of the frame and checks the label trace
 *             of the same frame (gate, verdicts, positions, text, no
 *             overlap). Then two more frames without labels, the second
 *             with the nearest labelled monster hidden: label pixels must
 *             differ where the trace puts them, and the label must sit
 *             centred just above the pixels the monster itself covers. A
 *             part-faded label (near range) must change the scene by its
 *             alpha times what the same label does opaque (far range)
 *   end       a real intermission (svc_intermission) and playback of the
 *             demo: neither may draw a label; no NSApplication was made and
 *             SDL's own video subsystem never started
 *
 * With --arena (ctest psq_labels_render_arena) it runs New Game instead, as
 * the Single Player menu does, with psq_testarena off:
 *
 *   arena     the arena map opens; the six process monsters teleport onto
 *             grid cells of their own (checked against the psq_arena grid)
 *             and stand there (the player is notarget); then each moves to
 *             a fixed cell, so every run frames the same scene, one of them
 *             (EDGE_PID) with its box in view and its head beside it
 *   scenes    the player stands at the hall's south end looking over the
 *             grid; at 1280x720 and 2560x1600 fullscreen, names and PIDs,
 *             the five in view are labelled with their own names, the edge
 *             one is culled, nothing is unlisted, no labels overlap, and
 *             label pixels sit where the trace puts them
 *   exit      the console's changelevel lq_e1m1 (the exit trigger is the
 *             headless arena tests'): no process monster or controller
 *             there, and a level monster given a listed pid's tag (as a
 *             crafted map or save could) gets no label: the pass is gated
 *
 * The default run also turns psq_testarena off mid-level with the clock
 * stopped (no builtin runs, the pool still lists the six): the pass must
 * be gated at once, and open again with psq_testarena back on.
 *
 * Every frame's label trace is logged ("psq-render: frame ..."). Exits 0
 * when every check passes, 1 on any failure (Sys_Error), 77 when LibreQuake
 * data is missing or no offscreen GL context can be made (skip).
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#define main psq_engine_main
#include "main_sdl.c"
#undef main

#include "psq_arena.h"
#include "psq_engine.h"
#include "psq_grid.h"
#include "psq_labels.h"
#include "psq_offscreen.h"
#include "psq_policy.h"
#include "proc_fake.h"
#include "psproc_options.h"

#include <OpenGL/OpenGL.h>
#include <dlfcn.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#define UID 501

static int checks_run;
static int checks_failed;

#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        checks_run++;                                                     \
        if (cond) {                                                       \
            Sys_Printf("psq-render PASS: ");                              \
        } else {                                                          \
            checks_failed++;                                              \
            Sys_Printf("psq-render FAIL: ");                              \
        }                                                                 \
        Sys_Printf(__VA_ARGS__);                                          \
        Sys_Printf("\n");                                                 \
    } while (0)

/* -------------------------------------------------------- fake processes */

typedef struct
{
    int                pid;
    const char        *name;
    unsigned long long mb;
    const char        *label;   /* what the label must say */
} fake_t;

/* Names as a process list would give them: the long one is cut by
 * psproc's 16-byte name, the hostile one sanitized by the policy and the
 * label pass. */
static const fake_t fakes[] = {
    { 3101, "editor",                       10,   "editor" },
    { 3102, "Terminal",                     40,   "Terminal" },
    { 3103, "com.apple.WebKit.WebContent",  150,  "com.apple.WebKi" },
    { 3104, "bad\x1b[2J\xc3\xa9name",        400,  "bad?[2J??name" },
    { 3105, "Google Chrome H",              1000, "Google Chrome H" },
    { 3106, "backupd",                      30,   "backupd" },
};
#define NFAKES ((int) (sizeof(fakes) / sizeof(fakes[0])))
#define HIDDEN_PID 3106
#define EDGE_PID 3101     /* --arena: the monster placed beside the view */
#define PLAYER (svs.clients[0].edict)

void psq_test_install_backend(void)
{
    psd_proc_t procs[NFAKES];
    int i;

    proc_fake_install(UID);
    for (i = 0; i < NFAKES; i++) {
        memset(&procs[i], 0, sizeof(procs[i]));
        procs[i].pid = fakes[i].pid;
        procs[i].uid = UID;
        procs[i].start_token = 100 + i;
        procs[i].footprint = fakes[i].mb << 20;
        q_strlcpy(procs[i].name, fakes[i].name, sizeof(procs[i].name));
        if (procs[i].pid == (int) getpid())
            Sys_Error("psq-render: a fake pid equals this process's pid");
    }
    proc_fake_push_snapshot(procs, NFAKES);   /* repeated on every poll */
}

static const char *label_for(int pid)
{
    int i;
    for (i = 0; i < NFAKES; i++)
        if (fakes[i].pid == pid)
            return fakes[i].label;
    return NULL;
}

/* ---------------------------------------------------------- entity help */

static float pid_of(edict_t *e)
{
    eval_t *v = GetEdictFieldValue(e, "psq_pid");
    return v ? v->_float : 0;
}

static edict_t *monster_for(int pid)
{
    int i;
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        if (!e->free && pid_of(e) == pid && e->v.health > 0)
            return e;
    }
    return NULL;
}

static int live_process_monsters(void)
{
    int i, n = 0;
    for (i = 0; i < NFAKES; i++)
        if (monster_for(fakes[i].pid))
            n++;
    return n;
}

static void angle_dir(float yaw_deg, vec3_t fwd, vec3_t right)
{
    float y = yaw_deg * (float) M_PI / 180.0f;
    fwd[0] = cosf(y); fwd[1] = sinf(y); fwd[2] = 0;
    right[0] = sinf(y); right[1] = -cosf(y); right[2] = 0;
}

static float clear_length(vec3_t from, vec3_t dir, float max)
{
    vec3_t to;
    trace_t tr;
    VectorMA(from, max, dir, to);
    tr = SV_Move(from, vec3_origin, vec3_origin, to, MOVE_NOMONSTERS, PLAYER);
    return tr.startsolid ? 0 : tr.fraction * max;
}

static int hull_fits(edict_t *m, vec3_t p)
{
    trace_t tr = SV_Move(p, m->v.mins, m->v.maxs, p, MOVE_NOMONSTERS, m);
    return !tr.startsolid && !tr.allsolid;
}

/* Drop p to the floor with m's hull; 0 if no floor within 512 units. */
static int drop_to_floor(edict_t *m, vec3_t p)
{
    vec3_t down;
    trace_t tr;
    VectorCopy(p, down);
    down[2] -= 512;
    tr = SV_Move(p, m->v.mins, m->v.maxs, down, MOVE_NOMONSTERS, m);
    if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f)
        return 0;
    VectorCopy(tr.endpos, p);
    return 1;
}

/* Stand m at p facing yaw, and stop its thinking so the scenes hold still. */
static void place(edict_t *m, vec3_t p, float yaw)
{
    VectorCopy(p, m->v.origin);
    VectorCopy(vec3_origin, m->v.velocity);
    m->v.angles[0] = m->v.angles[2] = 0;
    m->v.angles[1] = yaw;
    m->v.nextthink = sv.time + 100000;
    m->v.flags = (float) ((int) m->v.flags | FL_ONGROUND);
    SV_LinkEdict(m, false);
}

static int line_clear(vec3_t a, vec3_t b)
{
    trace_t tr = SV_Move(a, vec3_origin, vec3_origin, b, MOVE_NOMONSTERS, PLAYER);
    return !tr.startsolid && !tr.allsolid && tr.fraction >= 1.0f;
}

static int in_pvs(vec3_t eye, vec3_t p)
{
    mleaf_t *from = Mod_PointInLeaf(eye, sv.worldmodel);
    mleaf_t *to = Mod_PointInLeaf(p, sv.worldmodel);
    byte *pvs = Mod_LeafPVS(from, sv.worldmodel);
    int leafnum = (int) (to - sv.worldmodel->leafs) - 1;
    return leafnum >= 0 && (pvs[leafnum >> 3] & (1 << (leafnum & 7)));
}

/* ------------------------------------------------------------------ setup */

static vec3_t eye;
static float  view_yaw;

/* How far along dir from the standing spot p the floor stays level
 * (within 32 units), checked every 32 units up to max. */
static float level_floor(vec3_t p, vec3_t dir, float max)
{
    float t;

    for (t = 32; t <= max; t += 32) {
        vec3_t a, b;
        trace_t tr;
        VectorMA(p, t, dir, a);
        a[2] += 16;
        VectorCopy(a, b);
        b[2] -= 64;
        tr = SV_Move(a, vec3_origin, vec3_origin, b, MOVE_NOMONSTERS, PLAYER);
        if (tr.startsolid || tr.fraction >= 1.0f || fabsf(tr.endpos[2] - (p[2] + PLAYER->v.mins[2])) > 32)
            return t - 32;
    }
    return max;
}

/* The longest open line from p's eye, 128 units wide (three parallel
 * traces), over 72 headings. */
static float best_line(vec3_t p, float *yaw_out)
{
    vec3_t e, fwd, right, side;
    float best = -1;
    int yaw;

    VectorAdd(p, PLAYER->v.view_ofs, e);
    for (yaw = 0; yaw < 360; yaw += 5) {
        float len;
        angle_dir((float) yaw, fwd, right);
        len = clear_length(e, fwd, 3000);
        VectorMA(e, 64, right, side);
        len = fminf(len, clear_length(side, fwd, 3000));
        VectorMA(e, -64, right, side);
        len = fminf(len, clear_length(side, fwd, 3000));
        len = fminf(len, level_floor(p, fwd, len));
        if (len > best) {
            best = len;
            *yaw_out = (float) yaw;
        }
    }
    return best;
}

/* Stand the player where the level has its longest open line (searched
 * from every entity's spot), looking down it. */
static void setup_camera(void)
{
    vec3_t best_p;
    float best = -1, best_yaw = 0;
    int i;

    VectorCopy(PLAYER->v.origin, best_p);
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        vec3_t p, down;
        trace_t tr;
        float yaw = 0, len;

        if (e->free || e->v.solid == SOLID_BSP)
            continue;
        VectorCopy(e->v.origin, p);
        p[2] += 24;
        if (SV_PointContents(p) != CONTENTS_EMPTY)
            continue;
        tr = SV_Move(p, PLAYER->v.mins, PLAYER->v.maxs, p, MOVE_NOMONSTERS, PLAYER);
        if (tr.startsolid || tr.allsolid)
            continue;
        VectorCopy(p, down);
        down[2] -= 256;
        tr = SV_Move(p, PLAYER->v.mins, PLAYER->v.maxs, down, MOVE_NOMONSTERS, PLAYER);
        if (tr.startsolid || tr.fraction >= 1.0f)
            continue;
        VectorCopy(tr.endpos, p);
        len = best_line(p, &yaw);
        if (len > best) {
            best = len;
            best_yaw = yaw;
            VectorCopy(p, best_p);
        }
    }
    VectorCopy(best_p, PLAYER->v.origin);
    VectorCopy(vec3_origin, PLAYER->v.velocity);
    SV_LinkEdict(PLAYER, false);
    view_yaw = best_yaw;
    VectorAdd(PLAYER->v.origin, PLAYER->v.view_ofs, eye);
    PLAYER->v.angles[0] = 0;
    PLAYER->v.angles[1] = view_yaw;
    PLAYER->v.angles[2] = 0;
    VectorCopy(PLAYER->v.angles, PLAYER->v.v_angle);
    PLAYER->v.fixangle = 1;
    Sys_Printf("psq-render: eye (%.0f %.0f %.0f), yaw %.0f, %.0f units clear; "
               "scr_sbarscale %g scr_conscale %g\n",
               eye[0], eye[1], eye[2], view_yaw, best, scr_sbarscale.value, scr_conscale.value);
    CHECK(best >= 1250, "the camera has a long open line (%.0f units)", best);
}

static void setup_lineup(void)
{
    static const float dist[5] = { 200, 380, 600, 900, 1150 };
    static const float lat[5] = { -50, 60, -80, 90, 0 };
    vec3_t fwd, right;
    int i;

    angle_dir(view_yaw, fwd, right);
    for (i = 0; i < 5; i++) {
        edict_t *m = monster_for(fakes[i].pid);
        vec3_t start, target, p;
        trace_t tr;

        if (!m)
            continue;
        /* Out along the open line, then sideways, as far as m's hull goes. */
        VectorMA(PLAYER->v.origin, dist[i], fwd, target);
        VectorCopy(target, start);
        VectorMA(target, lat[i], right, target);
        tr = SV_Move(start, m->v.mins, m->v.maxs, target, MOVE_NOMONSTERS, m);
        if (tr.startsolid || tr.allsolid) {
            VectorCopy(start, p);
        } else {
            VectorCopy(tr.endpos, p);
        }
        if (!drop_to_floor(m, p) || !hull_fits(m, p))
            Sys_Printf("psq-render: pid %d: no floor at %.0f units\n", fakes[i].pid, dist[i]);
        place(m, p, view_yaw + 180);
        Sys_Printf("psq-render: pid %d (%s) at (%.0f %.0f %.0f)\n", fakes[i].pid,
                   PR_GetString(m->v.classname), p[0], p[1], p[2]);
    }
}

/* The hidden one: in the eye's PVS and inside the view, but with world
 * geometry on both lines the label pass tests (anchor and centre). */
static void setup_hidden(void)
{
    edict_t *m = monster_for(HIDDEN_PID);
    int a, found = 0;
    float r;
    vec3_t p;

    if (!m) {
        CHECK(0, "the hidden monster exists");
        return;
    }
    for (r = 128; r <= 1600 && !found; r += 16) {
        for (a = 0; a <= 70 && !found; a++) {
            /* 0, +1, -1, +2, -2 ... degrees off the view line, within
             * +-35 (inside the 4:3 fov of 90). */
            float off = (a & 1) ? (float) ((a + 1) / 2) : -(float) (a / 2);
            vec3_t fwd, right, anchor, centre;

            angle_dir(view_yaw + off * 0.5f, fwd, right);
            VectorMA(PLAYER->v.origin, r, fwd, p);
            p[2] += 32;
            if (SV_PointContents(p) != CONTENTS_EMPTY || !drop_to_floor(m, p) || !hull_fits(m, p))
                continue;
            if (!in_pvs(eye, p))
                continue;
            VectorCopy(p, anchor);
            anchor[2] += m->v.maxs[2] + 16;
            VectorCopy(p, centre);
            centre[2] += (m->v.mins[2] + m->v.maxs[2]) * 0.5f;
            if (line_clear(eye, anchor) || line_clear(eye, centre))
                continue;
            if (fabsf(p[2] - eye[2]) > r * 0.6f)
                continue;   /* stay well inside the vertical fov */
            found = 1;
        }
    }
    CHECK(found, "found a spot in the PVS and the view where walls hide a monster");
    if (found) {
        place(m, p, view_yaw + 180);
        Sys_Printf("psq-render: pid %d (%s) hidden at (%.0f %.0f %.0f)\n", HIDDEN_PID,
                   PR_GetString(m->v.classname), p[0], p[1], p[2]);
    }
}

/* ----------------------------------------------------------------- scenes */

typedef struct
{
    const char *name;
    int w, h, fullscreen;
    int showpid, range, labels, viewsize, menu;
} scene_t;

static const scene_t scenes[] = {
    { "labels-1280x720-off",        1280, 720, 0, 1, PSD_LABELS_NORMAL, 0, 100, 0 },
    { "labels-1280x720-pid",        1280, 720, 0, 1, PSD_LABELS_NORMAL, 1, 100, 0 },
    { "labels-1280x720-name",       1280, 720, 0, 0, PSD_LABELS_NORMAL, 1, 100, 0 },
    { "labels-1280x720-near",       1280, 720, 0, 1, PSD_LABELS_NEAR,   1, 100, 0 },
    { "labels-1280x720-far",        1280, 720, 0, 1, PSD_LABELS_FAR,    1, 100, 0 },
    { "labels-640x480",             640,  480, 0, 0, PSD_LABELS_NORMAL, 1, 100, 0 },
    { "labels-1920x1080-far",       1920, 1080, 0, 1, PSD_LABELS_FAR,   1, 100, 0 },
    { "labels-fullscreen-2560x1600", 0,    0,  1, 1, PSD_LABELS_ALL,    1, 100, 0 },
    { "labels-1280x720-nosbar",     1280, 720, 0, 0, PSD_LABELS_NORMAL, 1, 120, 0 },
    { "labels-1280x720-menu",       1280, 720, 0, 1, PSD_LABELS_NORMAL, 1, 100, 1 },
};
#define NSCENES ((int) (sizeof(scenes) / sizeof(scenes[0])))

/* --arena: New Game, the grid, then the gated campaign map. */
static const scene_t arena_scenes[] = {
    { "arena-1280x720",                1280, 720, 0, 0, PSD_LABELS_NORMAL, 1, 100, 0 },
    { "arena-1280x720-pid",            1280, 720, 0, 1, PSD_LABELS_NORMAL, 1, 100, 0 },
    { "arena-fullscreen-2560x1600",    0,    0,   1, 0, PSD_LABELS_NORMAL, 1, 100, 0 },
    { "arena-fullscreen-2560x1600-pid", 0,   0,   1, 1, PSD_LABELS_NORMAL, 1, 100, 0 },
};
#define NARENA_SCENES ((int) (sizeof(arena_scenes) / sizeof(arena_scenes[0])))
static const scene_t gated_scene =
    { "arena-exit-lq_e1m1-gated",      1280, 720, 0, 1, PSD_LABELS_NORMAL, 1, 100, 0 };

static int arena_mode;   /* --arena */
static const scene_t *cur_scenes = scenes;
static int ncur_scenes = NSCENES;
#define SETTLE_FRAMES 6

static const char *verdict_name(psq_label_verdict_t v)
{
    switch (v) {
    case PSQ_LABEL_DRAWN: return "drawn";
    case PSQ_LABEL_DEAD: return "dead";
    case PSQ_LABEL_UNLISTED: return "unlisted";
    case PSQ_LABEL_FADED: return "faded";
    case PSQ_LABEL_CULLED: return "culled";
    case PSQ_LABEL_BEHIND: return "behind";
    case PSQ_LABEL_OCCLUDED: return "occluded";
    case PSQ_LABEL_NO_ROOM: return "no-room";
    case PSQ_LABEL_CROWDED: return "crowded";
    }
    return "?";
}

static const char *gate_name(psq_labels_gate_t g)
{
    switch (g) {
    case PSQ_LABELS_OPEN: return "open";
    case PSQ_LABELS_OFF: return "off";
    case PSQ_LABELS_NO_SERVER: return "no-server";
    case PSQ_LABELS_DEMO: return "demo";
    case PSQ_LABELS_NOT_IN_GAME: return "not-in-game";
    case PSQ_LABELS_INTERMISSION: return "intermission";
    case PSQ_LABELS_MENU: return "menu";
    case PSQ_LABELS_GATED: return "gated";
    }
    return "?";
}

/* The per-frame trace line(s). Returns the trace and its size. */
static const psq_label_trace_t *log_trace(const char *what, int *n, psq_labels_gate_t *gate,
                                          int *frame)
{
    const psq_label_trace_t *t = PSQ_LabelsLastFrame(n, gate, frame);
    int i, r;

    Sys_Printf("psq-render: frame %d [%s] %dx%d vrect %d,%d %dx%d gate %s, %d traced\n",
               *frame, what, vid.width, vid.height, r_refdef.vrect.x, r_refdef.vrect.y,
               r_refdef.vrect.width, r_refdef.vrect.height, gate_name(*gate), *n);
    for (i = 0; i < *n; i++) {
        Sys_Printf("psq-render:   ent %d pid %d %-8s dist %6.1f alpha %.2f anchor (%.1f, %.1f)",
                   t[i].entity, t[i].pid, verdict_name(t[i].verdict), t[i].distance,
                   t[i].alpha, t[i].sx, t[i].sy);
        for (r = 0; r < t[i].layout.rows; r++)
            Sys_Printf(" row%d \"%s\" at %d,%d w %d", r, t[i].layout.text[r],
                       t[i].layout.x[r], t[i].layout.y[r], t[i].layout.width[r]);
        Sys_Printf("\n");
    }
    return t;
}

static const psq_label_trace_t *find_pid(const psq_label_trace_t *t, int n, int pid)
{
    int i;
    for (i = 0; i < n; i++)
        if (t[i].pid == pid)
            return &t[i];
    return NULL;
}

/* ------------------------------------------------------------- pixels */

/* One scene's frames: with labels, without (base), and without labels with
 * the nearest labelled monster hidden. Rows are bottom-up RGB. */
typedef struct
{
    unsigned char *lab, *base, *hid;
    int w, h;
    vrect_t vrect;
    psq_label_trace_t t[PSQ_LABEL_TRACE_MAX];
    int n;
    int hidden_pid;
    float hidden_modelindex;
} frames_t;

static frames_t cur, fade_near;   /* fade_near: the near scene, kept for the far one */

static void frames_free(frames_t *f)
{
    free(f->lab);
    free(f->base);
    free(f->hid);
    memset(f, 0, sizeof(*f));
}

static unsigned char *grab(const char *name, int *w, int *h, int write)
{
    unsigned char *rgb = NULL;
    char file[64];

    if (!psq_offscreen_read(&rgb, w, h)) {
        CHECK(0, "%s: read the framebuffer", name);
        return NULL;
    }
    if (write) {
        q_snprintf(file, sizeof(file), "%s.png", name);
        CHECK(Image_WritePNG(file, rgb, *w, *h, 24, false), "%s: wrote %s/%s", name, com_gamedir, file);
    }
    return rgb;
}

static unsigned char *capture(const char *name, int *w, int *h)
{
    return grab(name, w, h, 1);
}

static const unsigned char *px(const unsigned char *rgb, int w, int h, int x, int y)
{
    return rgb + ((size_t) (h - 1 - y) * w + x) * 3;
}

static int diff3(const unsigned char *a, const unsigned char *b)
{
    return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]);
}

/* Over a label's rows (each with its one-step shadow): the fraction of
 * pixels changed against base, the fraction darker than base, and the sum
 * of absolute differences. */
static void label_pixels(const unsigned char *lab, const unsigned char *base, int w, int h,
                         const psq_label_layout_t *l, float *changed, float *darker, double *sum)
{
    int r, x, y, total = 0, ch = 0, dk = 0;
    double s = 0;

    for (r = 0; r < l->rows; r++) {
        for (y = l->y[r]; y < l->y[r] + (PSQ_LABEL_GLYPH + 1) * l->scale; y++) {
            for (x = l->x[r]; x < l->x[r] + l->width[r] + l->scale; x++) {
                const unsigned char *p, *q;
                int d;
                if (x < 0 || y < 0 || x >= w || y >= h)
                    continue;
                p = px(lab, w, h, x, y);
                q = px(base, w, h, x, y);
                d = diff3(p, q);
                total++;
                s += d;
                if (d > 24)
                    ch++;
                /* The shadow is black at 3/4 opacity: a quarter of the
                 * scene's brightness or less where it shows. */
                if (q[0] + q[1] + q[2] > 120
                    && (p[0] + p[1] + p[2]) * 100 < (q[0] + q[1] + q[2]) * 35)
                    dk++;
            }
        }
    }
    *changed = total ? (float) ch / total : 0;
    *darker = total ? (float) dk / total : 0;
    *sum = s;
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *) a - *(const int *) b;
}

/* Where two frames differ inside the view rectangle: the hidden monster's
 * own pixels, without isolated ones; its extent is taken between the 2nd
 * and 98th percentiles of their x and y, so stray pixels elsewhere cannot
 * stretch it. Returns the count. */
static int silhouette(const unsigned char *a, const unsigned char *b, int w, int h,
                      const vrect_t *v, int *left, int *right, int *top, int *bottom)
{
    int x, y, n = 0, lo, hi;
    int *xs = (int *) malloc(sizeof(int) * (size_t) w * h);
    int *ys = (int *) malloc(sizeof(int) * (size_t) w * h);

    *left = *right = *top = *bottom = -1;
    if (!xs || !ys) {
        free(xs);
        free(ys);
        return 0;
    }
    for (y = v->y; y < v->y + v->height && y < h; y++)
        for (x = v->x; x < v->x + v->width && x < w; x++) {
            int dx, dy, around = 0;
            if (diff3(px(a, w, h, x, y), px(b, w, h, x, y)) <= 24)
                continue;
            /* A model is a solid patch; the sky's sub-pixel drift is
             * scattered single pixels. Keep a pixel only with at least
             * four differing neighbours. */
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++)
                    if ((dx || dy) && x + dx >= 0 && x + dx < w && y + dy >= 0 && y + dy < h
                        && diff3(px(a, w, h, x + dx, y + dy), px(b, w, h, x + dx, y + dy)) > 24)
                        around++;
            if (around >= 4) {
                xs[n] = x;
                ys[n] = y;
                n++;
            }
        }
    if (n > 0) {
        qsort(xs, n, sizeof(int), cmp_int);
        qsort(ys, n, sizeof(int), cmp_int);
        lo = n / 50;
        hi = n - 1 - n / 50;
        *left = xs[lo];
        *right = xs[hi];
        *top = ys[lo];
        *bottom = ys[hi];
    }
    free(xs);
    free(ys);
    return n;
}

static const psq_label_trace_t *nearest_drawn(const psq_label_trace_t *t, int n)
{
    const psq_label_trace_t *best = NULL;
    int i;
    for (i = 0; i < n; i++)
        if (t[i].verdict == PSQ_LABEL_DRAWN && (!best || t[i].distance < best->distance))
            best = &t[i];
    return best;
}

/* The label trace and the labelled frame of a scene. Returns 1 when the
 * scene goes on to its base and hidden-monster frames. */
static int check_scene(const scene_t *s)
{
    psq_labels_gate_t gate;
    int n, frame, i, j, w = 0, h = 0, drawn = 0, faded = 0, crowded = 0, overlaps = 0;
    const psq_label_trace_t *t = log_trace(s->name, &n, &gate, &frame);
    const psq_label_trace_t *hidden, *near;
    unsigned char *rgb;
    int vx = r_refdef.vrect.x, vy = r_refdef.vrect.y;
    int vw = r_refdef.vrect.width, vh = r_refdef.vrect.height;

    CHECK(frame == host_framecount - 1, "%s: the label pass ran on the frame just drawn", s->name);
    if (s->fullscreen)
        CHECK(vid.width == 2560 && vid.height == 1600, "%s: fullscreen at the desktop size (%dx%d)",
              s->name, vid.width, vid.height);
    else
        CHECK(vid.width == s->w && vid.height == s->h, "%s: window %dx%d", s->name, vid.width, vid.height);

    rgb = capture(s->name, &w, &h);

    if (!s->labels) {
        CHECK(gate == PSQ_LABELS_OFF && n == 0, "%s: psq_labels 0 draws nothing", s->name);
        free(rgb);
        return 0;
    }
    if (s->menu) {
        CHECK(key_dest == key_menu && gate == PSQ_LABELS_MENU && n == 0,
              "%s: no label while the menu is open", s->name);
        free(rgb);
        return 0;
    }
    CHECK(gate == PSQ_LABELS_OPEN, "%s: the pass is open in a local game", s->name);

    for (i = 0; i < n; i++) {
        const psq_label_layout_t *l = &t[i].layout;
        int r;

        if (t[i].verdict == PSQ_LABEL_FADED)
            faded++;
        if (t[i].verdict == PSQ_LABEL_CROWDED)
            crowded++;
        for (j = 0; j < i && t[i].verdict == PSQ_LABEL_DRAWN; j++) {
            const psq_label_layout_t *a = &t[i].layout, *b = &t[j].layout;
            int ra, rb;
            if (t[j].verdict != PSQ_LABEL_DRAWN)
                continue;
            for (ra = 0; ra < a->rows; ra++)
                for (rb = 0; rb < b->rows; rb++)
                    if (a->x[ra] < b->x[rb] + b->width[rb] + b->scale
                        && b->x[rb] < a->x[ra] + a->width[ra] + a->scale
                        && a->y[ra] < b->y[rb] + (PSQ_LABEL_GLYPH + 1) * b->scale
                        && b->y[rb] < a->y[ra] + (PSQ_LABEL_GLYPH + 1) * a->scale)
                        overlaps++;
        }
        if (t[i].verdict != PSQ_LABEL_DRAWN)
            continue;
        drawn++;
        /* A label pinned to the view's edge for a monster beside the view
         * would sit over some other monster. */
        CHECK(t[i].sx >= vx && t[i].sx < vx + vw,
              "%s: pid %d's anchor is inside the view horizontally (x %.1f in %d..%d)", s->name,
              t[i].pid, t[i].sx, vx, vx + vw);
        CHECK(l->top >= vy && l->bottom + l->scale <= vy + vh,
              "%s: pid %d label and its shadow inside the view vertically", s->name, t[i].pid);
        CHECK(l->scale == psq_label_text_scale(vh, CLAMP(1.0f, scr_sbarscale.value, (float) vid.width / 320.0f)),
              "%s: pid %d text scale %d", s->name, t[i].pid, l->scale);
        CHECK(t[i].alpha > 0 && t[i].alpha <= 1, "%s: pid %d alpha %.2f", s->name, t[i].pid,
              t[i].alpha);
        for (r = 0; r < l->rows; r++)
            CHECK(l->x[r] >= vx && l->x[r] + l->width[r] + l->scale <= vx + vw,
                  "%s: pid %d row %d and its shadow inside the view horizontally", s->name, t[i].pid, r);
        if (s->showpid) {
            char want[16];
            q_snprintf(want, sizeof(want), "%d", t[i].pid);
            CHECK(l->rows == 2 && !strcmp(l->text[0], want), "%s: pid %d shows its PID on top",
                  s->name, t[i].pid);
        } else {
            CHECK(l->rows == 1, "%s: pid %d shows its name only", s->name, t[i].pid);
        }
        CHECK(label_for(t[i].pid) && !strcmp(l->text[l->rows - 1], label_for(t[i].pid)),
              "%s: pid %d is labelled \"%s\"", s->name, t[i].pid, l->text[l->rows - 1]);
    }

    if (arena_mode) {
        /* One open hall: every process monster is traced, and each is
         * labelled unless it is outside the view or crowded out. */
        int traced = 0, wrong = 0, culled = 0;
        const psq_label_trace_t *edge = find_pid(t, n, EDGE_PID);
        CHECK(edge && edge->verdict == PSQ_LABEL_CULLED,
              "%s: pid %d, its head beside the view, gets no label pinned to the edge (%s)",
              s->name, EDGE_PID, edge ? verdict_name(edge->verdict) : "not traced");
        for (i = 0; i < NFAKES; i++) {
            const psq_label_trace_t *f = find_pid(t, n, fakes[i].pid);
            if (!f)
                continue;
            traced++;
            if (f->verdict == PSQ_LABEL_CULLED) {
                culled++;
                /* Only the edge monster is out of view in the pinned scene. */
                if (fakes[i].pid != EDGE_PID) {
                    wrong++;
                    Sys_Printf("psq-render: %s: pid %d in plain view was culled\n", s->name,
                               fakes[i].pid);
                }
            } else if (f->verdict != PSQ_LABEL_DRAWN && f->verdict != PSQ_LABEL_CROWDED) {
                wrong++;
                Sys_Printf("psq-render: %s: pid %d %s\n", s->name, fakes[i].pid,
                           verdict_name(f->verdict));
            }
        }
        CHECK(traced == NFAKES && n == NFAKES,
              "%s: all %d process monsters on the grid are traced (%d of %d entries)",
              s->name, NFAKES, traced, n);
        CHECK(wrong == 0, "%s: none unlisted, occluded, faded, behind the eye or culled in plain view (%d)", s->name,
              wrong);
        CHECK(drawn >= 4, "%s: at least four of the five in view labelled (%d drawn, %d culled, "
              "%d crowded)", s->name, drawn, culled, crowded);
        CHECK(overlaps == 0, "%s: no two drawn labels (with shadows) overlap", s->name);
        goto keep;
    }

    hidden = find_pid(t, n, HIDDEN_PID);
    CHECK(hidden && hidden->verdict == PSQ_LABEL_OCCLUDED,
          "%s: the monster behind walls gets no label (%s)", s->name,
          hidden ? verdict_name(hidden->verdict) : "not in view");

    if (s->range == PSD_LABELS_NEAR)
        CHECK(faded >= 2 && drawn >= 1, "%s: near range fades the far monsters (%d faded, %d drawn)",
              s->name, faded, drawn);
    else if (s->range == PSD_LABELS_NORMAL)
        CHECK(drawn >= 3, "%s: at least three labels drawn (%d)", s->name, drawn);
    else
        CHECK(drawn >= 3 && faded == 0 && drawn + crowded == NFAKES - 1,
              "%s: far and all ranges fade none of the lineup (%d drawn, %d crowded out, %d faded)",
              s->name, drawn, crowded, faded);
    CHECK(overlaps == 0, "%s: no two drawn labels (with shadows) overlap", s->name);

keep:
    /* Keep the labelled frame; the base and hidden frames follow. */
    frames_free(&cur);
    cur.lab = rgb;
    cur.w = w;
    cur.h = h;
    cur.vrect = r_refdef.vrect;
    cur.n = n;
    memcpy(cur.t, t, sizeof(cur.t[0]) * n);
    near = nearest_drawn(t, n);
    CHECK(near != NULL, "%s: a label to tie to its monster", s->name);
    if (!near)
        return 0;
    cur.hidden_pid = near->pid;
    return 1;
}

/* With the base and hidden frames in hand: label pixels are drawn where the
 * trace says, and the nearest label sits just above its monster's own
 * pixels (found by hiding the monster, not by projection). */
static void check_pixels(const scene_t *s)
{
    int i, left, right, top, bottom, count;
    const psq_label_trace_t *near = find_pid(cur.t, cur.n, cur.hidden_pid);

    if (!cur.lab || !cur.base || !cur.hid) {
        CHECK(0, "%s: all three frames read", s->name);
        return;
    }
    for (i = 0; i < cur.n; i++) {
        const psq_label_trace_t *t = &cur.t[i];
        float changed, darker;
        double sum;
        if (t->verdict != PSQ_LABEL_DRAWN)
            continue;
        label_pixels(cur.lab, cur.base, cur.w, cur.h, &t->layout, &changed, &darker, &sum);
        CHECK(changed > 0.05f, "%s: pid %d label pixels drawn where the trace says (%.0f%% changed)",
              s->name, t->pid, changed * 100);
        (void) darker;
    }
    count = silhouette(cur.base, cur.hid, cur.w, cur.h, &cur.vrect, &left, &right, &top, &bottom);
    Sys_Printf("psq-render: %s: pid %d's monster covers x %d..%d y %d..%d (%d pixels)\n",
               s->name, cur.hidden_pid, left, right, top, bottom, count);
    CHECK(count >= 30, "%s: hiding pid %d's monster changes the frame (%d pixels)", s->name,
          cur.hidden_pid, count);
    if (near && count >= 30) {
        const psq_label_layout_t *l = &near->layout;
        int name_mid = l->x[l->rows - 1] + l->width[l->rows - 1] / 2;
        int gap = top - l->bottom;
        int max_gap = 24 * l->scale > cur.vrect.height / 10 ? 24 * l->scale : cur.vrect.height / 10;
        CHECK(name_mid >= left && name_mid <= right,
              "%s: pid %d's name is centred over its monster (centre %d, monster %d..%d)",
              s->name, near->pid, name_mid, left, right);
        CHECK(gap >= 0 && gap <= max_gap,
              "%s: pid %d's label sits just above its monster (gap %d px, at most %d)",
              s->name, near->pid, gap, max_gap);
    }

    /* The fade: the near scene's faded labels against this far scene's
     * opaque ones, at the same place. Brightness change scales with alpha. */
    if (s->range == PSD_LABELS_NEAR) {
        frames_free(&fade_near);
        fade_near = cur;
        memset(&cur, 0, sizeof(cur));
    } else if (s->range == PSD_LABELS_FAR && fade_near.lab && fade_near.w == cur.w
               && fade_near.h == cur.h) {
        int checked = 0;
        for (i = 0; i < fade_near.n; i++) {
            const psq_label_trace_t *a = &fade_near.t[i];
            const psq_label_trace_t *b = find_pid(cur.t, cur.n, a->pid);
            float c1, d1, c2, d2;
            double s1, s2, ratio;
            if (a->verdict != PSQ_LABEL_DRAWN || !b || b->verdict != PSQ_LABEL_DRAWN
                || a->alpha > 0.9f || a->alpha < 0.05f
                || memcmp(&a->layout, &b->layout, sizeof(a->layout)) != 0)
                continue;
            label_pixels(fade_near.lab, fade_near.base, cur.w, cur.h, &a->layout, &c1, &d1, &s1);
            label_pixels(cur.lab, cur.base, cur.w, cur.h, &b->layout, &c2, &d2, &s2);
            ratio = s2 > 0 ? s1 / s2 : 0;
            CHECK(fabs(ratio - a->alpha) < 0.15,
                  "%s: pid %d at alpha %.2f changes the scene %.2f as much as when opaque",
                  s->name, a->pid, a->alpha, ratio);
            checked++;
        }
        CHECK(checked >= 1, "%s: at least one part-faded label compared (%d)", s->name, checked);
    }
}

/* ------------------------------------------------------------ the ticker */

enum { ST_WAIT_GAME, ST_SETUP, ST_PLACED, ST_SCENE_APPLY, ST_SCENE_SETTLE, ST_SCENE_BASE,
       ST_SCENE_HIDDEN, ST_INTERMISSION,
       ST_INTER_CHECK, ST_INTER_CONSOLE, ST_DEMO, ST_DEMO_CHECK,
       ST_A_SETUP, ST_A_LEAVE, ST_A_CAMPAIGN, ST_A_GATED_WAIT, ST_A_GATED_CHECK,
       ST_A_GATED_BASE, ST_A_GATED_HIDDEN, ST_GATE_OFF, ST_GATE_OFF_CHECK, ST_GATE_ON_CHECK,
       ST_DONE };

static int    state = ST_WAIT_GAME;
static int    scene;
static int    settle;
static double wait_start;
static double placed_at;

static void finish(void)
{
    /* No NSApplication: SDL's Cocoa video never started, so nothing could
     * have reached the screen. */
    void **nsapp = (void **) dlsym(RTLD_DEFAULT, "NSApp");
    CHECK(nsapp == NULL || *nsapp == NULL, "no NSApplication was ever created");
    CHECK(SDL_WasInit(SDL_INIT_VIDEO) == 0, "SDL's own video subsystem was never started");
    Sys_Printf("psq-render: %d checks, %d failed\n", checks_run, checks_failed);
    if (checks_failed)
        Sys_Error("psq-render: %d of %d checks failed", checks_failed, checks_run);
    Sys_Printf("psq-render: ALL PASS\n");
    key_dest = key_console;   /* quit without the in-game confirmation */
    Cbuf_AddText("quit\n");
    state = ST_DONE;
}

static void apply_scene(const scene_t *s)
{
    char cmd[256];

    if (s->fullscreen)
        q_snprintf(cmd, sizeof(cmd), "vid_fullscreen 1\nvid_restart\n");
    else
        q_snprintf(cmd, sizeof(cmd), "vid_fullscreen 0\nvid_width %d\nvid_height %d\nvid_restart\n",
                   s->w, s->h);
    Cbuf_AddText(cmd);
    Cvar_SetValue("psq_labels", (float) s->labels);
    Cvar_SetValue("psq_showpid", (float) s->showpid);
    Cvar_SetValue("psq_labelrange", (float) s->range);
    Cvar_SetValue("viewsize", (float) s->viewsize);
    if (s->menu && key_dest != key_menu)
        M_Menu_Main_f();
    if (!s->menu && key_dest == key_menu)
        M_ToggleMenu_f();
}

/* --------------------------------------------------------------- --arena */

static float field_of(edict_t *e, const char *name)
{
    eval_t *v = GetEdictFieldValue(e, name);
    return v ? v->_float : 0;
}

static int count_class(const char *classname, edict_t **found)
{
    int i, n = 0;
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        if (!e->free && !strcmp(PR_GetString(e->v.classname), classname)) {
            if (found)
                *found = e;
            n++;
        }
    }
    return n;
}

static int tagged_edicts(void)
{
    int i, n = 0;
    for (i = 1; i < sv.num_edicts; i++)
        if (!EDICT_NUM(i)->free && pid_of(EDICT_NUM(i)) != 0)
            n++;
    return n;
}

/* The grid of the level's one psq_arena entity; 0 if there is not exactly one. */
static int arena_grid(psq_grid *g)
{
    edict_t *c = NULL;
    memset(g, 0, sizeof(*g));
    if (count_class("psq_arena", &c) != 1)
        return 0;
    return psq_grid_from_keys(g, c->v.origin, field_of(c, "psq_cols"),
                              field_of(c, "psq_rows"), field_of(c, "psq_cell"));
}

/* Each process monster stands on a grid cell of its own: its psq_gridcell is
 * a cell no other has, its x and y are that cell's centre and its box stays
 * inside the cell. */
static void arena_check_grid(void)
{
    psq_grid g;
    int i, j, cells[NFAKES];

    CHECK(arena_grid(&g) && psq_grid_cells(&g) > 0, "the arena has one psq_arena grid (%d x %d "
          "cells of %g units)", g.cols, g.rows, g.cell);
    for (i = 0; i < NFAKES; i++) {
        edict_t *m = monster_for(fakes[i].pid);
        float cell, c[3] = { 0, 0, 0 };
        int idx, good;

        cells[i] = -1;
        if (!m) {
            CHECK(0, "pid %d has a monster in the arena", fakes[i].pid);
            continue;
        }
        cell = field_of(m, "psq_gridcell");
        idx = (int) cell;
        good = cell >= 0 && cell == (float) idx && psq_grid_centre(&g, idx, c)
            && fabsf(m->v.origin[0] - c[0]) < 0.5f && fabsf(m->v.origin[1] - c[1]) < 0.5f
            && m->v.origin[0] + m->v.mins[0] >= c[0] - g.cell / 2
            && m->v.origin[0] + m->v.maxs[0] <= c[0] + g.cell / 2
            && m->v.origin[1] + m->v.mins[1] >= c[1] - g.cell / 2
            && m->v.origin[1] + m->v.maxs[1] <= c[1] + g.cell / 2
            && hull_fits(m, m->v.origin);
        for (j = 0; j < i; j++)
            if (cells[j] == idx)
                good = 0;
        cells[i] = idx;
        CHECK(good, "pid %d (%s) stands alone on grid cell %d (col %d, row %d) at (%.0f %.0f %.0f)",
              fakes[i].pid, PR_GetString(m->v.classname), idx, g.cols ? idx % g.cols : -1,
              g.cols ? idx / g.cols : -1, m->v.origin[0], m->v.origin[1], m->v.origin[2]);
    }
}

/* The controller's cells are random. Once they are checked, each monster
 * moves to a fixed cell of its own so every run frames the same scene: five
 * in plain view, and pid 3101 in the near row's east end (col 9, row 0),
 * whose box reaches into the view from the right while its head is beside
 * it. */
/* From the camera: 3102 at -40 degrees, 3105 at -13, 3104 ahead, 3103 at
 * +15, 3106 at +33, none behind another; 3101 at +57, beside the view. */
static const int pinned_cell[NFAKES] = { 9, 13, 62, 38, 69, 30 };

static void arena_pin(void)
{
    psq_grid g;
    int i;

    if (!arena_grid(&g))
        return;
    for (i = 0; i < NFAKES; i++) {
        edict_t *m = monster_for(fakes[i].pid);
        eval_t *cell;
        float c[3];
        vec3_t p;

        if (!m || !psq_grid_centre(&g, pinned_cell[i], c))
            continue;
        p[0] = c[0];
        p[1] = c[1];
        p[2] = m->v.origin[2];
        /* Out of the way first, so no two boxes meet mid-move. */
        m->v.origin[2] += 4096;
        SV_LinkEdict(m, false);
        place(m, p, 270);
        cell = GetEdictFieldValue(m, "psq_gridcell");
        if (cell)
            cell->_float = (float) pinned_cell[i];
    }
    for (i = 0; i < NFAKES; i++) {
        edict_t *m = monster_for(fakes[i].pid);
        CHECK(m && hull_fits(m, m->v.origin), "pid %d moved to cell %d (%.0f %.0f %.0f), clear",
              fakes[i].pid, pinned_cell[i], m ? m->v.origin[0] : 0, m ? m->v.origin[1] : 0,
              m ? m->v.origin[2] : 0);
    }
}

/* Stand the player in the hall's south margin, 200 units short of the grid,
 * looking north over it and a little down. */
static void arena_camera(void)
{
    psq_grid g;
    vec3_t p, down;
    trace_t tr;

    if (!arena_grid(&g))
        return;
    p[0] = g.origin[0];
    p[1] = g.origin[1] - g.rows * g.cell / 2 - 200;
    p[2] = g.origin[2] + 64;
    VectorCopy(p, down);
    down[2] -= 256;
    tr = SV_Move(p, PLAYER->v.mins, PLAYER->v.maxs, down, MOVE_NOMONSTERS, PLAYER);
    CHECK(!tr.startsolid && !tr.allsolid && tr.fraction < 1.0f,
          "the camera stands on the hall floor south of the grid");
    VectorCopy(tr.endpos, PLAYER->v.origin);
    VectorCopy(vec3_origin, PLAYER->v.velocity);
    SV_LinkEdict(PLAYER, false);
    view_yaw = atan2f(g.origin[1] - p[1], g.origin[0] - p[0]) * 180.0f / (float) M_PI;
    VectorAdd(PLAYER->v.origin, PLAYER->v.view_ofs, eye);
    PLAYER->v.angles[0] = 8;   /* view pitch: down */
    PLAYER->v.angles[1] = view_yaw;
    PLAYER->v.angles[2] = 0;
    VectorCopy(PLAYER->v.angles, PLAYER->v.v_angle);
    PLAYER->v.fixangle = 1;
    Sys_Printf("psq-render: arena eye (%.0f %.0f %.0f), yaw %.0f, grid at (%.0f %.0f %.0f)\n",
               eye[0], eye[1], eye[2], view_yaw, g.origin[0], g.origin[1], g.origin[2]);
}

/* What Single Player > New Game runs (menu.c), then the ticker. */
static void PSQ_Render_Arena_f(void)
{
    const char *m = psq_new_game_map(COM_FileExists(PSQ_ARENA_BSP, NULL));

    arena_mode = 1;
    cur_scenes = arena_scenes;
    ncur_scenes = NARENA_SCENES;
    CHECK(!strcmp(m, PSQ_ARENA_MAP), "New Game opens %s", m);
    CHECK(Cvar_VariableValue("psq_testarena") == 0, "psq_testarena is off");
    key_dest = key_game;
    Cbuf_AddText(va("maxplayers 1\ndeathmatch 0\ncoop 0\nmap %s\npsq_render_tick\n", m));
}

static void PSQ_Render_Tick_f(void)
{
    int n, frame;
    psq_labels_gate_t gate;

    if (state == ST_DONE)
        return;
    if (realtime - wait_start > 60)
        Sys_Error("psq-render: timed out in state %d", state);

    switch (state) {
    case ST_WAIT_GAME:
        if (sv.active && svs.clients[0].active && PLAYER) {
            /* The level's own monsters stand still and the player cannot
             * die: only the process monsters matter here. */
            int i;
            PLAYER->v.flags = (float) ((int) PLAYER->v.flags | FL_GODMODE | FL_NOTARGET);
            for (i = 1; i < sv.num_edicts; i++) {
                edict_t *e = EDICT_NUM(i);
                if (!e->free && ((int) e->v.flags & FL_MONSTER) && pid_of(e) == 0)
                    e->v.nextthink = sv.time + 100000;
            }
        }
        if (sv.active && cls.signon == SIGNONS && live_process_monsters() == NFAKES
            && sv.time > (arena_mode ? 4 : 3)) {
            Sys_Printf("psq-render: %d process monsters at %.1f s on %s\n", NFAKES, sv.time,
                       sv.name);
            state = arena_mode ? ST_A_SETUP : ST_SETUP;
        }
        break;
    case ST_A_SETUP:
        Cvar_SetValue("psq_debug", 0);
        Cvar_SetValue("developer", 0);
        Con_ClearNotify();
        CHECK(psq_arena_is_map(sv.name), "New Game opened the arena (%s)", sv.name);
        CHECK(psq_policy_level_active(), "the arena's controller polled: the gate is open");
        arena_check_grid();
        arena_pin();
        arena_camera();
        placed_at = realtime;
        state = ST_PLACED;
        break;
    case ST_SETUP:
        /* Quiet from here: no debug lines in the notify area of the frames. */
        Cvar_SetValue("psq_debug", 0);
        Cvar_SetValue("developer", 0);
        Con_ClearNotify();
        Cbuf_AddText("record psqlabels\n");
        setup_camera();
        setup_lineup();
        setup_hidden();
        placed_at = realtime;
        state = ST_PLACED;
        break;
    case ST_PLACED:
        /* Let the client's 0.1 s move interpolation carry the models to
         * where they now stand, then stop the clock so frames compare
         * pixel for pixel (sky and lights hold still). */
        if (realtime - placed_at < 2.0)
            break;
        /* The demo is these two seconds of real time with the lineup in
         * view; it is played back at the end. */
        if (!arena_mode)
            Cbuf_AddText("stop\n");
        Cvar_SetValue("host_framerate", 0.00001f);
        state = ST_SCENE_APPLY;
        break;
    case ST_SCENE_APPLY:
        apply_scene(&cur_scenes[scene]);
        settle = 0;
        state = ST_SCENE_SETTLE;
        break;
    case ST_SCENE_SETTLE:
        if (++settle < SETTLE_FRAMES) {
            log_trace(cur_scenes[scene].name, &n, &gate, &frame);
            break;
        }
        if (check_scene(&cur_scenes[scene])) {
            Cvar_SetValue("psq_labels", 0);
            settle = 0;
            state = ST_SCENE_BASE;
            break;
        }
    next_scene:
        if (++scene < ncur_scenes)
            state = ST_SCENE_APPLY;
        else {
            Cvar_SetValue("psq_labels", 1);
            Cvar_SetValue("viewsize", 100);
            if (key_dest == key_menu)
                M_ToggleMenu_f();
            settle = 0;
            state = arena_mode ? ST_A_LEAVE : ST_GATE_OFF;
        }
        break;
    case ST_GATE_OFF:
        /* psq_testarena off mid-level, with the clock all but stopped: no
         * builtin runs, the pool still lists the six, and the gate must
         * shut for the labels at once. */
        if (++settle < 3)
            break;
        Cvar_SetValue("psq_testarena", 0);
        settle = 0;
        state = ST_GATE_OFF_CHECK;
        break;
    case ST_GATE_OFF_CHECK:
        if (++settle < SETTLE_FRAMES)
            break;
        {
            int w, h;
            unsigned char *rgb;
            log_trace("gate-shut", &n, &gate, &frame);
            CHECK(frame == host_framecount - 1 && gate == PSQ_LABELS_GATED && n == 0,
                  "psq_testarena 0 mid-level, no poll since: the label pass is gated at once "
                  "(gate %s, %d traced)", gate_name(gate), n);
            rgb = capture("labels-1280x720-gate-shut", &w, &h);
            free(rgb);
            Cvar_SetValue("psq_testarena", 1);
            settle = 0;
            state = ST_GATE_ON_CHECK;
        }
        break;
    case ST_GATE_ON_CHECK:
        if (++settle < SETTLE_FRAMES)
            break;
        {
            const psq_label_trace_t *t = log_trace("gate-open-again", &n, &gate, &frame);
            int i, drawn = 0;
            for (i = 0; i < n; i++)
                if (t[i].verdict == PSQ_LABEL_DRAWN)
                    drawn++;
            CHECK(gate == PSQ_LABELS_OPEN && drawn >= 3,
                  "psq_testarena 1 again: the labels return (gate %s, %d drawn)", gate_name(gate),
                  drawn);
            state = ST_INTERMISSION;
        }
        break;
    case ST_SCENE_BASE:
        if (++settle < 3)
            break;
        {
            int w, h;
            edict_t *m = monster_for(cur.hidden_pid);
            cur.base = grab(cur_scenes[scene].name, &w, &h, 0);
            /* Not sent to the client while its model index is 0. */
            if (m) {
                cur.hidden_modelindex = m->v.modelindex;
                m->v.modelindex = 0;
            }
            settle = 0;
            state = ST_SCENE_HIDDEN;
        }
        break;
    case ST_SCENE_HIDDEN:
        if (++settle < 3)
            break;
        {
            int w, h;
            edict_t *m = monster_for(cur.hidden_pid);
            cur.hid = grab(cur_scenes[scene].name, &w, &h, 0);
            if (m)
                m->v.modelindex = cur.hidden_modelindex;
            Cvar_SetValue("psq_labels", 1);
            check_pixels(&cur_scenes[scene]);
        }
        goto next_scene;
    case ST_INTERMISSION:
        /* The server's own intermission message, as a level exit sends. */
        MSG_WriteByte(&sv.reliable_datagram, svc_intermission);
        settle = 0;
        state = ST_INTER_CHECK;
        break;
    case ST_INTER_CHECK:
        if (++settle < SETTLE_FRAMES)
            break;
        {
            int w, h;
            unsigned char *rgb;
            PSQ_LabelsLastFrame(&n, &gate, &frame);
            CHECK(cl.intermission == 1, "a real intermission is showing");
            CHECK(frame < host_framecount - 1,
                  "intermission screen: the label pass does not run (last ran frame %d, now %d)",
                  frame, host_framecount);
            rgb = capture("labels-intermission", &w, &h);
            free(rgb);
            key_dest = key_console;   /* the console over an intermission */
            settle = 0;
            state = ST_INTER_CONSOLE;
        }
        break;
    case ST_INTER_CONSOLE:
        if (++settle < 3)
            break;
        log_trace("intermission+console", &n, &gate, &frame);
        CHECK(frame == host_framecount - 1 && gate == PSQ_LABELS_INTERMISSION && n == 0,
              "intermission with the console down: gated, nothing drawn");
        key_dest = key_game;
        Cvar_SetValue("host_framerate", 0);
        Cbuf_AddText("playdemo psqlabels\n");
        Sys_Printf("psq-render: demo playback starts at %.1f s real time\n", realtime);
        settle = 0;
        state = ST_DEMO;
        break;
    case ST_DEMO:
        if (!cls.demoplayback || cls.signon != SIGNONS || scr_con_current > 0 || ++settle < 5)
            break;
        state = ST_DEMO_CHECK;
        /* fall through */
    case ST_DEMO_CHECK:
        {
            int w, h, ents = 0, i;
            unsigned char *rgb;
            log_trace("demo", &n, &gate, &frame);
            for (i = 0; i < cl_numvisedicts; i++)
                if (cl_visedicts[i]->model && cl_visedicts[i]->model->type == mod_alias)
                    ents++;
            CHECK(!sv.active && cls.demoplayback, "a recorded demo is playing, no server");
            CHECK(ents >= 3, "the demo shows the monsters (%d alias models in view)", ents);
            CHECK(gate == PSQ_LABELS_DEMO && n == 0, "demo playback: gated, no label drawn");
            rgb = capture("labels-demo", &w, &h);
            free(rgb);
            finish();
        }
        return;
    case ST_A_LEAVE:
        /* The arena's exit: the campaign's first map, psq_testarena off. */
        Cvar_SetValue("host_framerate", 0);
        Cbuf_AddText("changelevel lq_e1m1\n");
        state = ST_A_CAMPAIGN;
        break;
    case ST_A_CAMPAIGN:
        if (!sv.active || cls.signon != SIGNONS || strcmp(sv.name, "lq_e1m1") || !PLAYER
            || sv.time < 2)
            break;
        {
            int i;
            edict_t *m = NULL;
            vec3_t fwd, right, p;

            PLAYER->v.flags = (float) ((int) PLAYER->v.flags | FL_GODMODE | FL_NOTARGET);
            for (i = 1; i < sv.num_edicts; i++) {
                edict_t *e = EDICT_NUM(i);
                if (!e->free && ((int) e->v.flags & FL_MONSTER)) {
                    e->v.nextthink = sv.time + 100000;
                    if (!m && e->v.health > 0 && e->v.solid == SOLID_SLIDEBOX)
                        m = e;
                }
            }
            CHECK(live_process_monsters() == 0 && tagged_edicts() == 0
                  && count_class("psq_arena", NULL) == 0,
                  "lq_e1m1 after the arena: no process monster, tag or controller");
            CHECK(!psq_policy_level_active() && psq_policy_entry(0) == NULL,
                  "lq_e1m1: no poll ran, the policy lists no process");
            setup_camera();
            /* A level monster with a listed pid's tag, as a crafted map or
             * save could give it, in plain view. */
            CHECK(m != NULL, "lq_e1m1 has a monster to tag");
            if (!m) {
                finish();
                return;
            }
            angle_dir(view_yaw, fwd, right);
            VectorMA(PLAYER->v.origin, 300, fwd, p);
            if (!drop_to_floor(m, p) || !hull_fits(m, p))
                Sys_Printf("psq-render: no floor 300 units ahead for the tagged monster\n");
            place(m, p, view_yaw + 180);
            GetEdictFieldValue(m, "psq_pid")->_float = (float) fakes[0].pid;
            Sys_Printf("psq-render: %s tagged pid %d at (%.0f %.0f %.0f)\n",
                       PR_GetString(m->v.classname), fakes[0].pid, p[0], p[1], p[2]);
            placed_at = realtime;
            state = ST_A_GATED_WAIT;
        }
        break;
    case ST_A_GATED_WAIT:
        if (realtime - placed_at < 1.5)
            break;
        Cvar_SetValue("host_framerate", 0.00001f);
        apply_scene(&gated_scene);
        settle = 0;
        state = ST_A_GATED_CHECK;
        break;
    case ST_A_GATED_CHECK:
        if (++settle < SETTLE_FRAMES)
            break;
        {
            int w, h;

            log_trace(gated_scene.name, &n, &gate, &frame);
            /* Gated by the level, not merely an empty pool: the pass says
             * why it drew nothing. */
            CHECK(frame == host_framecount - 1 && gate == PSQ_LABELS_GATED && n == 0,
                  "%s: the label pass is gated here, the tagged monster gets no label "
                  "(gate %s, %d traced)", gated_scene.name, gate_name(gate), n);
            frames_free(&cur);
            cur.lab = capture(gated_scene.name, &w, &h);
            cur.w = w;
            cur.h = h;
            cur.vrect = r_refdef.vrect;
            Cvar_SetValue("psq_labels", 0);
            settle = 0;
            state = ST_A_GATED_BASE;
        }
        break;
    case ST_A_GATED_BASE:
        if (++settle < 3)
            break;
        {
            int w, h, l, r, tp, b, count;
            edict_t *m = monster_for(fakes[0].pid);
            cur.base = grab(gated_scene.name, &w, &h, 0);
            count = cur.lab && cur.base
                ? silhouette(cur.lab, cur.base, w, h, &cur.vrect, &l, &r, &tp, &b) : -1;
            CHECK(count >= 0 && count < 30,
                  "%s: psq_labels 1 and 0 draw the same frame (%d label pixels)",
                  gated_scene.name, count);
            if (m) {
                cur.hidden_modelindex = m->v.modelindex;
                m->v.modelindex = 0;
            }
            settle = 0;
            state = ST_A_GATED_HIDDEN;
        }
        break;
    case ST_A_GATED_HIDDEN:
        if (++settle < 3)
            break;
        {
            int w, h, l, r, tp, b, count;
            edict_t *m = monster_for(fakes[0].pid);
            cur.hid = grab(gated_scene.name, &w, &h, 0);
            count = cur.base && cur.hid
                ? silhouette(cur.base, cur.hid, w, h, &cur.vrect, &l, &r, &tp, &b) : 0;
            CHECK(count >= 30, "%s: the tagged monster is in plain view (%d pixels, x %d..%d "
                  "y %d..%d)", gated_scene.name, count, l, r, tp, b);
            if (m)
                m->v.modelindex = cur.hidden_modelindex;
            Cvar_SetValue("psq_labels", 1);
            frames_free(&cur);
            finish();
        }
        return;
    }
    Cbuf_AddText("wait\npsq_render_tick\n");
}

void psq_test_register(void)
{
    Cmd_AddCommand("psq_render_tick", PSQ_Render_Tick_f);
    Cmd_AddCommand("psq_render_arena", PSQ_Render_Arena_f);
}

int main(int argc, char *argv[])
{
    static char *def_argv[] = {
        "psq_labels_render", "-psquake-root", PSQ_TEST_ROOT,
        "-nosound", "-nocdaudio", "-nojoy", "-width", "640", "-height", "480", "-window",
        "+developer", "1", "+psq_debug", "1", "+psq_testarena", "1", "+psq_killpolicy", "0",
        "+map", "lq_e1m1", "+psq_render_tick", NULL };
    /* --arena: New Game, psq_testarena off. The +commands come before the
     * root: the engine keeps only the first 256 characters of them. */
    static char *arena_argv[] = {
        "psq_labels_render",
        "-nosound", "-nocdaudio", "-nojoy", "-width", "640", "-height", "480", "-window",
        "+developer", "1", "+psq_debug", "1", "+psq_killpolicy", "0", "+psq_render_arena",
        "-psquake-root", PSQ_TEST_ROOT "-arena", NULL };
    struct stat st;
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAAccelerated, kCGLPFAAllowOfflineRenderers,
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute) kCGLOGLPVersion_Legacy,
        (CGLPixelFormatAttribute) 0 };
    CGLPixelFormatObj pf = NULL;
    GLint npf = 0;

    if (stat(PSQ_TEST_RESOURCES "/id1/pak0.pak", &st) != 0) {
        printf("psq-render: SKIP: no LibreQuake data (run tools/fetch-librequake.sh)\n");
        return 77;
    }
    if (CGLChoosePixelFormat(attrs, &pf, &npf) != kCGLNoError || !pf) {
        printf("psq-render: SKIP: no offscreen OpenGL on this host\n");
        return 77;
    }
    CGLReleasePixelFormat(pf);
    if (argc == 2 && !strcmp(argv[1], "--arena")) {
        argc = (int) (sizeof(arena_argv) / sizeof(arena_argv[0])) - 1;
        argv = arena_argv;
    } else if (argc <= 1) {
        argc = (int) (sizeof(def_argv) / sizeof(def_argv[0])) - 1;
        argv = def_argv;
    }
    wait_start = 0;
    return psq_engine_main(argc, argv);
}
