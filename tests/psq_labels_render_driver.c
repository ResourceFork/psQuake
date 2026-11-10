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
 *             (EDGE_PID) just off the right edge
 *   scenes    the player stands at the hall's south end looking over the
 *             grid; at 1280x720 and 2560x1600 fullscreen, names and PIDs,
 *             the five in view are labelled with their own names, the edge
 *             one is not (at 1280x720 its box reaches into the view and the
 *             anchor rule drops it, verdict beside; at 16:10 the frustum
 *             culls it), nothing is unlisted, no labels overlap, and label
 *             pixels sit where the trace puts them
 *   exit      the console's changelevel lq_e1m1 (the exit trigger is the
 *             headless arena tests'): no process monster or controller
 *             there, and a level monster given a listed pid's tag (as a
 *             crafted map or save could) gets no label: the pass is gated
 *
 * With --arena-shots <arena> it looks at one of the arenas instead (below,
 * shots_tick): frames from the script's camera stands, then two chases.
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
    case PSQ_LABEL_BESIDE: return "beside";
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
        /* At 16:9 the edge monster's box reaches into the view and the
         * anchor rule drops its label; the narrower 16:10 fullscreen view
         * leaves its box outside, and the frustum cull drops it first. */
        psq_label_verdict_t edge_want = s->fullscreen ? PSQ_LABEL_CULLED : PSQ_LABEL_BESIDE;
        CHECK(edge && edge->verdict == edge_want,
              "%s: pid %d, its head beside the view, gets no label pinned to the edge (%s, "
              "want %s)", s->name, EDGE_PID, edge ? verdict_name(edge->verdict) : "not traced",
              verdict_name(edge_want));
        for (i = 0; i < NFAKES; i++) {
            const psq_label_trace_t *f = find_pid(t, n, fakes[i].pid);
            if (!f)
                continue;
            traced++;
            if (f->verdict == PSQ_LABEL_CULLED || f->verdict == PSQ_LABEL_BESIDE) {
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
       ST_S_SETUP, ST_S_APPLY, ST_S_MOVE, ST_S_SETTLE, ST_S_CHASE_START, ST_S_CHASE,
       ST_S_SUPPLY,
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
 * whose box reaches into the 16:9 view from the right while its head is
 * beside it (the 16:10 view is narrower and leaves the whole box out). */
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
    /* The scene is pinned on the test arena, the original one-grid hall,
     * loaded by name as `map psqtesthall` would (New Game opens the Pit,
     * which --arena-shots psqarena frames). */
    key_dest = key_game;
    Cbuf_AddText("maxplayers 1\ndeathmatch 0\ncoop 0\nmap " PSQ_TEST_ARENA_MAP "\npsq_render_tick\n");
}

/* --------------------------------------------------------- --arena-shots */

/* --arena-shots <arena>: a look at one arena (psqarena, psqterraces,
 * psqtowers or psqtesthall), loaded by name as --arena loads the test hall;
 * once the six process monsters are in, each one's class, floor
 * and grid cell is logged (they arrive on whichever grids the controller
 * picks). Then they move to the script's pins, and for each "shot" the
 * player stands there and a 1280x720 frame with names and PIDs is written
 * (shot-NAME.png) beside its label trace. Last, a chase from each of the
 * first two stands: the monsters are re-pinned and woken hunting the
 * god-mode player for CHASE_SECONDS of game time, a frame is written at
 * CHASE_FRAME_AT (chase-NAME.png), and each monster's run is logged: its
 * closest approach, where it ended, and whether it stopped short (moved
 * under STUCK_MOVE units in the last STUCK_SECONDS while more than
 * STUCK_DIST from the player). The script is shots.txt in the user root's
 * game dir (generate_arena.py --shots):
 *
 *   shot NAME X Y Z PITCH YAW     a camera stand on the floor at height Z
 *   view NAME X Y Z PITCH YAW     a camera in the air (the player noclips)
 *   pin X Y Z                     where the next monster stands for shots
 *
 * It checks the arrivals (each monster alone on a cell centre of one of the
 * grids) and that the script and frames are good; the frames and the chase
 * log are for a person to judge. Stand names are [a-z0-9_] only (they name
 * the frames). Its user root is build/headless/root-render-shots-<arena>. */

#define MAX_SHOTS 8
#define CHASE_SECONDS 24.0
#define CHASE_FRAME_AT 5.0
#define CHASE_STEP 0.05f
#define STUCK_SECONDS 6.0
#define STUCK_MOVE 48.0f
#define STUCK_DIST 192.0f

typedef struct { char name[32]; vec3_t p; float pitch, yaw; int air, chase; } shot_t;

static int shots_mode;   /* --arena-shots */
static char shots_map[32];   /* the arena it frames */
static shot_t shots[MAX_SHOTS];
static int nshots, npins;
static vec3_t pins[NFAKES];
static int shot_i, chase_i;
static double chase_t;
static int chase_frame_done;
static float chase_min[NFAKES];
static vec3_t chase_mark[NFAKES];   /* position STUCK_SECONDS ago */
static double chase_mark_t;
static vec3_t chase_stand;   /* the player is held here (no knock-back out of the hall) */
static vec3_t chase_last[NFAKES];

static const scene_t shot_scene =
    { "shot", 1280, 720, 0, 1, PSD_LABELS_NORMAL, 1, 100, 0 };

static void shots_read(void)
{
    FILE *f = fopen(va("%s/shots.txt", com_gamedir), "r");
    char line[256];

    CHECK(f != NULL, "read %s/shots.txt", com_gamedir);
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        shot_t s;
        float x, y, z;
        memset(&s, 0, sizeof(s));
        if (!strncmp(line, "shot ", 5) || !strncmp(line, "view ", 5)) {
            s.air = line[0] == 'v';
            int good = nshots < MAX_SHOTS
                && sscanf(line + 5, "%31s %f %f %f %f %f", s.name, &s.p[0], &s.p[1], &s.p[2],
                          &s.pitch, &s.yaw) == 6;
            const char *c;
            /* The name names a frame file: plain characters only. */
            for (c = s.name; good && *c; c++)
                good = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_';
            CHECK(good, "shots.txt: a good stand (%.40s)", line);
            if (good)
                shots[nshots++] = s;
        } else if (npins < NFAKES && sscanf(line, "pin %f %f %f", &x, &y, &z) == 3) {
            pins[npins][0] = x;
            pins[npins][1] = y;
            pins[npins][2] = z;
            npins++;
        }
    }
    fclose(f);
    CHECK(nshots >= 2 && npins == NFAKES && !shots[0].air && !shots[1].air,
          "the script has %d shots, the first two on the floor (the chases), and %d pins",
          nshots, npins);
    /* Chased from: the first two stands, and the start room's ("room"). */
    shots[0].chase = shots[1].chase = 1;
    {
        int k;
        for (k = 2; k < nshots; k++)
            shots[k].chase = !shots[k].air && !strcmp(shots[k].name, "room");
    }
}

