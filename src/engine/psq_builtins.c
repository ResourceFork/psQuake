/*
 * psQuake -- engine glue for the process-monster builtins #1200-#1208.
 *
 * QuakeC reaches processes only through these builtins, and they only
 * forward to the engine-free policy (src/core/psq_policy.c), which decides
 * everything and acts only through psproc's pid-reuse-checked wrappers.
 * Before every call the gate is recomputed from the server's state: closed
 * unless this is a local single-player game on the arena (or a test arena
 * the user turned on in a trusted session), so a builtin is then a no-op.
 *
 *   #1200 float()              psq_poll        refresh (1 Hz), pool size
 *   #1201 float(float i)       psq_proc_pid    pid of pool entry i
 *   #1202 string(float i)      psq_proc_name   its name (stable for the level)
 *   #1203 string(float i)      psq_class       its committed monster class
 *   #1204 float(float pid)     psq_burst       swarm members owed now
 *   #1205 void(float pid, float take) psq_wound  hit: renice if policy allows
 *   #1206 void(float pid)      psq_kill        died: killed; SIGTERM if live
 *   #1207 float(float pid)     psq_present     still listed and not killed
 *   #1208 string(float t)      psq_tier_class  t-th available class; -1 swarm
 *
 * Also here: the psq_* cvars, the level reset, the engine side of the save
 * rule (psq_save_rule_for in psq_arena.h: never the arena, never a level
 * the process controller has run on; process monsters live only while their
 * processes are listed), and the arena grid builtins #1209-#1210.
 *
 * MIT licensed, like the rest of psQuake's own files. It is compiled only
 * into the GPL-2 engine binary.
 */

#include "quakedef.h"
#include "psq_engine.h"

#include "psq_arena.h"
#include "psq_grid.h"
#include "psq_policy.h"
#include "proc_script.h"
#include "psproc_options.h"

#include <math.h>

/* Options, set on the Options > psQuake page (psq_menu.c). All but the kill
 * mode are archived, so they persist in config.cfg. The kill mode is never
 * saved: every launch starts renice-only, and only the page changes it
 * (PSQ_SetKillMode); psq_killpolicy is its read-only mirror, and the
 * console, a config file, a +command, a server, a demo and QuakeC cannot
 * set it. */
static cvar_t psq_killpolicy = {"psq_killpolicy", "1", CVAR_NONE};
static int    psq_killmode = PSD_KILL_RENICE;
static cvar_t psq_allusers   = {"psq_allusers",   "0", CVAR_ARCHIVE};
static cvar_t psq_cap        = {"psq_cap",        "10", CVAR_ARCHIVE};
static cvar_t psq_classify   = {"psq_classify",   "0", CVAR_ARCHIVE};
static cvar_t psq_labels     = {"psq_labels",     "1", CVAR_ARCHIVE};
static cvar_t psq_showpid    = {"psq_showpid",    "0", CVAR_ARCHIVE};
static cvar_t psq_labelrange = {"psq_labelrange", "1", CVAR_ARCHIVE};
/* 0 (default): a monster's attack on a process monster or Scrag, or theirs
 * on another monster, does no damage, so only the player wounds and kills
 * them. 1: they can hurt and kill each other, and the policy acts on those
 * processes as for the player's hits. Read by QuakeC (psq.qc). */
static cvar_t psq_infighting = {"psq_infighting", "0", CVAR_ARCHIVE};
static float  psq_infighting_old;

/* Not archived. psq_debug logs every builtin call. psq_testarena makes
 * worldspawn start the process controller on any map, until the arena map
 * (which places a psq_arena entity) exists. */
static cvar_t psq_debug      = {"psq_debug",      "0", CVAR_NONE};
static cvar_t psq_testarena  = {"psq_testarena",  "0", CVAR_NONE};

void (*psq_test_poll_hook)(int poll_count);

/* ------------------------------------------------------------- options */

static qboolean psq_syncing;

/* Set once a remote server or a demo has put commands in the console buffer
 * (svc_stufftext with no local server, or during demo playback), or game
 * code has (PSQ_QuakeCCommand, PSQ_ChangelevelRefused): from then on, until
 * psQuake restarts, nothing may raise the kill policy, widen it to all
 * users, turn psq_infighting on or let psq_testarena open the gate. A
 * stuffed command can be split across messages or run after
 * a stuffed `disconnect`, so the session is marked rather than each
 * message filtered. */
