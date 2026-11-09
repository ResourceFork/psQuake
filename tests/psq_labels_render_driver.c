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
 *   scenes    window and fullscreen sizes, PID on and off, each label
 *             range, labels off, the menu open, the status bar hidden; for
 *             each it writes a PNG of the frame and checks the label trace
 *             of the same frame (gate, verdicts, positions, text) and that
 *             label pixels really changed against a labels-off frame
 *   end       a real intermission (svc_intermission) and playback of a demo
 *             recorded during the scenes: neither may draw a label
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

#include "psq_engine.h"
#include "psq_labels.h"
#include "psq_offscreen.h"
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
    { "labels-640x480",             640,  480, 0, 0, PSD_LABELS_NORMAL, 1, 100, 0 },
    { "labels-1920x1080-far",       1920, 1080, 0, 1, PSD_LABELS_FAR,   1, 100, 0 },
    { "labels-fullscreen-2560x1600", 0,    0,  1, 1, PSD_LABELS_ALL,    1, 100, 0 },
    { "labels-1280x720-nosbar",     1280, 720, 0, 0, PSD_LABELS_NORMAL, 1, 120, 0 },
    { "labels-1280x720-menu",       1280, 720, 0, 1, PSD_LABELS_NORMAL, 1, 100, 1 },
};
#define NSCENES ((int) (sizeof(scenes) / sizeof(scenes[0])))
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

/* Frame captures. The off scene's frame is kept to compare label pixels. */
static unsigned char *off_rgb;
static int off_w, off_h;
static vrect_t off_vrect;

static unsigned char *capture(const char *name, int *w, int *h)
{
    unsigned char *rgb = NULL;
    char file[64];

    if (!psq_offscreen_read(&rgb, w, h)) {
        CHECK(0, "%s: read the framebuffer", name);
        return NULL;
    }
    q_snprintf(file, sizeof(file), "%s.png", name);
    CHECK(Image_WritePNG(file, rgb, *w, *h, 24, false), "%s: wrote %s/%s", name, com_gamedir, file);
    return rgb;
}

/* Fraction of a label row's pixels that differ from the labels-off frame
 * (rgb rows are bottom-up). */
static float changed_fraction(const unsigned char *a, const unsigned char *b, int w, int h,
                              int x0, int y0, int rw, int rh)
{
    int x, y, changed = 0, total = 0;

    for (y = y0; y < y0 + rh; y++) {
        for (x = x0; x < x0 + rw; x++) {
            size_t o;
            if (x < 0 || y < 0 || x >= w || y >= h)
                continue;
            o = ((size_t) (h - 1 - y) * w + x) * 3;
            total++;
            if (abs(a[o] - b[o]) + abs(a[o + 1] - b[o + 1]) + abs(a[o + 2] - b[o + 2]) > 24)
                changed++;
        }
    }
    return total ? (float) changed / total : 0;
}