/* The floor centre of cell number idx, counted across the arena's grids in
 * QuakeC's order (psq.qc psq_gridspot): the psq_arena controller's own, then
 * each psq_grid entity in edict order. 0 when there is no such cell. */
static int shots_cell_centre(int idx, float c[3])
{
    int i, pass;

    for (pass = 0; pass < 2; pass++) {
        for (i = 1; i < sv.num_edicts; i++) {
            edict_t *e = EDICT_NUM(i);
            psq_grid g;
            int n;
            if (e->free || strcmp(PR_GetString(e->v.classname), pass ? "psq_grid" : "psq_arena"))
                continue;
            if (!psq_grid_from_keys(&g, e->v.origin, field_of(e, "psq_cols"),
                                    field_of(e, "psq_rows"), field_of(e, "psq_cell")))
                continue;
            n = psq_grid_cells(&g);
            if (idx < n)
                return psq_grid_centre(&g, idx, c);
            idx -= n;
        }
    }
    return 0;
}

/* Each process monster's arrival: class, where, which cell; each must stand
 * on the centre of a cell no other holds, whichever floor's grid it is. */
static void shots_log_arrivals(void)
{
    int i, j;
    for (i = 0; i < NFAKES; i++) {
        edict_t *m = monster_for(fakes[i].pid);
        float cell, c[3];
        int dup = 0;
        if (!m)
            continue;
        cell = field_of(m, "psq_gridcell");
        Sys_Printf("psq-shots: arrived pid %d %-20s at (%.0f %.0f %.0f) floor %.0f cell %.0f\n",
                   fakes[i].pid, PR_GetString(m->v.classname), m->v.origin[0], m->v.origin[1],
                   m->v.origin[2], m->v.origin[2] + m->v.mins[2], cell);
        for (j = 0; j < i; j++) {
            edict_t *o = monster_for(fakes[j].pid);
            dup = dup || (o && field_of(o, "psq_gridcell") == cell);
        }
        CHECK(cell >= 0 && cell == floorf(cell) && shots_cell_centre((int) cell, c) && !dup
              && fabsf(m->v.origin[0] - c[0]) < 0.5f && fabsf(m->v.origin[1] - c[1]) < 0.5f
              && fabsf(m->v.origin[2] + m->v.mins[2] - c[2]) <= 1.0f,
              "pid %d arrived alone on the centre of cell %.0f", fakes[i].pid, cell);
    }
}

static void shots_pin(float face_yaw)
{
    int i;
    for (i = 0; i < NFAKES; i++) {
        edict_t *m = monster_for(fakes[i].pid);
        if (m) {
            m->v.origin[2] += 4096;   /* out of the way, so no two boxes meet mid-move */
            SV_LinkEdict(m, false);
        }
    }
    for (i = 0; i < NFAKES; i++) {
        edict_t *m = monster_for(fakes[i].pid);
        vec3_t p;
        if (!m)
            continue;
        VectorCopy(pins[i], p);
        p[2] += 40;
        CHECK(drop_to_floor(m, p) && hull_fits(m, p), "pid %d pinned at (%.0f %.0f %.0f)",
              fakes[i].pid, p[0], p[1], p[2]);
        place(m, p, face_yaw);
    }
}

static void shots_stand(const shot_t *s)
{
    vec3_t p, down;
    trace_t tr;

    VectorCopy(s->p, p);
    PLAYER->v.movetype = s->air ? MOVETYPE_NOCLIP : MOVETYPE_WALK;
    if (s->air) {
        VectorCopy(p, PLAYER->v.origin);
    } else {
        p[2] += 40;
        VectorCopy(p, down);
        down[2] -= 256;
        tr = SV_Move(p, PLAYER->v.mins, PLAYER->v.maxs, down, MOVE_NOMONSTERS, PLAYER);
        CHECK(!tr.startsolid && !tr.allsolid && tr.fraction < 1.0f, "%s: the stand (%.0f %.0f "
              "%.0f) has a floor", s->name, s->p[0], s->p[1], s->p[2]);
        VectorCopy(tr.endpos, PLAYER->v.origin);
    }
    VectorCopy(vec3_origin, PLAYER->v.velocity);
    SV_LinkEdict(PLAYER, false);
    PLAYER->v.angles[0] = s->pitch;
    PLAYER->v.angles[1] = s->yaw;
    PLAYER->v.angles[2] = 0;
    VectorCopy(PLAYER->v.angles, PLAYER->v.v_angle);
    PLAYER->v.fixangle = 1;
    VectorAdd(PLAYER->v.origin, PLAYER->v.view_ofs, eye);
    Sys_Printf("psq-shots: %s: eye (%.0f %.0f %.0f) pitch %.0f yaw %.0f\n", s->name, eye[0],
               eye[1], eye[2], s->pitch, s->yaw);
}

/* Wake every process monster hunting the player, as HuntTarget does. */
static void shots_wake(void)
{
    int i;
    PLAYER->v.flags = (float) ((int) PLAYER->v.flags & ~FL_NOTARGET);
    for (i = 0; i < NFAKES; i++) {
        edict_t *m = monster_for(fakes[i].pid);
        eval_t *run = m ? GetEdictFieldValue(m, "th_run") : NULL;
        if (!m || !run)
            continue;
        m->v.enemy = EDICT_TO_PROG(PLAYER);
        m->v.goalentity = EDICT_TO_PROG(PLAYER);
        m->v.think = run->function;
        m->v.nextthink = sv.time + 0.1f;
        chase_min[i] = 1e9f;
        VectorCopy(m->v.origin, chase_mark[i]);
        VectorCopy(m->v.origin, chase_last[i]);
    }
    chase_mark_t = 0;
}

static float dist3(const vec3_t a, const vec3_t b)
{
    vec3_t d;
    VectorSubtract(a, b, d);
    return VectorLength(d);
}