static qboolean psq_untrusted;

void PSQ_StuffText(const char *text)
{
    /* Comment-only text runs nothing (LibreQuake's attract demos stuff
     * "//st 15 0"), so it leaves the session trusted. */
    if ((!sv.active || cls.demoplayback) && !psq_untrusted
        && !psq_stuff_text_inert(text))
    {
        psq_untrusted = true;
        Con_DPrintf("psQuake: a server or demo sent console commands; the "
                    "kill policy and psq_infighting cannot be raised "
                    "until restart\n");
    }
}

/* QuakeC's localcmd and stuffcmd reach the console buffer, where a
 * command runs as if typed: anything but the stock game code's own commands
 * marks the session as a stuffed one does. */
void PSQ_QuakeCCommand(const char *text)
{
    if (!psq_untrusted && !psq_qc_command_trusted(text))
    {
        psq_untrusted = true;
        Con_DPrintf("psQuake: game code sent console commands; the kill "
                    "policy and psq_infighting cannot be raised until "
                    "restart\n");
    }
}

/* QuakeC's changelevel puts `changelevel <name>` in the console buffer, and
 * the name is usually a map's own "map" key: anything but a plain map name
 * could run further commands, so it is refused, and the session marked as
 * for any other game code's commands. */
qboolean PSQ_ChangelevelRefused(const char *map)
{
    if (psq_map_name_plain(map))
        return false;
    Con_Printf("psQuake: a level exit named something other than a map; "
               "refused\n");
    if (!psq_untrusted)
    {
        psq_untrusted = true;
        Con_DPrintf("psQuake: game code sent console commands; the kill "
                    "policy and psq_infighting cannot be raised until "
                    "restart\n");
    }
    return true;
}

qboolean PSQ_CvarSetRefused(const char *name)
{
    /* QuakeC never sets these: a mod's progs would otherwise arm SIGTERM,
     * or put process monsters on any level. */
    if (name && (!q_strcasecmp(name, psq_killpolicy.name)
                 || !q_strcasecmp(name, psq_allusers.name)
                 || !q_strcasecmp(name, psq_testarena.name)
                 || !q_strcasecmp(name, psq_infighting.name)))
    {
        Con_Printf("psQuake: QuakeC may not set %s\n", name);
        return true;
    }
    return false;
}

/* Copy the cvars into psproc's settings, clamp them there, and write any
 * clamped value back, so QuakeC and the console see what the policy uses. */
static void PSQ_SyncOptions(cvar_t *var)
{
    int old_policy = psproc_kill_policy;
    int old_all = psproc_all_users;

    (void) var;
    if (psq_syncing)
        return;
    psq_syncing = true;

    /* The mirror changed, and not through PSQ_SetKillMode: put it back. */
    if (psq_killpolicy.value != (float) psq_killmode)
    {
        Con_Printf("psQuake: the kill mode is chosen on Options > psQuake; "
                   "psq_killpolicy cannot be set from the console, a config "
                   "file, the command line or the game\n");
        Cvar_SetValueQuick(&psq_killpolicy, (float) psq_killmode);
    }

    psproc_kill_policy = psq_killmode;
    psproc_all_users = (int) psq_allusers.value;
    psproc_monster_cap = (int) psq_cap.value;
    psproc_classify_by = (int) psq_classify.value;
    psproc_show_labels = (int) psq_labels.value;
    psproc_show_pid = (int) psq_showpid.value;
    psproc_label_range = (int) psq_labelrange.value;
    psproc_options_sanitize();
    if (psproc_kill_policy != psq_killmode)
        Sys_Error("psQuake: kill mode %d is not a valid mode", psq_killmode);
    if (!psq_option_change_allowed(psq_untrusted, old_policy,
                                   psproc_kill_policy, old_all,
                                   psproc_all_users))
    {
        if (psproc_all_users > old_all)
            psproc_all_users = old_all;
        Con_Printf("psQuake: a server, demo or game code ran commands this "
                   "session; restart psQuake to set psq_allusers\n");
    }

    if (psq_allusers.value != (float) psproc_all_users)
        Cvar_SetValueQuick(&psq_allusers, (float) psproc_all_users);
    if (psq_cap.value != (float) psproc_monster_cap)
        Cvar_SetValueQuick(&psq_cap, (float) psproc_monster_cap);
    if (psq_classify.value != (float) psproc_classify_by)
        Cvar_SetValueQuick(&psq_classify, (float) psproc_classify_by);
    if (psq_labels.value != (float) psproc_show_labels)
        Cvar_SetValueQuick(&psq_labels, (float) psproc_show_labels);
    if (psq_showpid.value != (float) psproc_show_pid)
        Cvar_SetValueQuick(&psq_showpid, (float) psproc_show_pid);
    if (psq_labelrange.value != (float) psproc_label_range)
        Cvar_SetValueQuick(&psq_labelrange, (float) psproc_label_range);

    if (psproc_kill_policy != old_policy)
    {
        if (psproc_kill_policy == PSD_KILL_LIVE)
            Con_Printf("psQuake: psq_killpolicy 2 (live): killing a process "
                       "monster sends SIGTERM to its process\n");
        else if (psproc_kill_policy == PSD_KILL_RENICE)
            Con_Printf("psQuake: psq_killpolicy 1 (renice): hits renice "
                       "processes; kills signal nothing\n");
        else
            Con_Printf("psQuake: psq_killpolicy 0 (simulate): no process is "
                       "touched\n");
    }
    psq_syncing = false;
}