static void check_scene(const scene_t *s)
{
    psq_labels_gate_t gate;
    int n, frame, i, j, w = 0, h = 0, drawn = 0, faded = 0, crowded = 0, overlaps = 0;
    const psq_label_trace_t *t = log_trace(s->name, &n, &gate, &frame);
    const psq_label_trace_t *hidden;
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
        free(off_rgb);
        off_rgb = rgb;
        off_w = w;
        off_h = h;
        off_vrect = r_refdef.vrect;
        return;
    }
    if (s->menu) {
        CHECK(key_dest == key_menu && gate == PSQ_LABELS_MENU && n == 0,
              "%s: no label while the menu is open", s->name);
        free(rgb);
        return;
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
                    if (a->x[ra] < b->x[rb] + b->width[rb] && b->x[rb] < a->x[ra] + a->width[ra]
                        && a->y[ra] < b->y[rb] + PSQ_LABEL_GLYPH * b->scale
                        && b->y[rb] < a->y[ra] + PSQ_LABEL_GLYPH * a->scale)
                        overlaps++;
        }
        if (t[i].verdict != PSQ_LABEL_DRAWN)
            continue;
        drawn++;
        CHECK(l->top >= vy && l->bottom <= vy + vh, "%s: pid %d label inside the view vertically",
              s->name, t[i].pid);
        CHECK(l->scale == psq_label_text_scale(vh, CLAMP(1.0f, scr_sbarscale.value, (float) vid.width / 320.0f)), "%s: pid %d text scale %d", s->name, t[i].pid,
              l->scale);
        CHECK(t[i].alpha > 0 && t[i].alpha <= 1, "%s: pid %d alpha %.2f", s->name, t[i].pid,
              t[i].alpha);
        for (r = 0; r < l->rows; r++)
            CHECK(l->x[r] >= vx && l->x[r] + l->width[r] <= vx + vw,
                  "%s: pid %d row %d inside the view horizontally", s->name, t[i].pid, r);
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
        if (off_rgb && rgb && off_w == w && off_h == h && !s->menu
            && !memcmp(&off_vrect, &r_refdef.vrect, sizeof(off_vrect))) {
            float best = 0;
            for (r = 0; r < l->rows; r++) {
                float f = changed_fraction(rgb, off_rgb, w, h, l->x[r], l->y[r], l->width[r],
                                           PSQ_LABEL_GLYPH * l->scale);
                if (f > best)
                    best = f;
            }
            CHECK(best > 0.05f, "%s: pid %d label pixels drawn where the trace says (%.0f%% changed)",
                  s->name, t[i].pid, best * 100);
        }
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
    CHECK(overlaps == 0, "%s: no two drawn labels overlap", s->name);
    if (rgb != off_rgb)
        free(rgb);
}

/* ------------------------------------------------------------ the ticker */

enum { ST_WAIT_GAME, ST_SETUP, ST_SCENE_APPLY, ST_SCENE_SETTLE, ST_INTERMISSION,
       ST_INTER_CHECK, ST_INTER_CONSOLE, ST_DEMO, ST_DEMO_CHECK, ST_DONE };

static int    state = ST_WAIT_GAME;
static int    scene;
static int    settle;
static double wait_start;

static void finish(void)
{
    /* No NSApplication: SDL's Cocoa video never started, so nothing could
     * have reached the screen. */
    void **nsapp = (void **) dlsym(RTLD_DEFAULT, "NSApp");
    CHECK(nsapp == NULL || *nsapp == NULL, "no NSApplication was ever created");
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
            && sv.time > 3) {
            Sys_Printf("psq-render: %d process monsters at %.1f s\n", NFAKES, sv.time);
            state = ST_SETUP;
        }
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
        state = ST_SCENE_APPLY;
        break;
    case ST_SCENE_APPLY:
        apply_scene(&scenes[scene]);
        settle = 0;
        state = ST_SCENE_SETTLE;
        break;
    case ST_SCENE_SETTLE:
        if (++settle < SETTLE_FRAMES) {
            log_trace(scenes[scene].name, &n, &gate, &frame);
            break;
        }
        check_scene(&scenes[scene]);
        if (++scene < NSCENES)
            state = ST_SCENE_APPLY;
        else {
            Cvar_SetValue("psq_labels", 1);
            Cvar_SetValue("viewsize", 100);
            if (key_dest == key_menu)
                M_ToggleMenu_f();
            state = ST_INTERMISSION;
        }
        break;
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
        Cbuf_AddText("stop\nplaydemo psqlabels\n");
        settle = 0;
        state = ST_DEMO;
        break;
    case ST_DEMO:
        if (!cls.demoplayback || cls.signon != SIGNONS || scr_con_current > 0 || ++settle < 20)
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
    }
    Cbuf_AddText("wait\npsq_render_tick\n");
}

void psq_test_register(void)
{
    Cmd_AddCommand("psq_render_tick", PSQ_Render_Tick_f);
}

int main(int argc, char *argv[])
{
    static char *def_argv[] = {
        "psq_labels_render", "-psquake-root", PSQ_TEST_ROOT,
        "-nosound", "-nocdaudio", "-nojoy", "-width", "640", "-height", "480", "-window",
        "+developer", "1", "+psq_debug", "1", "+psq_testarena", "1", "+psq_killpolicy", "0",
        "+map", "lq_e1m1", "+psq_render_tick", NULL };
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
    if (argc <= 1) {
        argc = (int) (sizeof(def_argv) / sizeof(def_argv[0])) - 1;
        argv = def_argv;
    }
    wait_start = 0;
    return psq_engine_main(argc, argv);
}