static void shots_chase_step(void)
{
    int i;
    for (i = 0; i < NFAKES; i++) {
        edict_t *m = monster_for(fakes[i].pid);
        float d;
        if (!m)
            continue;
        d = dist3(m->v.origin, PLAYER->v.origin);
        if (d < chase_min[i])
            chase_min[i] = d;
        VectorCopy(m->v.origin, chase_last[i]);
    }
    if (chase_t - chase_mark_t >= STUCK_SECONDS && chase_t < CHASE_SECONDS - 0.01) {
        for (i = 0; i < NFAKES; i++) {
            edict_t *m = monster_for(fakes[i].pid);
            if (m)
                VectorCopy(m->v.origin, chase_mark[i]);
        }
        chase_mark_t = chase_t;
    }
}

static void shots_chase_report(const char *stand)
{
    int i, reached = 0, stuck = 0;
    for (i = 0; i < NFAKES; i++) {
        edict_t *m = monster_for(fakes[i].pid);
        float d, moved;
        int is_stuck;
        if (!m) {
            Sys_Printf("psq-shots: chase %s: pid %d is dead or gone\n", stand, fakes[i].pid);
            continue;
        }
        d = dist3(m->v.origin, PLAYER->v.origin);
        moved = dist3(m->v.origin, chase_mark[i]);
        is_stuck = d > STUCK_DIST && moved < STUCK_MOVE;
        reached += chase_min[i] <= 128.0f;
        stuck += is_stuck;
        Sys_Printf("psq-shots: chase %s: pid %d %-20s closest %5.0f, ends %5.0f away at "
                   "(%.0f %.0f %.0f), moved %4.0f in the last %.0f s%s\n", stand, fakes[i].pid,
                   PR_GetString(m->v.classname), chase_min[i], d, m->v.origin[0], m->v.origin[1],
                   m->v.origin[2], moved, CHASE_SECONDS - chase_mark_t,
                   is_stuck ? "  STOPPED SHORT" : "");
    }
    Sys_Printf("psq-shots: chase %s: %d of %d came within 128 units; %d stopped short\n", stand,
               reached, NFAKES, stuck);
    if (!strcmp(stand, "room")) {
        /* The start room (generate_arena.py START_ROOM: north of it, y -592,
         * is the hall): behind its lip, no walking monster gets in; only
         * the leapers (dog, fiend) may jump it. */
        int in = 0, walkers_in = 0;
        for (i = 0; i < NFAKES; i++) {
            edict_t *m = monster_for(fakes[i].pid);
            const char *c = m ? PR_GetString(m->v.classname) : "";
            if (!m || m->v.origin[1] >= -592)
                continue;
            in++;
            if (strcmp(c, "monster_dog") && strcmp(c, "monster_demon1"))
                walkers_in++;
        }
        Sys_Printf("psq-shots: chase room: %d of %d monsters got into the start room\n", in,
                   NFAKES);
        CHECK(walkers_in == 0, "no walking monster got into the start room (%d did; %d in all)",
              walkers_in, in);
    }
}

/* The next stand to chase from, at or after i; -1 when none is left. */
static int shots_next_chase(int i)
{
    for (; i < nshots; i++)
        if (shots[i].chase)
            return i;
    return -1;
}

static void PSQ_Render_Shots_f(void)
{
    const char *m = psq_new_game_map(COM_FileExists(PSQ_ARENA_BSP, NULL));

    arena_mode = 1;
    shots_mode = 1;
    CHECK(!strcmp(m, PSQ_ARENA_MAP), "New Game opens %s", m);
    CHECK(psq_arena_is_map(shots_map), "%s is an arena", shots_map);
    shots_read();
    key_dest = key_game;
    Cbuf_AddText(va("maxplayers 1\ndeathmatch 0\ncoop 0\nmap %s\npsq_render_tick\n", shots_map));
}

#define SHOTS_SUPPLY_WAIT 65.0   /* s of arena time before the frames */

static float shots_qc_global(const char *name)
{
    ddef_t *defs = (ddef_t *) ((byte *) progs + progs->ofs_globaldefs);
    int i;

    for (i = 0; i < progs->numglobaldefs; i++)
        if (!strcmp(PR_GetString(defs[i].s_name), name)) {
            if ((defs[i].type & ~DEF_SAVEGLOBAL) != ev_float || defs[i].ofs >= progs->numglobals)
                Sys_Error("psq-render: QuakeC global %s is not a float", name);
            return G_FLOAT(defs[i].ofs);
        }
    Sys_Error("psq-render: no QuakeC global %s", name);
    return 0;
}