/* psq_infighting is 0 or 1; after a server, demo or game code ran commands
 * it may only be turned off (it lets monsters, not only the player, wound
 * and kill process monsters). */
static void PSQ_InfightingChanged(cvar_t *var)
{
    float want = psq_infighting.value != 0 ? 1 : 0;

    (void) var;
    if (psq_syncing)
        return;
    psq_syncing = true;
    if (!psq_flag_change_allowed(psq_untrusted, psq_infighting_old != 0,
                                 want != 0))
    {
        want = psq_infighting_old;
        Con_Printf("psQuake: a server, demo or game code ran commands this "
                   "session; restart psQuake to turn psq_infighting on\n");
    }
    if (psq_infighting.value != want)
        Cvar_SetValueQuick(&psq_infighting, want);
    if (want != psq_infighting_old)
        Con_Printf("psQuake: psq_infighting %g: monsters %s process "
                   "monsters\n", want, want ? "can hurt and kill"
                                             : "do no damage to");
    psq_infighting_old = want;
    psq_syncing = false;
}

/* The one way the kill mode changes (psq_killmode_change decides). Only
 * the Options > psQuake page calls it, and it passes live_confirmed only
 * from the live-kill warning answered yes. */
int PSQ_SetKillMode(int mode, qboolean live_confirmed)
{
    psq_killmode_result_t r = psq_killmode_change(psq_killmode, mode,
                                                  live_confirmed != 0,
                                                  psq_untrusted);

    if (r == PSQ_KILLMODE_CHANGED)
    {
        psq_killmode = mode;
        Cvar_SetValueQuick(&psq_killpolicy, (float) mode);  /* syncs psproc */
    }
    else if (r == PSQ_KILLMODE_UNTRUSTED)
        Con_Printf("psQuake: a server, demo or game code ran commands this "
                   "session; restart psQuake to raise the kill mode\n");
    return (int) r;
}

int PSQ_KillMode(void)
{
    return psq_killmode;
}

qboolean PSQ_SessionUntrusted(void)
{
    return psq_untrusted;
}

void PSQ_Init(void)
{
    cvar_t *opts[] = { &psq_killpolicy, &psq_allusers, &psq_cap,
                       &psq_classify, &psq_labels, &psq_showpid,
                       &psq_labelrange };
    size_t i;

    for (i = 0; i < sizeof(opts) / sizeof(opts[0]); i++)
    {
        Cvar_RegisterVariable(opts[i]);
        Cvar_SetCallback(opts[i], PSQ_SyncOptions);
    }
    Cvar_RegisterVariable(&psq_infighting);
    Cvar_SetCallback(&psq_infighting, PSQ_InfightingChanged);
    psq_infighting_old = psq_infighting.value;
    Cvar_RegisterVariable(&psq_debug);
    Cvar_RegisterVariable(&psq_testarena);
    PSQ_SyncOptions(NULL);

#ifdef PSQ_HEADLESS_TEST
    /* Test build: psproc_fake only (no native backend is linked), driven
     * by tests/psq_headless_driver.c. */
    psq_test_install_backend();
    psq_test_register();
    Con_Printf("psQuake: process backend: fake (test build)\n");
#else
    /* PSQUAKE_POLL_CMD set: monsters come from the user's script, and
     * wounds and kills go to PSQUAKE_RESPOND_CMD (psproc/docs). */
    if (proc_script_install_from_env("PSQUAKE"))
        Con_Printf("psQuake: process backend: script (PSQUAKE_POLL_CMD)\n");
    else
        Con_Printf("psQuake: process backend: native\n");
#endif
    psq_policy_init();
}

/* ---------------------------------------------------------------- level */

static int PSQ_ModelExists(const char *model)
{
    return COM_FileExists(model, NULL);
}

static qboolean psq_gated_logged;   /* psq_debug: one gated-poll line per level */

void PSQ_LevelReset(void)
{
    psq_gated_logged = false;
    psq_policy_level_reset(PSQ_ModelExists);
}


/* --------------------------------------------------------------- saving */

/* 1 if any edict carries a process tag (psq_pid != 0). */
static qboolean PSQ_AnyProcessEdict(void)
{
    int i;

    for (i = 1; i < sv.num_edicts; i++)
    {
        edict_t *ed = EDICT_NUM(i);
        eval_t  *v;

        if (ed->free)
            continue;
        /* The controller itself, too: before its first poll it has spawned
         * nothing, but a save of it would come back on a level that never
         * precached the classes it spawns. */
        if (!strcmp(PR_GetString(ed->v.classname), "psq_arena"))
            return true;
        v = GetEdictFieldValue(ed, "psq_pid");
        if (v != NULL && v->_float != 0)
            return true;
    }
    return false;
}

int PSQ_SaveRule(void)
{
    int process_level;

    if (!sv.active)
        return PSQ_SAVE_ALLOWED;
    process_level = psq_policy_level_active() || PSQ_AnyProcessEdict();
    return psq_save_rule_for(sv.name, process_level);
}

void PSQ_AfterLoad(void)
{
    int i;
    int cleared = 0;

    /* A save never holds a process monster (PSQ_SaveRule). Should one
     * carry a tag anyway, it becomes an ordinary monster: no pid from a
     * file is ever acted on. */
    for (i = 1; i < sv.num_edicts; i++)
    {
        edict_t *ed = EDICT_NUM(i);
        eval_t  *v;

        if (ed->free)
            continue;
        v = GetEdictFieldValue(ed, "psq_pid");
        if (v != NULL && v->_float != 0)
        {
            v->_float = 0;
            cleared++;
        }
    }
    if (cleared)
        Con_Printf("psQuake: cleared %d process tags from a loaded game\n",
                   cleared);
}

/* ------------------------------------------------------------- builtins */

int PSQ_ProcessGateAllows(void)
{
    psq_gate_facts f;

    f.server_active = sv.active;
    f.maxclients = svs.maxclients;
    f.deathmatch = deathmatch.value != 0;
    f.coop = coop.value != 0;
#ifdef PSQ_HEADLESS_TEST
    /* Test build only (psproc_fake, no native backend linked): a dedicated
     * loopback server stands in for the local game. */
    f.dedicated = 0;
#else
    f.dedicated = isDedicated;
#endif
    /* Only the arena, or a test arena the user turned on: a controller that
     * another map or a crafted save brings stays inert. */
    return psq_gate_allows(&f)
        && psq_level_allowed(psq_arena_is_map(sv.name),
                             psq_testarena.value != 0, psq_untrusted);
}

static void PSQ_Gate(void)
{
    psq_policy_set_gate(PSQ_ProcessGateAllows());
}

/* A QuakeC float as a pid or index: an exact integer in the range a float
 * holds exactly, else 0 (never a valid pid or a process index > 0). */
static int PSQ_FloatToInt(float f)
{
    if (!(f >= -16777216.0f && f <= 16777216.0f) || f != floorf(f))
        return 0;
    return (int) f;
}