/* The --arena-shots states, from ST_S_SETUP on; 0 when done. */
static int shots_tick(void)
{
    int n, frame, w, h;
    psq_labels_gate_t gate;
    unsigned char *rgb;

    switch (state) {
    case ST_S_SUPPLY:
        /* On an arena with resupply spots, run the clock fast until
         * SHOTS_SUPPLY_WAIT seconds into the arena, so the frames show
         * resupplied health, ammo and the first weapons. */
        if (count_class("psq_supply", NULL) == 0) {
            state = ST_S_SETUP;
            return 1;
        }
        if (Cvar_VariableValue("host_framerate") == 0) {
            Cvar_SetValue("host_framerate", 0.5f);
            wait_start = realtime;
        }
        if (shots_qc_global("psq_supply_t0") > 0
            && sv.time - shots_qc_global("psq_supply_t0") >= SHOTS_SUPPLY_WAIT) {
            Cvar_SetValue("host_framerate", 0);
            Sys_Printf("psq-shots: %g s in: %g health and ammo drops, %g weapons\n",
                       sv.time - shots_qc_global("psq_supply_t0"),
                       shots_qc_global("psq_supply_drops"), shots_qc_global("psq_weapons_given"));
            CHECK(shots_qc_global("psq_supply_drops") >= 1 && shots_qc_global("psq_weapons_given") >= 1,
                  "resupply has dropped health or ammo and a weapon");
            wait_start = realtime;
            state = ST_S_SETUP;
        }
        return 1;
    case ST_S_SETUP:
        Cvar_SetValue("psq_debug", 0);
        Cvar_SetValue("developer", 0);
        Con_ClearNotify();
        shots_log_arrivals();
        apply_scene(&shot_scene);
        /* No pitch drift: a stand's pitch holds, on the floor too. */
        Cvar_SetValue("v_centerspeed", 0);
        shots_pin(shots[0].yaw + 180);
        shot_i = 0;
        state = ST_S_APPLY;
        return 1;
    case ST_S_APPLY:
        Cvar_SetValue("host_framerate", 0);
        shots_stand(&shots[shot_i]);
        placed_at = realtime;
        state = ST_S_MOVE;
        return 1;
    case ST_S_MOVE:
        if (realtime - placed_at < 0.8)
            return 1;
        Cvar_SetValue("host_framerate", 0.00001f);
        settle = 0;
        state = ST_S_SETTLE;
        return 1;
    case ST_S_SETTLE:
        if (++settle < SETTLE_FRAMES)
            return 1;
        log_trace(va("shot-%s", shots[shot_i].name), &n, &gate, &frame);
        rgb = capture(va("shot-%s", shots[shot_i].name), &w, &h);
        free(rgb);
        if (++shot_i < nshots) {
            state = ST_S_APPLY;
            return 1;
        }
        chase_i = shots_next_chase(0);
        state = ST_S_CHASE_START;
        return 1;
    case ST_S_CHASE_START:
        Cvar_SetValue("host_framerate", 0);
        shots_pin(shots[chase_i].yaw + 180);
        shots_stand(&shots[chase_i]);
        VectorCopy(PLAYER->v.origin, chase_stand);
        shots_wake();
        Cvar_SetValue("host_framerate", CHASE_STEP);
        chase_t = 0;
        chase_frame_done = 0;
        wait_start = realtime;
        state = ST_S_CHASE;
        return 1;
    case ST_S_CHASE:
        chase_t += CHASE_STEP;
        /* The player holds the stand and its view: no input moves it, and
         * hits do not knock it out of the hall. */
        VectorCopy(chase_stand, PLAYER->v.origin);
        VectorCopy(vec3_origin, PLAYER->v.velocity);
        SV_LinkEdict(PLAYER, false);
        PLAYER->v.angles[0] = shots[chase_i].pitch;
        PLAYER->v.angles[1] = shots[chase_i].yaw;
        VectorCopy(PLAYER->v.angles, PLAYER->v.v_angle);
        PLAYER->v.fixangle = 1;
        shots_chase_step();
        if (!chase_frame_done && chase_t >= CHASE_FRAME_AT) {
            log_trace(va("chase-%s", shots[chase_i].name), &n, &gate, &frame);
            rgb = capture(va("chase-%s", shots[chase_i].name), &w, &h);
            free(rgb);
            chase_frame_done = 1;
        }
        if (chase_t < CHASE_SECONDS)
            return 1;
        shots_chase_report(shots[chase_i].name);
        chase_i = shots_next_chase(chase_i + 1);
        if (chase_i >= 0) {
            state = ST_S_CHASE_START;
            return 1;
        }
        Cvar_SetValue("host_framerate", 0);
        finish();
        return 0;
    }
    return 0;
}

static void PSQ_Render_Tick_f(void)
{
    int n, frame;
    psq_labels_gate_t gate;

    if (state == ST_DONE)
        return;
    if (realtime - wait_start > 60)
        Sys_Error("psq-render: timed out in state %d", state);
    if (shots_mode && state >= ST_S_SETUP) {
        if (shots_tick())
            Cbuf_AddText("wait\npsq_render_tick\n");
        return;
    }

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
            wait_start = realtime;
            state = shots_mode ? ST_S_SUPPLY : arena_mode ? ST_A_SETUP : ST_SETUP;
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

/* ------------------------------------------- --options, --options-restart */

/* The Options > psQuake page, driven through the engine's own key path
 * (Key_Event, Char_Event), as a player's keys would be:
 *
 *   --options          the page at launch (renice, although the command
 *                      line says +psq_killpolicy 2); moving to live kill
 *                      opens the warning and changes nothing; Enter,
 *                      Escape, a typed n, a mouse button and the Y key
 *                      without a typed Y all leave renice; only a typed Y
 *                      sets live. The console cannot change it either way.
 *                      The other rows set their cvars; config.cfg then
 *                      carries them and no kill mode; autoexec.cfg gets
 *                      an old-style psq_killpolicy 2 line
 *   --options-restart  the same user root, launched again (the restart):
 *                      the page shows renice, the other settings came
 *                      back; then game code runs a command, and the page
 *                      can only lower the kill mode and infighting
 *
 * Frames: options-page, options-livewarn, options-page-live,
 * options-cap-min, options-cap-max (the cap slider at 5 and 35),
 * options-credits (first run); options-restart, options-untrusted. */

static int opt_mode;          /* 1 --options, 2 --options-restart */
static int opt_step;
static int n_opt;
static double demo_seen;   /* --options-demo: when the demo was first seen playing */
static int opt_wait;
static unsigned char *opt_base;
static int opt_bw, opt_bh;

static void opt_press(int key)
{
    Key_Event(key, true);
    Key_Event(key, false);
}

/* A printable key as SDL delivers it: the key (by physical position),
 * then the character the layout types, then the release. */
static void opt_type(int key, int ch)
{
    Key_Event(key, true);
    Char_Event(ch);
    Key_Event(key, false);
}

static int opt_changed_pixels(const unsigned char *a, const unsigned char *b, int w, int h)
{
    int n = 0;
    int i;

    for (i = 0; i < w * h; i++)
        if (diff3(a + 3 * i, b + 3 * i) > 48)
            n++;
    return n;
}

static int opt_near(float a, float b)
{
    return a - b < 1e-6f && b - a < 1e-6f;
}

/* Changed pixels between two frames inside a rectangle of the menu canvas
 * (320x200, scaled and centred as GL_SetCanvas's CANVAS_MENU does). */
static int opt_menu_changed(const unsigned char *a, const unsigned char *b, int w, int h,
                            int mx, int my, int mw, int mh)
{
    float s = q_min((float) w / 320.0f, (float) h / 200.0f);
    int x0, y0, x1, y1, x, y;
    int n = 0;

    s = CLAMP(1.0f, scr_menuscale.value, s);
    x0 = (int) ((w - 320 * s) / 2) + (int) (mx * s);
    y0 = (int) ((h - 200 * s) / 2) + (int) (my * s);
    x1 = x0 + (int) (mw * s);
    y1 = y0 + (int) (mh * s);
    for (y = y0; y < y1 && y < h; y++)
        for (x = x0; x < x1 && x < w; x++)
            if (x >= 0 && y >= 0 && diff3(px(a, w, h, x, y), px(b, w, h, x, y)) > 48)
                n++;
    return n;
}

static char *opt_read_file(const char *name)
{
    char path[MAX_OSPATH];
    FILE *f;
    long len;
    char *buf;

    q_snprintf(path, sizeof(path), "%s/%s", com_gamedir, name);
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *) calloc(1, (size_t) len + 1);
    if (buf && fread(buf, 1, (size_t) len, f) != (size_t) len) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

static void opt_check_mode(int want, const char *what)
{
    CHECK(PSQ_KillMode() == want && psproc_kill_policy == want
          && Cvar_VariableValue("psq_killpolicy") == (float) want,
          "%s: kill mode %d (mode %d, psproc %d, psq_killpolicy %g)", what, want,
          PSQ_KillMode(), psproc_kill_policy, Cvar_VariableValue("psq_killpolicy"));
}

static void opt_capture(const char *name, int base, int min_changed)
{
    int w, h;
    unsigned char *rgb = capture(name, &w, &h);

    if (!rgb)
        return;
    if (base || !opt_base) {
        free(opt_base);
        opt_base = rgb;
        opt_bw = w;
        opt_bh = h;
        return;
    }
    if (w == opt_bw && h == opt_bh) {
        int n = opt_changed_pixels(rgb, opt_base, w, h);
        CHECK(n >= min_changed, "%s: differs from the page underneath (%d pixels, at least %d)",
              name, n, min_changed);
    }
    free(rgb);
}

static void opt_open_page(void)
{
    opt_press(K_ESCAPE);
    CHECK(key_dest == key_menu && m_state == m_main, "Escape opens the main menu");
    opt_press(K_DOWNARROW);
    opt_press(K_DOWNARROW);
    opt_press(K_ENTER);
    CHECK(m_state == m_options, "Single Player, Multiplayer, Options: the Options menu");
    opt_press(K_UPARROW);   /* wraps to Video Options */
    opt_press(K_UPARROW);   /* psQuake Options */
    opt_press(K_ENTER);
    CHECK(m_state == m_psquake, "Options > psQuake Options opens the psQuake page");
}

static void PSQ_Render_Opts_Tick_f(void)
{
    char *cfg;
    int i;

    if (state == ST_DONE)
        return;
    if (realtime - wait_start > 60)
        Sys_Error("psq-render: timed out in options step %d", opt_step);
    if (opt_wait > 0) {
        opt_wait--;
        Cbuf_AddText("wait\npsq_render_opts_tick\n");
        return;
    }

    switch (opt_step) {
    case 0:
        if (!(sv.active && cls.signon == SIGNONS && sv.time > 1 && key_dest == key_game
              && scr_con_current == 0))
            break;
        Cvar_SetValue("developer", 0);
        Con_ClearNotify();
        opt_check_mode(PSD_KILL_RENICE,
                       opt_mode != 2 ? "launch with +psq_killpolicy 2"
                                     : "restart (+psq_killpolicy 2, autoexec's "
                                       "psq_killpolicy 2, live at quit)");
        if (opt_mode == 2) {
            cfg = opt_read_file("config.cfg");
            CHECK(cfg && !strstr(cfg, "psq_killpolicy"), "config.cfg holds no kill mode");
            free(cfg);
            CHECK(Cvar_VariableValue("psq_cap") == 13 && Cvar_VariableValue("psq_classify") == 1
                  && Cvar_VariableValue("psq_infighting") == 1
                  && Cvar_VariableValue("psq_labelrange") == 2,
                  "restart: cap, monster size, infighting and label distance came back "
                  "(%g %g %g %g)", Cvar_VariableValue("psq_cap"),
                  Cvar_VariableValue("psq_classify"), Cvar_VariableValue("psq_infighting"),
                  Cvar_VariableValue("psq_labelrange"));
            CHECK(Cvar_VariableValue("psq_showpid") == 0,
                  "restart: autoexec.cfg ran (its psq_showpid 0 overrode the saved 1)");
        }
        opt_open_page();
        opt_wait = 3;
        opt_step = opt_mode == 1 ? 1 : opt_mode == 2 ? 20 : 30;
        break;

    case 1:
        opt_capture("options-page", 1, 0);
        opt_press(K_RIGHTARROW);
        CHECK(m_state == m_psq_livewarn, "renice, right: the live-kill warning opens");
        opt_check_mode(PSD_KILL_RENICE, "the warning is showing");
        opt_wait = 3;
        opt_step = 2;
        break;

    case 2:
        opt_capture("options-livewarn", 0, 800 * 600 / 10);   /* a box over the page */
        CHECK(Key_TextEntry(), "the warning takes typed characters (keyboard layout)");
        opt_press(K_ENTER);
        CHECK(m_state == m_psquake, "Enter closes the warning");
        opt_check_mode(PSD_KILL_RENICE, "Enter cancels");
        opt_press(K_RIGHTARROW);
        opt_press(K_ESCAPE);
        CHECK(m_state == m_psquake, "Escape closes the warning, back to the page");
        opt_check_mode(PSD_KILL_RENICE, "Escape cancels");
        opt_press(K_RIGHTARROW);
        opt_type('n', 'n');
        opt_check_mode(PSD_KILL_RENICE, "a typed n cancels");
        opt_press(K_RIGHTARROW);
        opt_press(K_MOUSE1);
        opt_check_mode(PSD_KILL_RENICE, "a mouse button cancels");
        opt_press(K_RIGHTARROW);
        opt_press(K_ABUTTON);
        opt_check_mode(PSD_KILL_RENICE, "a gamepad button cancels");
        opt_press(K_RIGHTARROW);
        opt_press('y');
        CHECK(m_state == m_psq_livewarn, "the Y key alone (no typed Y) leaves the warning up");
        opt_check_mode(PSD_KILL_RENICE, "the Y key's position without a typed Y");
        Char_Event('z');
        CHECK(m_state == m_psquake, "a typed z closes the warning");
        opt_check_mode(PSD_KILL_RENICE, "a typed z cancels");
        opt_press(K_LEFTARROW);
        opt_check_mode(PSD_KILL_SIMULATE, "left from renice: simulate, no warning");
        CHECK(m_state == m_psquake, "no warning for simulate");
        opt_press(K_LEFTARROW);
        CHECK(m_state == m_psq_livewarn, "left from simulate wraps to live: the warning");
        opt_check_mode(PSD_KILL_SIMULATE, "the warning is showing (from simulate)");
        opt_type('y', 'y');
        opt_check_mode(PSD_KILL_LIVE, "a typed y on the warning");
        CHECK(m_state == m_psquake, "back on the page");
        opt_wait = 3;
        opt_step = 3;
        break;

    case 3:
        opt_capture("options-page-live", 0, 2000);   /* the kill row and help text */
        Cbuf_AddText("psq_killpolicy 1\n");
        Cbuf_Execute();
        opt_check_mode(PSD_KILL_LIVE, "the console cannot lower it either");
        CHECK(PSQ_CvarSetRefused("psq_killpolicy"), "QuakeC cvar_set may not touch it");
        opt_press(K_DOWNARROW);   /* max live monsters: a new install's 6 */
        CHECK(Cvar_VariableValue("psq_cap") == 6 && psproc_monster_cap == 6,
              "max live monsters starts at 6 (%g, psproc %d)", Cvar_VariableValue("psq_cap"),
              psproc_monster_cap);
        CHECK(opt_near(PSQ_CapSliderFraction(PSD_MONSTER_CAP_MIN), 0.0f)
              && opt_near(PSQ_CapSliderFraction(6), 1.0f / 30.0f)
              && opt_near(PSQ_CapSliderFraction(20), 0.5f)
              && opt_near(PSQ_CapSliderFraction(PSD_MONSTER_CAP_MAX), 1.0f)
              && opt_near(PSQ_CapSliderFraction(0), 0.0f)
              && opt_near(PSQ_CapSliderFraction(1000), 1.0f),
              "the cap slider fills 0 at 5, 1/30 at 6, half at 20, all at 35, clamped outside");
        opt_press(K_LEFTARROW);
        CHECK(Cvar_VariableValue("psq_cap") == 5 && psproc_monster_cap == 5,
              "left: 6 -> 5 (%g)", Cvar_VariableValue("psq_cap"));
        opt_press(K_LEFTARROW);
        opt_press(K_LEFTARROW);
        CHECK(Cvar_VariableValue("psq_cap") == PSD_MONSTER_CAP_MIN && psproc_monster_cap == 5,
              "left at 5 stays at 5 (%g)", Cvar_VariableValue("psq_cap"));
        opt_wait = 3;
        opt_step = 10;
        break;

    case 10:
        opt_capture("options-cap-min", 1, 0);
        for (i = 0; i < 40; i++)
            opt_press(K_RIGHTARROW);
        CHECK(Cvar_VariableValue("psq_cap") == PSD_MONSTER_CAP_MAX && psproc_monster_cap == 35,
              "right 40 times from 5 stops at 35 (%g)", Cvar_VariableValue("psq_cap"));
        opt_wait = 3;
        opt_step = 11;
        break;

    case 11:
        {
            int w, h;
            unsigned char *rgb = capture("options-cap-max", &w, &h);
            int row = PSQ_MENU_ROW_Y(PSQ_MENU_CAP_ROW);
            int bar = PSQ_MENU_SLIDER_X;   /* 10 characters, then the right end */

            if (rgb && opt_base && w == opt_bw && h == opt_bh) {
                /* The thumb at the bar's first character for 5 and its
                 * last (bar + 72) for 35; the number after the right end. */
                CHECK(opt_menu_changed(rgb, opt_base, w, h, bar, row, 8, 8) >= 8,
                      "slider: the thumb left 5's end of the bar");
                CHECK(opt_menu_changed(rgb, opt_base, w, h, bar + 72, row, 8, 8) >= 8,
                      "slider: the thumb is at 35's end of the bar");
                CHECK(opt_menu_changed(rgb, opt_base, w, h, bar + 88, row, 16, 8) >= 8,
                      "slider: the number changed (5 -> 35)");
                CHECK(opt_menu_changed(rgb, opt_base, w, h, bar + 16, row, 48, 8) == 0,
                      "slider: the middle of the bar did not change");
                CHECK(opt_menu_changed(rgb, opt_base, w, h, 0, row, PSQ_MENU_LABEL_R, 8) == 0,
                      "slider: the row's label did not change");
            } else {
                CHECK(0, "slider frames: both captured at one size");
            }
            free(rgb);
        }
        for (i = 0; i < 22; i++)
            opt_press(K_LEFTARROW);   /* 35 -> 13 */
        opt_press(K_DOWNARROW);   /* monster size: CPU */
        opt_press(K_RIGHTARROW);
        opt_press(K_DOWNARROW);   /* infighting: on */
        opt_press(K_ENTER);
        opt_press(K_DOWNARROW);   /* all users: left off */
        opt_press(K_DOWNARROW);   /* labels: left on */
        opt_press(K_DOWNARROW);   /* PID: on */
        opt_press(K_RIGHTARROW);
        opt_press(K_DOWNARROW);   /* label distance: normal -> far */
        opt_press(K_RIGHTARROW);
        CHECK(Cvar_VariableValue("psq_cap") == 13 && psproc_monster_cap == 13,
              "max live monsters 13 (%g)", Cvar_VariableValue("psq_cap"));
        CHECK(Cvar_VariableValue("psq_classify") == 1 && psproc_classify_by == PSD_CLASSIFY_CPU,
              "monster size by CPU load");
        CHECK(Cvar_VariableValue("psq_infighting") == 1, "infighting on");
        CHECK(Cvar_VariableValue("psq_allusers") == 0 && psproc_all_users == 0,
              "all users still off");
        CHECK(Cvar_VariableValue("psq_labels") == 1 && Cvar_VariableValue("psq_showpid") == 1,
              "labels on, PID on");
        CHECK(Cvar_VariableValue("psq_labelrange") == 2, "label distance far");
        opt_press(K_DOWNARROW);   /* credits */
        opt_press(K_ENTER);
        CHECK(m_state == m_psq_credits, "Credits, licences opens the credits view");
        opt_wait = 3;
        opt_step = 4;
        break;

    case 4:
        opt_capture("options-credits", 0, 800 * 600 / 10);   /* a whole other view */
        opt_press(K_ESCAPE);
        CHECK(m_state == m_psquake, "any key: back to the page");
        Host_WriteConfiguration();
        cfg = opt_read_file("config.cfg");
        CHECK(cfg != NULL, "config.cfg written");
        if (cfg) {
            CHECK(!strstr(cfg, "psq_killpolicy"), "config.cfg holds no kill mode (live now)");
            CHECK(strstr(cfg, "psq_cap \"13\"") && strstr(cfg, "psq_classify \"1\"")
                  && strstr(cfg, "psq_infighting \"1\"") && strstr(cfg, "psq_showpid \"1\"")
                  && strstr(cfg, "psq_labelrange \"2\"") && strstr(cfg, "psq_allusers \"0\""),
                  "config.cfg holds the other psQuake settings");
            free(cfg);
        }
        {
            FILE *f = fopen(va("%s/autoexec.cfg", com_gamedir), "w");
            CHECK(f != NULL, "wrote autoexec.cfg");
            if (f) {
                /* What a phase 2-4 config carried, and a marker. */
                fputs("psq_killpolicy \"2\"\npsq_showpid 0\n", f);
                fclose(f);
            }
        }
        opt_check_mode(PSD_KILL_LIVE, "quitting in live kill");
        finish();
        return;

    case 20:
        opt_capture("options-restart", 1, 0);
        PSQ_QuakeCCommand("echo game code ran a command\n");
        CHECK(PSQ_SessionUntrusted(), "game code's command marks the session");
        opt_press(K_RIGHTARROW);
        CHECK(m_state == m_psquake, "untrusted: right from renice opens no warning");
        opt_check_mode(PSD_KILL_RENICE, "untrusted: no raise to live");
        opt_press(K_LEFTARROW);
        opt_check_mode(PSD_KILL_SIMULATE, "untrusted: lowering to simulate works");
        opt_press(K_RIGHTARROW);
        opt_check_mode(PSD_KILL_SIMULATE, "untrusted: no raise to renice");
        CHECK(m_state == m_psquake, "still on the page");
        opt_press(K_DOWNARROW);
        opt_press(K_DOWNARROW);
        opt_press(K_DOWNARROW);   /* infighting: on -> off -> stays off */
        opt_press(K_RIGHTARROW);
        CHECK(Cvar_VariableValue("psq_infighting") == 0, "untrusted: infighting can go off");
        opt_press(K_RIGHTARROW);
        CHECK(Cvar_VariableValue("psq_infighting") == 0, "untrusted: and not back on");
        opt_press(K_DOWNARROW);   /* all users stays off */
        opt_press(K_RIGHTARROW);
        CHECK(Cvar_VariableValue("psq_allusers") == 0 && psproc_all_users == 0,
              "untrusted: all users cannot be turned on");
        opt_wait = 3;
        opt_step = 21;
        break;

    case 21:
        opt_capture("options-untrusted", 0, 2000);   /* the note, kill row, help */
        finish();
        return;

    /* --options-scale: Options > Scale at 3 (the menu canvas then fills the
     * window's width), at 960x660 and then 800x600, where the scale clamps
     * to 2.5. Frames of the psQuake page, the warning, the credits and, for
     * comparison, the stock Options menu. */
    case 30:
        CHECK(vid.width == 960 && vid.height == 660, "960x660 (%dx%d)", vid.width, vid.height);
        Cvar_SetValue("scr_conscale", 3);
        Cvar_SetValue("scr_menuscale", 3);
        Cvar_SetValue("scr_sbarscale", 3);
        opt_wait = 3;
        opt_step = 31;
        break;
    case 31:
        opt_capture("scale3-960x660-psquake", 1, 0);
        opt_press(K_RIGHTARROW);
        CHECK(m_state == m_psq_livewarn, "scale 3: the warning opens");
        opt_wait = 3;
        opt_step = 32;
        break;
    case 32:
        opt_capture("scale3-960x660-livewarn", 0, 960 * 660 / 10);
        opt_press(K_ESCAPE);
        opt_check_mode(PSD_KILL_RENICE, "scale 3: Escape cancels");
        for (n_opt = 0; n_opt < 8; n_opt++)
            opt_press(K_DOWNARROW);
        opt_press(K_ENTER);
        CHECK(m_state == m_psq_credits, "scale 3: the credits view");
        opt_wait = 3;
        opt_step = 33;
        break;
    case 33:
        opt_capture("scale3-960x660-credits", 0, 960 * 660 / 10);
        opt_press(K_ESCAPE);
        opt_press(K_ESCAPE);
        CHECK(m_state == m_options, "Escape from the page: the stock Options menu");
        opt_wait = 3;
        opt_step = 34;
        break;
    case 34:
        opt_capture("scale3-960x660-options-stock", 0, 0);
        Cbuf_AddText("vid_width 800\nvid_height 600\nvid_restart\n");
        opt_wait = 10;
        opt_step = 35;
        break;
    case 35:
        CHECK(vid.width == 800 && vid.height == 600, "800x600 (%dx%d)", vid.width, vid.height);
        opt_capture("scale3-800x600-options-stock", 1, 0);
        opt_press(K_ENTER);   /* the cursor is still on psQuake Options */
        CHECK(m_state == m_psquake, "back on the psQuake page");
        for (n_opt = 0; n_opt < 8; n_opt++)
            opt_press(K_UPARROW);
        opt_wait = 3;
        opt_step = 36;
        break;
    case 36:
        opt_capture("scale3-800x600-psquake", 0, 0);
        /* The warning names the saved options that widen a kill. */
        Cvar_SetValue("psq_infighting", 1);
        Cvar_SetValue("psq_allusers", 1);
        opt_press(K_RIGHTARROW);
        CHECK(m_state == m_psq_livewarn, "800x600 scale 3: the warning opens");
        opt_wait = 3;
        opt_step = 37;
        break;
    case 37:
        opt_capture("scale3-800x600-livewarn", 0, 0);
        opt_press(K_ESCAPE);
        opt_check_mode(PSD_KILL_RENICE, "800x600 scale 3: Escape cancels");
        finish();
        return;

    /* --options-demo: a normal launch, no +map: LibreQuake's quake.rc
     * starts the attract demos, whose signon stuffs a comment line
     * ("//st 15 0"). That must not mark the session, or live kill could
     * never be chosen; a demo that stuffs a command still must. */
    case 40:
        if (!(cls.demoplayback && cls.signon == SIGNONS && realtime - demo_seen > 2.0)) {
            if (!cls.demoplayback || cls.signon != SIGNONS)
                demo_seen = realtime;
            break;
        }
        Cvar_SetValue("developer", 0);
        Con_ClearNotify();
        CHECK(cls.demoplayback && !sv.active, "the attract demo is playing (no local server)");
        CHECK(!PSQ_SessionUntrusted(),
              "two seconds of the attract demo did not mark the session untrusted");
        opt_check_mode(PSD_KILL_RENICE, "launch, attract demo playing");
        opt_open_page();
        opt_wait = 3;
        opt_step = 41;
        break;
    case 41:
        opt_capture("options-over-demo", 1, 0);
        opt_press(K_RIGHTARROW);
        CHECK(m_state == m_psq_livewarn, "over the demo: renice, right opens the warning");
        opt_press(K_ESCAPE);
        opt_check_mode(PSD_KILL_RENICE, "over the demo: Escape cancels");
        /* The guard itself still holds for a demo's real commands. */
        PSQ_StuffText("// a comment\n");
        CHECK(!PSQ_SessionUntrusted(), "a comment-only stuff leaves the session trusted");
        PSQ_StuffText("// a ; psq_allusers 1\n");
        CHECK(PSQ_SessionUntrusted(), "a ';' after a comment marks the session");
        opt_press(K_RIGHTARROW);
        CHECK(m_state == m_psquake, "untrusted: right opens no warning");
        opt_check_mode(PSD_KILL_RENICE, "untrusted over the demo: no raise");
        finish();
        return;
    }
    Cbuf_AddText("wait\npsq_render_opts_tick\n");
}

static void PSQ_Render_Options_f(void)
{
    opt_mode = Q_atoi(Cmd_Argv(1));
    if (opt_mode < 1 || opt_mode > 4)
        opt_mode = 1;
    opt_step = opt_mode == 4 ? 40 : 0;
    Cbuf_AddText("psq_render_opts_tick\n");
}

/* psq_test_killmode N: what the options page does with the warning
 * answered yes. Test builds only. */
static void PSQ_Test_KillMode_f(void)
{
    int r = PSQ_SetKillMode(Q_atoi(Cmd_Argv(1)), true);

    if (r != PSQ_KILLMODE_CHANGED && r != PSQ_KILLMODE_UNCHANGED)
        Sys_Error("psq-render: psq_test_killmode %s refused (%d)", Cmd_Argv(1), r);
}

void psq_test_register(void)
{
    Cmd_AddCommand("psq_render_tick", PSQ_Render_Tick_f);
    Cmd_AddCommand("psq_render_arena", PSQ_Render_Arena_f);
    Cmd_AddCommand("psq_render_shots", PSQ_Render_Shots_f);
    Cmd_AddCommand("psq_render_options", PSQ_Render_Options_f);
    Cmd_AddCommand("psq_render_opts_tick", PSQ_Render_Opts_Tick_f);
    Cmd_AddCommand("psq_test_killmode", PSQ_Test_KillMode_f);
}

int main(int argc, char *argv[])
{
    static char *def_argv[] = {
        "psq_labels_render",
        "-nosound", "-nocdaudio", "-nojoy", "-width", "640", "-height", "480", "-window",
        "+developer", "1", "+psq_debug", "1", "+psq_testarena", "1", "+psq_test_killmode", "0",
        "+map", "lq_e1m1", "+psq_render_tick",
        "-psquake-root", PSQ_TEST_ROOT, NULL };
    /* --arena: New Game, psq_testarena off. The +commands come before the
     * root: the engine keeps only the first 256 characters of them. */
    static char *arena_argv[] = {
        "psq_labels_render",
        "-nosound", "-nocdaudio", "-nojoy", "-width", "640", "-height", "480", "-window",
        "+developer", "1", "+psq_debug", "1", "+psq_test_killmode", "0", "+psq_render_arena",
        "-psquake-root", PSQ_TEST_ROOT "-arena", NULL };
    /* --options and --options-restart: one user root, so the second run is
     * the first one's restart. The app's default window size; the command
     * line asks for live kill, which no launch may give. */
    static char *options_argv[] = {
        "psq_labels_render",
        "-nosound", "-nocdaudio", "-nojoy", "-width", "800", "-height", "600", "-window",
        "+developer", "1", "+psq_killpolicy", "2", "+map", "lq_e1m1",
        "+psq_render_options", "1",
        "-psquake-root", PSQ_TEST_ROOT "-options", NULL };
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
    if ((argc == 2 && !strcmp(argv[1], "--arena"))
        || (argc == 3 && !strcmp(argv[1], "--arena-shots"))) {
        static char shots_root[1024];
        int k;
        if (argc == 3) {
            /* --arena-shots <arena>: that map, its own user root. */
            for (k = 0; psq_arena_maps[k] && strcmp(psq_arena_maps[k], argv[2]); k++)
                ;
            if (!psq_arena_maps[k]) {
                printf("psq-render: FAIL: --arena-shots: %s is not an arena\n", argv[2]);
                return 1;
            }
            q_strlcpy(shots_map, psq_arena_maps[k], sizeof(shots_map));
            q_snprintf(shots_root, sizeof(shots_root), "%s-shots-%s", PSQ_TEST_ROOT, shots_map);
            for (k = 0; arena_argv[k]; k++) {
                if (!strcmp(arena_argv[k], "+psq_render_arena"))
                    arena_argv[k] = "+psq_render_shots";
                else if (!strcmp(arena_argv[k], "-psquake-root"))
                    arena_argv[++k] = shots_root;
            }
        }
        argc = (int) (sizeof(arena_argv) / sizeof(arena_argv[0])) - 1;
        argv = arena_argv;
    } else if (argc == 2 && (!strcmp(argv[1], "--options")
                             || !strcmp(argv[1], "--options-restart")
                             || !strcmp(argv[1], "--options-scale")
                             || !strcmp(argv[1], "--options-demo"))) {
        if (!strcmp(argv[1], "--options-restart"))
            options_argv[16] = "2";
        if (!strcmp(argv[1], "--options-scale")) {
            options_argv[5] = "960";
            options_argv[7] = "660";
            options_argv[16] = "3";
            options_argv[18] = PSQ_TEST_ROOT "-options-scale";
        }
        if (!strcmp(argv[1], "--options-demo")) {
            options_argv[13] = "+psq_debug";   /* in place of +map lq_e1m1 */
            options_argv[14] = "0";
            options_argv[16] = "4";
            options_argv[18] = PSQ_TEST_ROOT "-options-demo";
        }
        argc = (int) (sizeof(options_argv) / sizeof(options_argv[0])) - 1;
        argv = options_argv;
    } else if (argc <= 1) {
        argc = (int) (sizeof(def_argv) / sizeof(def_argv[0])) - 1;
        argv = def_argv;
    }
    /* The engine keeps only the first 255 characters of the command line
     * for its +commands (COM_InitArgv), so they come before the root; a
     * longer build path must not cut one off unnoticed. */
    {
        size_t n = 0;
        int i;

        for (i = 0; i < argc && strcmp(argv[i], "-psquake-root"); i++)
            n += strlen(argv[i]) + 1;
        if (n > 255) {
            printf("psq-render: FAIL: the +commands need %zu characters; the engine keeps 255\n", n);
            return 1;
        }
    }
    wait_start = 0;
    return psq_engine_main(argc, argv);
}