static const char *PSQ_OutcomeName(psq_result_t r)
{
    switch (r.outcome)
    {
    case PSQ_OUT_GATED:       return "gated (not local single player)";
    case PSQ_OUT_NOT_PROCESS: return "not a process monster";
    case PSQ_OUT_UNKNOWN:     return "refused: pid not in the current poll";
    case PSQ_OUT_KILLED:      return "already killed";
    case PSQ_OUT_GONE:        return "process already found gone";
    case PSQ_OUT_NO_DAMAGE:   return "no damage";
    case PSQ_OUT_POLICY:      return "kill policy: no process action";
    case PSQ_OUT_SWARM:       return "swarm member ended";
    case PSQ_OUT_FULL:        return "refused: too many kills on this level";
    case PSQ_OUT_ACTED:
        switch (r.act)
        {
        case PSD_ACT_DONE:    return "done";
        case PSD_ACT_FAILED:  return "backend failed";
        case PSD_ACT_GONE:    return "skipped: process has exited";
        case PSD_ACT_REUSED:  return "skipped: pid belongs to another process";
        case PSD_ACT_REFUSED: return "skipped: could not verify";
        }
    }
    return "?";
}

void PF_psq_poll(void)
{
    static int last_count = -1;
    int n;
    int count;

    PSQ_Gate();
    n = psq_policy_poll(sv.time);
    count = psq_policy_poll_count();
    if (psq_debug.value && !psq_policy_level_active() && !psq_gated_logged)
    {
        psq_gated_logged = true;
        Con_Printf("psq: poll at %.1f gated: no process is listed here\n",
                   sv.time);
    }
    if (count != last_count)
    {
        last_count = count;
        if (psq_debug.value && count > 0)
            Con_Printf("psq: poll %d at %.1f: %d processes, %d swarm live\n",
                       count, sv.time, n, psq_policy_swarm_live());
    }
    if (psq_test_poll_hook)
        psq_test_poll_hook(count);
    G_FLOAT(OFS_RETURN) = n;
}

void PF_psq_proc_pid(void)
{
    const psd_proc_t *e;

    PSQ_Gate();
    e = psq_policy_entry(PSQ_FloatToInt(G_FLOAT(OFS_PARM0)));
    G_FLOAT(OFS_RETURN) = e ? e->pid : 0;
}

void PF_psq_proc_name(void)
{
    PSQ_Gate();
    G_INT(OFS_RETURN) = PR_SetEngineString(
        psq_policy_name(PSQ_FloatToInt(G_FLOAT(OFS_PARM0))));
}

void PF_psq_class(void)
{
    PSQ_Gate();
    G_INT(OFS_RETURN) = PR_SetEngineString(
        psq_policy_class(PSQ_FloatToInt(G_FLOAT(OFS_PARM0))));
}

void PF_psq_burst(void)
{
    int pid;
    int owed;

    PSQ_Gate();
    pid = PSQ_FloatToInt(G_FLOAT(OFS_PARM0));
    owed = psq_policy_burst(pid);
    if (owed > 0)
        Con_Printf("psq: fork burst from pid %d: %d swarm members\n", pid, owed);
    G_FLOAT(OFS_RETURN) = owed;
}

void PF_psq_wound(void)
{
    int          pid;
    float        take;
    psq_result_t r;

    PSQ_Gate();
    pid = PSQ_FloatToInt(G_FLOAT(OFS_PARM0));
    take = G_FLOAT(OFS_PARM1);
    r = psq_policy_wound(pid, isfinite(take) ? take : 0);
    if (psq_debug.value
        || (r.outcome == PSQ_OUT_ACTED && r.act != PSD_ACT_DONE))
        Con_Printf("psq: wound pid %d take %g: %s\n", pid, take,
                   PSQ_OutcomeName(r));
}

void PF_psq_kill(void)
{
    int          pid;
    psq_result_t r;

    PSQ_Gate();
    pid = PSQ_FloatToInt(G_FLOAT(OFS_PARM0));
    r = psq_policy_kill(pid);
    /* Every real signal attempt is logged, whatever psq_debug says. */
    if (psq_debug.value || r.outcome == PSQ_OUT_ACTED)
        Con_Printf("psq: kill pid %d: %s%s\n", pid, PSQ_OutcomeName(r),
                   (r.outcome == PSQ_OUT_ACTED && r.act == PSD_ACT_DONE)
                       ? " (SIGTERM sent)" : "");
}

void PF_psq_present(void)
{
    PSQ_Gate();
    G_FLOAT(OFS_RETURN) =
        psq_policy_present(PSQ_FloatToInt(G_FLOAT(OFS_PARM0)));
}

void PF_psq_tier_class(void)
{
    G_INT(OFS_RETURN) = PR_SetEngineString(
        psq_policy_tier_class(PSQ_FloatToInt(G_FLOAT(OFS_PARM0))));
}

/* ------------------------------------------------------------ arena grid */

/* The grid an arena entity describes (psq_grid.h); 0 if it is not one. */
static int PSQ_GridOf(edict_t *ed, psq_grid *g)
{
    eval_t *cols, *rows, *cell;

    memset(g, 0, sizeof(*g));
    if (ed == NULL || ed == sv.edicts || ed->free)
        return 0;
    cols = GetEdictFieldValue(ed, "psq_cols");
    rows = GetEdictFieldValue(ed, "psq_rows");
    cell = GetEdictFieldValue(ed, "psq_cell");
    if (cols == NULL || rows == NULL || cell == NULL)
        return 0;
    return psq_grid_from_keys(g, ed->v.origin, cols->_float, rows->_float,
                              cell->_float);
}

/* Every solid entity (monsters and players; not triggers, corpses or the
 * world) as an occupant box; players keep PSQ_GRID_PLAYER_PAD clear. */
static psq_grid_box *psq_occupants;
static int psq_occupants_size;

static int PSQ_GridOccupants(void)
{
    int i, k, n = 0;

    if (psq_occupants_size < sv.num_edicts)
    {
        psq_grid_box *p = (psq_grid_box *) realloc(psq_occupants,
            (size_t) sv.num_edicts * sizeof(*p));
        if (p == NULL)
            return -1;
        psq_occupants = p;
        psq_occupants_size = sv.num_edicts;
    }
    for (i = 1; i < sv.num_edicts; i++)
    {
        edict_t *ed = EDICT_NUM(i);

        if (ed->free)
            continue;
        if (ed->v.solid != SOLID_SLIDEBOX && ed->v.solid != SOLID_BBOX)
            continue;
        for (k = 0; k < 3; k++)
        {
            psq_occupants[n].mins[k] = ed->v.origin[k] + ed->v.mins[k];
            psq_occupants[n].maxs[k] = ed->v.origin[k] + ed->v.maxs[k];
        }
        psq_occupants[n].pad =
            ((int) ed->v.flags & FL_CLIENT) ? PSQ_GRID_PLAYER_PAD : 0;
        n++;
    }
    return n;
}

/* #1209 float(entity grid, float start, float skip) psq_grid_cell: the
 * skip-th free cell scanning from start (psq_grid_pick), or -1 (no grid,
 * or not that many free cells). Reads entities only; acts on nothing. */
void PF_psq_grid_cell(void)
{
    psq_grid g;
    int start = PSQ_FloatToInt(G_FLOAT(OFS_PARM1));
    int skip = PSQ_FloatToInt(G_FLOAT(OFS_PARM2));
    int n;

    G_FLOAT(OFS_RETURN) = -1;
    if (!PSQ_GridOf(G_EDICT(OFS_PARM0), &g))
        return;
    n = PSQ_GridOccupants();
    if (n < 0)
        return;
    G_FLOAT(OFS_RETURN) = psq_grid_pick(&g, psq_occupants, n, start, skip);
}

/* #1210 vector(entity grid, float cell) psq_grid_spot: the floor centre of
 * the cell; the grid entity's origin if either is not valid. */
void PF_psq_grid_spot(void)
{
    psq_grid g;
    edict_t *ed = G_EDICT(OFS_PARM0);
    float c[3];

    VectorCopy(ed->v.origin, c);
    if (PSQ_GridOf(ed, &g))
        psq_grid_centre(&g, PSQ_FloatToInt(G_FLOAT(OFS_PARM1)), c);
    VectorCopy(c, G_VECTOR(OFS_RETURN));
}
