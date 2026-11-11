/*
 * psQuake -- headless end-to-end check of the process-monster builtins.
 *
 * Built only into psq_headless_test: the QuakeSpasm engine, psQuake's glue
 * and policy, the compiled progs.dat, and psproc_fake. No native process
 * backend is linked (psproc_native_actions_unlinked checks the binary with
 * nm), so nothing here can renice or signal a real process. The engine runs
 * as a dedicated loopback server on LibreQuake's lq_e1m1 with psq_testarena,
 * and this driver feeds the fake backend one snapshot per poll, drives hits
 * through the game's own T_Damage, and checks what the QuakeC controller and
 * the policy did:
 *
 *   poll 1   five processes -> five monsters of the expected classes
 *   poll 2   wound pid 2001 (it survives), kill pid 2002
 *   poll 3   the wound reniced only under renice/live policy; the kill
 *            signalled only under live policy; 2002 stays a corpse and is
 *            not respawned
 *   poll 3   2003 exits -> its monster teleports out and leaves the total;
 *            parent 2005 forks 8 children -> a Scrag swarm (psq_pid -1)
 *   poll 4   2001 grew -> after the up-hold its monster morphs to an ogre
 *   poll 4   pid 2004 is handed to another process; a hit on its monster is
 *            refused by the identity check, and the monster teleports out
 *   poll 6   the swarm is killed; the policy's swarm count returns to 0
 *   poll 7   `save` is refused, with the process rule's one line; a grunt
 *            (pid 2004) hits the ogre (pid 2001): no damage with
 *            psq_infighting 0 (default), damage and a renice with
 *            psq_infighting 1 (the psq_headless_infight run)
 *   last     QuakeC's localcmd cannot raise the kill policy or turn
 *            psq_infighting on
 *
 * With +psq_test_newgame instead of +map, it runs New Game (the arena when
 * the data has it, else exit 77) and the arena scenario below
 * (on_refresh_arena): the skill arches, grid cells, wound, kill, retire, swarm, the grid-full
 * fallback, the save refusal, the exit's parms reset and lq_e1m1 after it.
 *
 * With -dedicated 2 +deathmatch 0 (two clients: not single player) the gate stays closed:
 * the controller polls, but nothing is listed, spawned or acted on.
 *
 * Exits 0 when every check passes (via `quit`), 1 on any failure
 * (Sys_Error), 77 when LibreQuake data is missing (skip).
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#define main psq_engine_main
#include "main_sdl.c"
#undef main

#include "psq_engine.h"
#include "psq_arena.h"
#include "psq_grid.h"
#include "psq_policy.h"
#include "proc_fake.h"
#include "psproc_options.h"

#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#define UID 501

static int checks_run;
static int checks_failed;
static int gate_closed_run;
static int stray_run;        /* -psqtest-stray: a controller on a map that may not have one */
static int untrusted_run;    /* -psqtest-untrusted: a server or demo stuffed commands first */
static int want_testarena;   /* +psq_testarena 1 on the command line */
static int hazards_run;      /* -psqtest-hazards: each hazard kills a monster */
static int dm_run;           /* -psqtest-dm: a deathmatch map */
static float policy_before_qc;
static int last_refresh;
static int finished;
static float ogre_health;
static int hit_by_ran;            /* psq_test_hit_by found both monsters */
static edict_t *hit_by_attacker;
static int kills_before_fight;
static int renice_before_fight;
static int infighting;   /* psq_infighting, read when the scenario starts */

#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        checks_run++;                                                     \
        if (cond) {                                                       \
            Sys_Printf("psq-test PASS: ");                                \
        } else {                                                          \
            checks_failed++;                                              \
            Sys_Printf("psq-test FAIL: ");                                \
        }                                                                 \
        Sys_Printf(__VA_ARGS__);                                          \
        Sys_Printf("\n");                                                 \
    } while (0)

/* -------------------------------------------------------- fake processes */

static psd_proc_t mk(int pid, int ppid, unsigned int uid,
                     unsigned long long start, const char *name,
                     unsigned long long mb)
{
    psd_proc_t p;

    memset(&p, 0, sizeof(p));
    p.pid = pid;
    p.ppid = ppid;
    p.uid = uid;
    p.start_token = start;
    p.footprint = mb << 20;
    q_strlcpy(p.name, name, sizeof(p.name));
    return p;
}

/* step 1: five processes. step 2: 2003 exits, 2001 grows, 2005 forks 8.
 * step 3: as step 2, but pid 2004 now belongs to another process. */
static int snapshot(int step, psd_proc_t *out)
{
    int n = 0;
    int i;

    out[n++] = mk(2001, 0, UID, 101, "editor", step >= 2 ? 300 : 10);
    out[n++] = mk(2002, 0, UID, 102, "browser", 300);
    if (step < 2)
        out[n++] = mk(2003, 0, UID, 103, "indexer", 100);
    if (step < 3)
        out[n++] = mk(2004, 0, UID, 104, "builder", 30);
    else
        out[n++] = mk(2004, 0, UID, 9104, "intruder", 30);
    out[n++] = mk(2005, 0, UID, 105, "forker", 10);
    if (step >= 2)
        for (i = 0; i < 8; i++)   /* another user's children: counted only */
            out[n++] = mk(2100 + i, 2005, 999, 200 + i, "child", 1);
    return n;
}

static int hazard_snapshot(psd_proc_t *out);

static void push_step(int step)
{
    psd_proc_t procs[32];
    proc_fake_push_snapshot(procs, snapshot(step, procs));
}

void psq_test_install_backend(void)
{
    psd_proc_t procs[32];
    int i;
    int n;

    proc_fake_install(UID);
    n = hazards_run ? hazard_snapshot(procs) : snapshot(1, procs);
    for (i = 0; i < n; i++)
        if (procs[i].pid == (int) getpid())
            Sys_Error("psq-test: a fake pid equals this process's pid");
    proc_fake_push_snapshot(procs, n);
}

/* ----------------------------------------------------------- entity view */

static float pid_of(edict_t *e)
{
    eval_t *v = GetEdictFieldValue(e, "psq_pid");
    return v ? v->_float : 0;
}

/* Live edicts with psq_pid == pid; the last one's class in *cls. */
static int count_live(float pid, const char **cls)
{
    int i;
    int n = 0;

    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        if (e->free || pid_of(e) != pid || e->v.health <= 0)
            continue;
        n++;
        if (cls)
            *cls = PR_GetString(e->v.classname);
    }
    return n;
}

static int count_live_process_monsters(void)
{
    int i;
    int n = 0;

    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        if (!e->free && pid_of(e) > 1 && e->v.health > 0)
            n++;
    }
    return n;
}

static int count_tag(float tag)
{
    int i;
    int n = 0;

    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        if (!e->free && pid_of(e) == tag)
            n++;
    }
    return n;
}

/* What the live edicts tagged pid added to total_monsters. */
static float counted_sum(float pid)
{
    int i;
    float n = 0;

    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        eval_t  *c = GetEdictFieldValue(e, "psq_counted");
        if (!e->free && pid_of(e) == pid && e->v.health > 0 && c)
            n += c->_float;
    }
    return n;
}

static int is_class(float pid, const char *want)
{
    const char *cls = "(none)";
    int n = count_live(pid, &cls);
    Sys_Printf("psq-test: pid %d -> %d live, %s\n", (int) pid, n, cls);
    return n == 1 && !strcmp(cls, want);
}

/* --------------------------------------------------------------- actions */

static dfunction_t *find_function(const char *name)
{
    int i;

    for (i = 0; i < progs->numfunctions; i++)
        if (!strcmp(PR_GetString(pr_functions[i].s_name), name))
            return &pr_functions[i];
    return NULL;
}

/* T_Damage(e, by, by, dmg); by is world for a hit from nowhere in
 * particular (the player's stand-in: no monster attacker). */
static void hit_by(edict_t *e, edict_t *by, float dmg)
{
    dfunction_t *f = find_function("T_Damage");

    if (!f)
        Sys_Error("psq-test: no T_Damage in progs");
    pr_global_struct->time = sv.time;
    pr_global_struct->self = EDICT_TO_PROG(sv.edicts);
    pr_global_struct->other = EDICT_TO_PROG(sv.edicts);
    G_INT(OFS_PARM0) = EDICT_TO_PROG(e);
    G_INT(OFS_PARM1) = EDICT_TO_PROG(by);
    G_INT(OFS_PARM2) = EDICT_TO_PROG(by);
    G_FLOAT(OFS_PARM3) = dmg;
    PR_ExecuteProgram(f - pr_functions);
}

static void hit(edict_t *e, float dmg)
{
    hit_by(e, sv.edicts, dmg);
}

static edict_t *first_live(float pid)
{
    int i;

    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        if (!e->free && pid_of(e) == pid && e->v.health > 0 && e->v.takedamage)
            return e;
    }
    return NULL;
}

/* psq_test_hit_by <pid> <attacker pid> <damage>: one monster hits another. */
static void PSQ_Test_HitBy_f(void)
{
    edict_t *t = first_live(Q_atof(Cmd_Argv(1)));
    edict_t *a = first_live(Q_atof(Cmd_Argv(2)));
    float dmg = Q_atof(Cmd_Argv(3));

    if (!t || !a) {
        Sys_Printf("psq-test: hit_by: no live monster for %s or %s\n",
                   Cmd_Argv(1), Cmd_Argv(2));
        return;
    }
    Sys_Printf("psq-test: %s (pid %s) hits %s (pid %s, health %g) for %g\n",
               PR_GetString(a->v.classname), Cmd_Argv(2),
               PR_GetString(t->v.classname), Cmd_Argv(1), t->v.health, dmg);
    hit_by_ran++;
    hit_by_attacker = a;
    hit_by(t, a, dmg);
}

/* QuakeC's changelevel (#70), as a level exit's trigger calls it with the
 * map's own "map" key. */
static void qc_changelevel(const char *map)
{
    G_INT(OFS_PARM0) = PR_SetEngineString(map);
    pr_builtins[70]();
}

/* psq_test_hit <pid> <damage>: every live edict tagged pid takes damage. */
static void PSQ_Test_Hit_f(void)
{
    float pid = Q_atof(Cmd_Argv(1));
    float dmg = Q_atof(Cmd_Argv(2));
    int i;
    int n = 0;

    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        if (e->free || pid_of(e) != pid || e->v.health <= 0 || !e->v.takedamage)
            continue;
        Sys_Printf("psq-test: hit %s (pid %d, health %g) for %g\n",
                   PR_GetString(e->v.classname), (int) pid, e->v.health, dmg);
        hit(e, dmg);
        n++;
    }
    if (!n)
        Sys_Printf("psq-test: hit: nothing live with pid %d\n", (int) pid);
}

/* ------------------------------------------------------------- scenario */

/* Remove the saves a run checks for, so an earlier run's file never passes. */
static void forget_saves(void)
{
    static const char *const names[] = {"psqtest", "psqarena_try", "psq_campaign", NULL};
    char path[MAX_OSPATH];
    int i;

    for (i = 0; names[i]; i++) {
        q_snprintf(path, sizeof(path), "%s/%s.sav", com_gamedir, names[i]);
        unlink(path);
    }
}

/* QuakeC's localcmd (#46), as a progs would call it. */
static void qc_localcmd(const char *text)
{
    G_INT(OFS_PARM0) = PR_SetEngineString(text);
    pr_builtins[46]();
}

static int log_count(const char *s);
static float field_of(edict_t *e, const char *name);

static void finish(void)
{
    char path[MAX_OSPATH];
    struct stat st;

    q_snprintf(path, sizeof(path), "%s/psqtest.sav", com_gamedir);
    CHECK(stat(path, &st) != 0, "save was refused: no %s", path);
    CHECK(PSQ_SaveRule() == PSQ_SAVE_REFUSED_PROCESSES,
          "and it was psQuake's process rule that refused it");
    CHECK(log_count("Can't save here") == 1 && log_count("Can't save in the psQuake arena") == 0,
          "one refusal message for one refused save (%d process, %d arena)",
          log_count("Can't save here"), log_count("Can't save in the psQuake arena"));
    CHECK(PSQ_CvarSetRefused("psq_killpolicy") && PSQ_CvarSetRefused("PSQ_AllUsers")
          && PSQ_CvarSetRefused("psq_testarena") && PSQ_CvarSetRefused("psq_infighting")
          && !PSQ_CvarSetRefused("psq_cap"),
          "QuakeC cvar_set may not touch the kill policy, all-users, the test arena or "
          "infighting");
    /* A third-party map's exit whose "map" key carries console commands:
     * changelevel must not run them (the session is still trusted here,
     * so nothing else would stop them). */
    {
        int policy_before = psproc_kill_policy;
        float infight_before = Cvar_VariableValue("psq_infighting");

        qc_changelevel("lq_e1m1;psq_killpolicy 2;psq_allusers 1;psq_infighting 1");
        Cbuf_Execute();
        CHECK(psproc_kill_policy == policy_before && psproc_all_users == 0
              && Cvar_VariableValue("psq_infighting") == infight_before && sv.active
              && !svs.changelevel_issued,
              "a level exit naming \"map;commands\" ran none of them (policy %d, was %d; "
              "all users %d; infighting %g, was %g; server %s)",
              psproc_kill_policy, policy_before, psproc_all_users,
              Cvar_VariableValue("psq_infighting"), infight_before,
              sv.active ? "up" : "down");
    }
    /* Last, as it ends the test arena for the session: a progs tries to
     * arm SIGTERM through the console buffer, in two fragments. */
    policy_before_qc = psproc_kill_policy;
    qc_localcmd("psq_kill");
    qc_localcmd("policy 2\npsq_allusers 1\n");
    Cbuf_Execute();
    CHECK(psproc_kill_policy == policy_before_qc && psproc_all_users == 0,
          "QuakeC localcmd raised nothing (policy %d, was %g; all users %d)",
          psproc_kill_policy, policy_before_qc, psproc_all_users);
    /* The session is untrusted now: infighting can still be turned off,
     * never on. */
    if (infighting) {
        Cbuf_AddText("psq_infighting 0\n");
        Cbuf_Execute();
        CHECK(Cvar_VariableValue("psq_infighting") == 0,
              "untrusted: psq_infighting can still be turned off (%g)",
              Cvar_VariableValue("psq_infighting"));
    }
    qc_localcmd("psq_infighting 1\n");
    Cbuf_Execute();
    CHECK(Cvar_VariableValue("psq_infighting") == 0,
          "untrusted: QuakeC localcmd did not turn psq_infighting on (%g)",
          Cvar_VariableValue("psq_infighting"));
    /* The kill mode: the console never sets it, and once untrusted even
     * the options page's confirmed warning cannot raise it. */
    {
        int before = PSQ_KillMode();
        int r;

        Cbuf_AddText(va("psq_killpolicy %d\n", before == PSD_KILL_LIVE ? 0 : 2));
        Cbuf_Execute();
        CHECK(PSQ_KillMode() == before && psproc_kill_policy == before
              && Cvar_VariableValue("psq_killpolicy") == (float) before,
              "the console cannot set the kill mode (%d, was %d; mirror %g)",
              PSQ_KillMode(), before, Cvar_VariableValue("psq_killpolicy"));
        if (before < PSD_KILL_LIVE) {
            r = PSQ_SetKillMode(PSD_KILL_LIVE, true);
            CHECK(r == PSQ_KILLMODE_UNTRUSTED && PSQ_KillMode() == before
                  && psproc_kill_policy == before,
                  "untrusted: the page's confirmed live kill is refused (%d; mode %d)",
                  r, PSQ_KillMode());
        }
        if (before > PSD_KILL_SIMULATE) {
            r = PSQ_SetKillMode(PSD_KILL_SIMULATE, false);
            CHECK(r == PSQ_KILLMODE_CHANGED && psproc_kill_policy == PSD_KILL_SIMULATE,
                  "untrusted: the page can still lower the kill mode (%d; mode %d)",
                  r, psproc_kill_policy);
        }
    }
    {
        int tagged = count_tag(-3) + count_live_process_monsters();
        int i;
        int left = 0;

        PSQ_AfterLoad();
        for (i = 1; i < sv.num_edicts; i++)
            if (!EDICT_NUM(i)->free && pid_of(EDICT_NUM(i)) != 0)
                left++;
        CHECK(tagged > 0 && left == 0,
              "after a load no edict keeps a process tag (%d tagged, %d left)",
              tagged, left);
    }
    Sys_Printf("psq-test: %d checks, %d failed\n", checks_run, checks_failed);
    if (checks_failed)
        Sys_Error("psq-test: %d of %d checks failed", checks_failed, checks_run);
    Sys_Printf("psq-test: ALL PASS\n");
    Cbuf_AddText("quit\n");
}

static void on_refresh(int k)
{
    int policy = psproc_kill_policy;
    int want_renice = policy != PSD_KILL_SIMULATE;
    int want_kill = policy == PSD_KILL_LIVE;
    static float total_before_exit;

    Sys_Printf("psq-test: --- refresh %d at %.1f (policy %d, total_monsters %g, "
               "killed %g) ---\n", k, sv.time, policy,
               pr_global_struct->total_monsters, pr_global_struct->killed_monsters);
    switch (k) {
    case 1:
        CHECK(!strcmp(sv.name, "lq_e1m1"), "the scenario runs on lq_e1m1 (%s)", sv.name);
        CHECK(want_testarena && Cvar_VariableValue("psq_testarena") == 1,
              "with psq_testarena 1 from the command line");
        forget_saves();
        infighting = Cvar_VariableValue("psq_infighting") != 0;
        push_step(1);
        break;
    case 2:
        CHECK(count_live_process_monsters() == 5, "five process monsters spawned");
        CHECK(is_class(2001, "monster_dog"), "10 MB editor is a dog");
        CHECK(is_class(2002, "monster_ogre"), "300 MB browser is an ogre");
        CHECK(is_class(2003, "monster_knight"), "100 MB indexer is a knight");
        CHECK(is_class(2004, "monster_army"), "30 MB builder is a grunt");
        CHECK(is_class(2005, "monster_dog"), "10 MB forker is a dog");
        {
            int i, off = 0;
            for (i = 1; i < sv.num_edicts; i++) {
                edict_t *e = EDICT_NUM(i);
                if (!e->free && pid_of(e) > 1 && e->v.health > 0
                    && field_of(e, "psq_gridcell") == -1)
                    off++;
            }
            CHECK(off == 5, "no arena grid on this map: all five placed near the "
                  "player or the spawn spots (%d)", off);
        }
        Cbuf_AddText("psq_test_hit 2001 5\npsq_test_hit 2002 5000\n");
        push_step(2);
        break;
    case 3:
        CHECK(proc_fake_renice_count() == (want_renice ? 1 : 0),
              "wound of a survivor: %d renice (policy %d)",
              proc_fake_renice_count(), policy);
        if (want_renice)
            CHECK(proc_fake_last_renice_pid() == 2001
                  && proc_fake_last_renice_delta() == PSQ_WOUND_NICE,
                  "renice went to pid 2001 by %d", PSQ_WOUND_NICE);
        CHECK(proc_fake_kill_count() == (want_kill ? 1 : 0),
              "kill: %d SIGTERM (policy %d)", proc_fake_kill_count(), policy);
        if (want_kill)
            CHECK(proc_fake_last_kill_pid() == 2002, "SIGTERM went to pid 2002");
        CHECK(count_live(2002, NULL) == 0, "the killed monster is dead");
        CHECK(count_tag(-3) == 1, "it left a corpse tagged PSQ_CORPSE");
        CHECK(pr_global_struct->killed_monsters == 1, "kill counted");
        total_before_exit = pr_global_struct->total_monsters;
        push_step(2);
        break;
    case 4:
        CHECK(count_live(2003, NULL) == 0, "exited process: monster teleported out");
        {
            /* The same think also teleported the swarm in, which adds. */
            float swarm = counted_sum(-1);
            CHECK(pr_global_struct->total_monsters == total_before_exit - 1 + swarm,
                  "it left the level total (%g - 1 + %g swarm = %g)",
                  total_before_exit, swarm, pr_global_struct->total_monsters);
        }
        CHECK(count_live(2002, NULL) == 0, "killed process is not respawned");
        CHECK(count_live(-1, NULL) > 0 && count_live(-1, NULL) <= PSQ_SWARM_PER_BURST,
              "fork burst: %d Scrags", count_live(-1, NULL));
        CHECK(psq_policy_swarm_live() == count_live(-1, NULL),
              "policy swarm count %d matches the live swarm",
              psq_policy_swarm_live());
        {
            const char *cls = "";
            count_live(-1, &cls);
            CHECK(!strcmp(cls, "monster_wizard"), "swarm members are Scrags");
        }
        /* pid 2004 changes hands before the next poll; hit its monster. */
        {
            psd_proc_t procs[32];
            int n = snapshot(3, procs);
            proc_fake_set_live(procs, n);
        }
        Cbuf_AddText("psq_test_hit 2004 1\n");
        push_step(3);
        break;
    case 5:
        CHECK(is_class(2001, "monster_ogre"), "grown editor morphed to an ogre");
        CHECK(proc_fake_renice_count() == (want_renice ? 1 : 0),
              "the hit on reused pid 2004 reniced nothing");
        CHECK(proc_fake_kill_count() == (want_kill ? 1 : 0), "and signalled nothing");
        push_step(3);
        break;
    case 6:
        CHECK(count_live(2004, NULL) == 0, "reused pid: old monster teleported out");
        Cbuf_AddText("psq_test_hit -1 1000\n");
        break;
    case 7:
        CHECK(count_live(-1, NULL) == 0, "swarm killed");
        CHECK(psq_policy_swarm_live() == 0, "policy swarm count back to 0");
        CHECK(proc_fake_renice_count() == (want_renice ? 1 : 0)
              && proc_fake_kill_count() == (want_kill ? 1 : 0),
              "swarm deaths acted on nothing");
        Cbuf_AddText("save psqtest\n");
        /* Last, since it can start a fight: the new grunt for pid 2004
         * hits the editor's ogre. */
        {
            edict_t *ogre = first_live(2001);
            ogre_health = ogre ? ogre->v.health : -1;
            renice_before_fight = proc_fake_renice_count();
        }
        Cbuf_AddText("psq_test_hit_by 2001 2004 5\n");
        break;
    case 8:
        {
            edict_t *ogre = first_live(2001);
            float now_health = ogre ? ogre->v.health : -1;

            CHECK(hit_by_ran == 1 && hit_by_attacker != NULL,
                  "the grunt (pid 2004) and the ogre (pid 2001) were both there to fight");
            if (infighting) {
                CHECK(ogre_health > 0 && now_health < ogre_health,
                      "psq_infighting 1: the grunt's hit hurt the ogre (%g -> %g)",
                      ogre_health, now_health);
                CHECK(want_renice ? proc_fake_renice_count() > renice_before_fight
                                    && proc_fake_last_renice_pid() == 2001
                                  : proc_fake_renice_count() == renice_before_fight,
                      "psq_infighting 1: and the ogre's process (2001) was %s (%d -> %d, "
                      "last %d)", want_renice ? "reniced" : "left alone (simulate)",
                      renice_before_fight, proc_fake_renice_count(),
                      proc_fake_last_renice_pid());
                /* Now a lethal hit: the grunt kills the ogre. */
                kills_before_fight = proc_fake_kill_count();
                Cbuf_AddText("psq_test_hit_by 2001 2004 1000\n");
            } else {
                CHECK(ogre_health > 0 && now_health == ogre_health,
                      "psq_infighting 0: the grunt's hit did no damage (%g -> %g)",
                      ogre_health, now_health);
                CHECK(proc_fake_renice_count() == renice_before_fight,
                      "psq_infighting 0: and reniced nothing");
                CHECK(ogre && hit_by_attacker && PROG_TO_EDICT(ogre->v.enemy) == hit_by_attacker,
                      "psq_infighting 0: the blocked hit still turned the ogre on the grunt");
            }
        }
        break;
    case 9:
        if (infighting) {
            CHECK(hit_by_ran == 2 && first_live(2001) == NULL,
                  "psq_infighting 1: the grunt killed the ogre");
            CHECK(want_kill ? proc_fake_kill_count() == kills_before_fight + 1
                              && proc_fake_last_kill_pid() == 2001
                            : proc_fake_kill_count() == kills_before_fight,
                  "psq_infighting 1: and the ogre's process (2001) was %s (%d -> %d)",
                  want_kill ? "killed" : "not killed (kill policy)", kills_before_fight,
                  proc_fake_kill_count());
        }
        break;
    case 10:
        finished = 1;
        finish();
        break;
    }
}

/* ------------------------------------------------------- arena scenario */

/* New Game in the arena (psq_test_newgame), psq_testarena off:
 *
 *   poll 1   six processes -> six monsters, each teleported onto its own
 *            free grid cell (hull 2 classes in a whole cell), cells logged
 *   poll 2   wound pid 3001 (it survives), kill pid 3002 (policy-gated)
 *   poll 3   3003 exits -> teleports out and leaves the level total;
 *            parent 3005 forks 8 children -> a Scrag swarm
 *   poll 4   every free cell is blocked but the last grid's; new pid 3007
 *            (poll 5) must land there, numbered after every other grid's
 *            cells (checked at poll 6: the pick passes the full grids)
 *   poll 6   the last grid's free cells are blocked too; new pid 3008
 *            (poll 7) gets no monster: the fallback rings the player start,
 *            inside the start room's safe zone (checked at poll 8); once
 *            the cells free, it arrives on one (poll 9)
 *   poll 8   the swarm is killed
 *   then     save refused (the arena rule, one message), the exit's parms
 *            reset, changelevel lq_e1m1: no process monster or controller
 *            there, save allowed, and the campaign keeps its parms.
 */

static int arena_run;
static const char *newgame_map;
static int blockers[512];
static int nblockers;

/* step 1: six processes. step 2: 3003 exits, 3005 forks 8 children.
 * step 3: as step 2, plus 3007. */
static int arena_snapshot(int step, psd_proc_t *out)
{
    int n = 0;
    int i;

    out[n++] = mk(3001, 0, UID, 301, "editor", 10);      /* dog */
    out[n++] = mk(3002, 0, UID, 302, "browser", 300);    /* ogre, hull 2 */
    if (step < 2)
        out[n++] = mk(3003, 0, UID, 303, "indexer", 100);   /* knight */
    out[n++] = mk(3004, 0, UID, 304, "database", 2000);  /* shambler, hull 2 */
    out[n++] = mk(3005, 0, UID, 305, "forker", 10);
    out[n++] = mk(3006, 0, UID, 306, "renderer", 800);   /* fiend, hull 2 */
    if (step >= 2)
        for (i = 0; i < 8; i++)
            out[n++] = mk(3100 + i, 3005, 999, 400 + i, "child", 1);
    if (step >= 3)
        out[n++] = mk(3007, 0, UID, 307, "latecomer", 30);   /* grunt */
    if (step >= 4)
        out[n++] = mk(3008, 0, UID, 308, "lastcomer", 30);   /* grunt */
    return n;
}

static void arena_push(int step)
{
    psd_proc_t procs[32];
    proc_fake_push_snapshot(procs, arena_snapshot(step, procs));
}

static float field_of(edict_t *e, const char *name)
{
    eval_t *v = GetEdictFieldValue(e, name);
    return v ? v->_float : 0;
}

/* The one psq_arena entity, or NULL (also when there are several). */
static edict_t *arena_controller(int *count)
{
    edict_t *found = NULL;
    int i;
    int n = 0;

    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        if (!e->free && !strcmp(PR_GetString(e->v.classname), "psq_arena")) {
            found = e;
            n++;
        }
    }
    if (count)
        *count = n;
    return n == 1 ? found : NULL;
}

static int arena_grid(psq_grid *g)
{
    edict_t *c = arena_controller(NULL);
    if (!c)
        return 0;
    return psq_grid_from_keys(g, c->v.origin, field_of(c, "psq_cols"),
                              field_of(c, "psq_rows"), field_of(c, "psq_cell"));
}

/* The arena's grids, in the order QuakeC numbers psq_gridcell across them
 * (psq.qc psq_gridspot): the controller's own, then each psq_grid entity in
 * edict order; a grid the engine would refuse counts no cells and is left
 * out. Returns how many, into arena_grids_g. */
#define MAX_ARENA_GRIDS 16
static psq_grid arena_grids_g[MAX_ARENA_GRIDS];

static int arena_grids(void)
{
    edict_t *c = arena_controller(NULL);
    int i, n = 0;

    if (!c)
        return 0;
    if (psq_grid_from_keys(&arena_grids_g[n], c->v.origin, field_of(c, "psq_cols"),
                           field_of(c, "psq_rows"), field_of(c, "psq_cell")))
        n++;
    for (i = 1; i < sv.num_edicts && n < MAX_ARENA_GRIDS; i++) {
        edict_t *e = EDICT_NUM(i);
        if (!e->free && !strcmp(PR_GetString(e->v.classname), "psq_grid")
            && psq_grid_from_keys(&arena_grids_g[n], e->v.origin, field_of(e, "psq_cols"),
                                  field_of(e, "psq_rows"), field_of(e, "psq_cell")))
            n++;
    }
    return n;
}

static int arena_total_cells(int ngrids)
{
    int k, n = 0;
    for (k = 0; k < ngrids; k++)
        n += psq_grid_cells(&arena_grids_g[k]);
    return n;
}

/* The grid holding cell number idx (counted across the grids), and that
 * cell's floor centre in c; NULL when idx is out of range. */
static const psq_grid *arena_cell(int ngrids, int idx, float c[3])
{
    int k;
    if (idx < 0)
        return NULL;
    for (k = 0; k < ngrids; k++) {
        int n = psq_grid_cells(&arena_grids_g[k]);
        if (idx < n)
            return psq_grid_centre(&arena_grids_g[k], idx, c) ? &arena_grids_g[k] : NULL;
        idx -= n;
    }
    return NULL;
}

/* Every tagged entity's contribution to total_monsters: live process
 * monsters, swarm members and corpses (a kill stays in the total). */
static float counted_all(void)
{
    int i;
    float n = 0;

    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        if (!e->free && pid_of(e) != 0)
            n += field_of(e, "psq_counted");
    }
    return n;
}

/* Every live process monster placed on the grid has its own cell (no other
 * live monster was given it); at_spawn, before any has moved, also its x and
 * y are that cell's centre, it stands on the floor, its box fits inside the
 * cell and it is not in a wall. Logs each. */
static int arena_cells_ok(const char *when, int expect_gridded, int at_spawn)
{
    static int used[MAX_ARENA_GRIDS * PSQ_GRID_MAX_DIM * PSQ_GRID_MAX_DIM];
    int i;
    int ok = 1;
    int gridded = 0;
    int ngrids = arena_grids();

    if (ngrids == 0)
        return 0;
    memset(used, 0, sizeof(used));
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        float cell, c[3];
        int idx;
        const psq_grid *gp;

        if (e->free || pid_of(e) <= 1 || e->v.health <= 0)
            continue;
        cell = field_of(e, "psq_gridcell");
        idx = (int) cell;
        if (cell < 0)
            continue;   /* placed by the fallback; checked by its caller */
        gridded++;
        gp = arena_cell(ngrids, idx, c);
        if (cell != (float) idx || !gp) {
            Sys_Printf("psq-test: %s: pid %d has bad cell %g\n", when, (int) pid_of(e), cell);
            ok = 0;
            continue;
        }
        Sys_Printf("psq-test: %s: pid %d %s (%gx%g) in cell %d (grid %d, floor %d) "
                   "at (%g %g %g), centre (%g %g)\n", when, (int) pid_of(e),
                   PR_GetString(e->v.classname), e->v.size[0], e->v.size[1],
                   idx, (int) (gp - arena_grids_g), (int) (c[2]),
                   e->v.origin[0], e->v.origin[1], e->v.origin[2], c[0], c[1]);
        if (used[idx]) {
            Sys_Printf("psq-test: %s: cell %d holds two monsters\n", when, idx);
            ok = 0;
        }
        used[idx] = 1;
        if (!at_spawn)
            continue;   /* monsters walk off their cells once they start */
        if (fabsf(e->v.origin[0] - c[0]) > 0.5f || fabsf(e->v.origin[1] - c[1]) > 0.5f
            || fabsf(e->v.origin[2] + e->v.mins[2] - c[2]) > 1.0f) {
            Sys_Printf("psq-test: %s: pid %d is not on its cell's centre\n", when, (int) pid_of(e));
            ok = 0;
        }
        if (e->v.origin[0] + e->v.mins[0] < c[0] - gp->cell / 2
            || e->v.origin[0] + e->v.maxs[0] > c[0] + gp->cell / 2
            || e->v.origin[1] + e->v.mins[1] < c[1] - gp->cell / 2
            || e->v.origin[1] + e->v.maxs[1] > c[1] + gp->cell / 2) {
            Sys_Printf("psq-test: %s: pid %d's box leaves its cell\n", when, (int) pid_of(e));
            ok = 0;
        }
        if (SV_PointContents(e->v.origin) == CONTENTS_SOLID) {
            Sys_Printf("psq-test: %s: pid %d is in a wall\n", when, (int) pid_of(e));
            ok = 0;
        }
    }
    if (gridded != expect_gridded) {
        Sys_Printf("psq-test: %s: %d monsters on the grid, expected %d\n", when,
                   gridded, expect_gridded);
        ok = 0;
    }
    return ok;
}

/* The ladder and swarm classes were precached while the arena loaded. */
static const char *class_model(const char *cls)
{
    static const char *const map[][2] = {
        {"monster_dog", "progs/dog.mdl"}, {"monster_army", "progs/soldier.mdl"},
        {"monster_enforcer", "progs/enforcer.mdl"}, {"monster_knight", "progs/knight.mdl"},
        {"monster_ogre", "progs/ogre.mdl"}, {"monster_hell_knight", "progs/hknight.mdl"},
        {"monster_demon1", "progs/demon.mdl"}, {"monster_shalrath", "progs/shalrath.mdl"},
        {"monster_shambler", "progs/shambler.mdl"}, {"monster_wizard", "progs/wizard.mdl"},
    };
    int i;
    for (i = 0; i < (int) (sizeof(map) / sizeof(map[0])); i++)
        if (!strcmp(map[i][0], cls))
            return map[i][1];
    return NULL;
}

static int model_precached(const char *model)
{
    int i;
    for (i = 0; i < MAX_MODELS && sv.model_precache[i]; i++)
        if (!strcmp(sv.model_precache[i], model))
            return 1;
    return 0;
}

/* Block every free cell (adding to the blockers already down), except those
 * of grid keep (-1: keep none). */
static void block_free_cells(int keep)
{
    int i;
    int ngrids = arena_grids();
    int total = arena_total_cells(ngrids);
    int before = nblockers;

    for (i = 0; i < total && nblockers < (int) (sizeof(blockers) / sizeof(blockers[0])); i++) {
        int idx = i;
        float c[3];
        edict_t *e;
        int j, taken = 0;
        const psq_grid *gp = arena_cell(ngrids, idx, c);

        if (keep >= 0 && gp == &arena_grids_g[keep])
            continue;

        /* Leave a cell a monster already holds. */
        for (j = 1; j < sv.num_edicts && !taken; j++) {
            edict_t *m = EDICT_NUM(j);
            taken = !m->free && pid_of(m) > 1 && m->v.health > 0
                    && field_of(m, "psq_gridcell") == (float) idx;
        }
        if (taken)
            continue;
        e = ED_Alloc();
        e->v.classname = PR_SetEngineString("psq_test_block");
        e->v.solid = SOLID_BBOX;
        e->v.movetype = MOVETYPE_NONE;
        e->v.mins[0] = -16; e->v.mins[1] = -16; e->v.mins[2] = -24;
        e->v.maxs[0] = 16; e->v.maxs[1] = 16; e->v.maxs[2] = 32;
        e->v.origin[0] = c[0]; e->v.origin[1] = c[1]; e->v.origin[2] = c[2] + 24;
        VectorSubtract(e->v.maxs, e->v.mins, e->v.size);
        SV_LinkEdict(e, false);
        blockers[nblockers++] = NUM_FOR_EDICT(e);
    }
    Sys_Printf("psq-test: blocked %d more free cells%s\n", nblockers - before,
               keep >= 0 ? " (all but the last grid's)" : "");
}

static void unblock_cells(void)
{
    int i;
    for (i = 0; i < nblockers; i++)
        ED_Free(EDICT_NUM(blockers[i]));
    nblockers = 0;
}

/* The start room's skill arches (psq.qc psq_skill_arch): the player's
 * touch sets the skill and says so once (it touches every frame it stands
 * there); another arch says its own, and the first again after it; a
 * monster's touch does nothing; a second player's touch is judged by its
 * own field (the dedicated server has one client slot, so only its log
 * line shows it was told). The
 * player is client 1's edict (no one is connected to the dedicated
 * server), its sprints caught in a buffer of the test's; each line also
 * reaches the log through psq_log. The skill and the client are put back. */
static const char *const arch_lines[4] = {
    "skill: Easy selected. Campaign maps have fewer monsters.",
    "skill: Normal selected. Campaign maps have their standard monsters.",
    "skill: Hard selected. Campaign maps have more monsters.",
    "skill: Nightmare selected. Campaign maps have Hard's monsters; monsters attack without "
        "pause and most rarely flinch, Shamblers cast an extra bolt and Vore balls fly faster." };
#define ARCH_TOLD(s) (arch_lines[s] + 7)   /* what the player is sent: no "skill: " */

static int buf_has(const sizebuf_t *b, const char *s)
{
    return b->cursize > 0 && memmem(b->data, (size_t) b->cursize, s, strlen(s)) != NULL;
}

static void touch_as(edict_t *trigger, edict_t *by)
{
    pr_global_struct->time = sv.time;
    pr_global_struct->self = EDICT_TO_PROG(trigger);
    pr_global_struct->other = EDICT_TO_PROG(by);
    PR_ExecuteProgram(trigger->v.touch);
}

static void arena_check_arches(void)
{
    static const int order[] = { 0, 0, 0, 2, 2, 0, 1, 1, 3, 3, 3 };
    dfunction_t *touch = find_function("psq_skill_arch_touch");
    float skill0 = Cvar_VariableValue("skill");
    edict_t *arch[4] = { NULL, NULL, NULL, NULL };
    client_t *c = &svs.clients[0];
    edict_t *p = c->edict, *p2, *m;
    sizebuf_t keep = c->message;
    static byte told[4096];
    string_t keep_class = p->v.classname;
    int base[4], said[4] = { 0, 0, 0, 0 };
    int i, j, n = 0;

    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        int s;
        if (e->free || strcmp(PR_GetString(e->v.classname), "psq_skill_arch"))
            continue;
        s = atoi(PR_GetString(e->v.message));
        if (s >= 0 && s < 4 && !arch[s]) {
            arch[s] = e;
            n++;
        }
    }
    CHECK(n == 4 && touch, "the start room has an arch for each skill (%d)", n);
    if (n != 4 || !touch)
        return;
    for (i = 0; i < 4; i++)
        CHECK(arch[i]->v.touch == (func_t) (touch - pr_functions)
              && arch[i]->v.solid == SOLID_TRIGGER, "arch %d is a trigger that says its skill", i);
    for (i = 0; i < 4; i++)
        base[i] = log_count(arch_lines[i]);
    p->v.classname = PR_SetEngineString("player");
    c->message.data = told;
    c->message.maxsize = sizeof(told);
    c->message.allowoverflow = true;
    for (i = 0; i < (int) (sizeof(order) / sizeof(order[0])); i++) {
        int s = order[i], ok = 1, fresh = i == 0 || order[i - 1] != s;
        SZ_Clear(&c->message);
        touch_as(arch[s], p);
        said[s] += fresh;
        for (j = 0; j < 4; j++)
            ok = ok && log_count(arch_lines[j]) - base[j] == said[j]
                && buf_has(&c->message, ARCH_TOLD(j)) == (fresh && j == s);
        CHECK(Cvar_VariableValue("skill") == s && ok,
              "touch %d, arch %d: skill %g, the player %s, and each skill said %d %d %d %d "
              "times (want %d %d %d %d)", i, s, Cvar_VariableValue("skill"),
              buf_has(&c->message, ARCH_TOLD(s)) ? "told it" : "not told it",
              log_count(arch_lines[0]) - base[0], log_count(arch_lines[1]) - base[1],
              log_count(arch_lines[2]) - base[2], log_count(arch_lines[3]) - base[3],
              said[0], said[1], said[2], said[3]);
    }
    SZ_Clear(&c->message);
    m = ED_Alloc();
    m->v.classname = PR_SetEngineString("monster_ogre");
    touch_as(arch[0], m);
    CHECK(Cvar_VariableValue("skill") == 3 && log_count(arch_lines[0]) - base[0] == said[0]
          && c->message.cursize == 0,
          "a monster walking through an arch changes nothing (skill %g)",
          Cvar_VariableValue("skill"));
    ED_Free(m);
    /* Another player (coop), in the arch the first is told of: told too. */
    p2 = ED_Alloc();
    p2->v.classname = PR_SetEngineString("player");
    touch_as(arch[3], p2);
    touch_as(arch[3], p2);
    CHECK(log_count(arch_lines[3]) - base[3] == said[3] + 1 && c->message.cursize == 0,
          "a second player's touch tells it once, by its own field (%d)",
          log_count(arch_lines[3]) - base[3]);
    ED_Free(p2);
    {   /* client 1 untold again */
        eval_t *f = GetEdictFieldValue(p, "psq_skill_said");
        if (f)
            f->_float = 0;
    }
    p->v.classname = keep_class;
    c->message = keep;
    Cvar_SetValue("skill", skill0);
}

static void on_refresh_arena(int k)
{
    int policy = psproc_kill_policy;
    int want_renice = policy != PSD_KILL_SIMULATE;
    int want_kill = policy == PSD_KILL_LIVE;
    static float total_before_exit;

    Sys_Printf("psq-test: --- arena refresh %d at %.1f (policy %d, total_monsters %g, "
               "killed %g) ---\n", k, sv.time, policy,
               pr_global_struct->total_monsters, pr_global_struct->killed_monsters);
    switch (k) {
    case 1: {
        psq_grid g;
        int n = 0;
        int t;
        int all = 1;
        const char *cls;

        CHECK(newgame_map && !strcmp(newgame_map, PSQ_ARENA_MAP), "New Game opened %s",
              newgame_map ? newgame_map : "(nothing)");
        CHECK((Cvar_VariableValue("psq_testarena") != 0) == want_testarena,
              "psq_testarena is %g, as the command line asked",
              Cvar_VariableValue("psq_testarena"));
        CHECK(psq_arena_is_map(sv.name), "the arena is running (%s)", sv.name);
        arena_check_arches();
        CHECK(arena_controller(&n) != NULL, "the map's psq_arena is the one controller (%d)", n);
        CHECK(arena_grid(&g) && g.cell == 96,
              "it carries the first grid: %d x %d cells of %g at (%g %g %g)",
              g.cols, g.rows, g.cell, g.origin[0], g.origin[1], g.origin[2]);
        {
            int ng = arena_grids(), k, floors = 0;
            float seen[MAX_ARENA_GRIDS];
            for (k = 0; k < ng; k++) {
                int j, dup = 0;
                for (j = 0; j < floors; j++)
                    dup = dup || seen[j] == arena_grids_g[k].origin[2];
                if (!dup)
                    seen[floors++] = arena_grids_g[k].origin[2];
            }
            CHECK(ng >= 3 && floors >= 3 && arena_total_cells(ng) >= 35,
                  "New Game's arena spawns on %d grids, %d floors, %d cells (psq_cap's 35 fit)",
                  ng, floors, arena_total_cells(ng));
        }
        CHECK(pr_global_struct->total_monsters == 0, "the arena starts with no monsters");
        for (t = -1; ; t++) {
            const char *m;
            cls = psq_policy_tier_class(t);
            if (!cls[0]) {
                if (t >= 0)
                    break;
                continue;
            }
            m = class_model(cls);
            Sys_Printf("psq-test: class %d %s -> %s precached %d\n", t, cls,
                       m ? m : "?", m ? model_precached(m) : 0);
            all = all && m && model_precached(m);
        }
        CHECK(all && model_precached("progs/wizard.mdl") && model_precached("progs/shambler.mdl"),
              "every ladder class and the swarm class were precached during load");
        arena_push(1);
        break;
    }
    case 2:
        CHECK(count_live_process_monsters() == 6, "six process monsters spawned (%d)",
              count_live_process_monsters());
        CHECK(arena_cells_ok("spawn", 6, 1), "each teleported onto its own free grid cell");
        CHECK(is_class(3002, "monster_ogre") && is_class(3004, "monster_shambler")
              && is_class(3006, "monster_demon1"),
              "the hull 2 classes are among them (ogre, shambler, fiend)");
        CHECK(pr_global_struct->total_monsters == 6 && counted_all() == 6,
              "the level total counts them (%g)", pr_global_struct->total_monsters);
        Cbuf_AddText("psq_test_hit 3001 5\npsq_test_hit 3002 5000\n");
        arena_push(2);
        break;
    case 3:
        CHECK(proc_fake_renice_count() == (want_renice ? 1 : 0),
              "arena wound: %d renice (policy %d)", proc_fake_renice_count(), policy);
        if (want_renice)
            CHECK(proc_fake_last_renice_pid() == 3001, "renice went to pid 3001");
        CHECK(proc_fake_kill_count() == (want_kill ? 1 : 0),
              "arena kill: %d SIGTERM (policy %d)", proc_fake_kill_count(), policy);
        if (want_kill)
            CHECK(proc_fake_last_kill_pid() == 3002, "SIGTERM went to pid 3002");
        CHECK(count_live(3001, NULL) == 1, "the wounded monster lives");
        CHECK(count_live(3002, NULL) == 0 && count_tag(-3) == 1,
              "the killed one is a corpse tagged PSQ_CORPSE");
        CHECK(pr_global_struct->killed_monsters == 1
              && pr_global_struct->total_monsters == 6 && counted_all() == 6,
              "kill counted; the total keeps the corpse (%g)", pr_global_struct->total_monsters);
        total_before_exit = pr_global_struct->total_monsters;
        arena_push(2);
        break;
    case 4: {
        float swarm = counted_sum(-1);
        const char *cls = "";

        CHECK(count_live(3003, NULL) == 0, "exited process: its monster teleported out");
        CHECK(pr_global_struct->total_monsters == total_before_exit - 1 + swarm
              && pr_global_struct->total_monsters == counted_all(),
              "it left the level total (%g - 1 + %g swarm = %g)",
              total_before_exit, swarm, pr_global_struct->total_monsters);
        CHECK(count_live(3002, NULL) == 0, "the killed process is not respawned");
        CHECK(count_live(-1, NULL) > 0 && count_live(-1, NULL) <= PSQ_SWARM_PER_BURST,
              "fork burst in the arena: %d Scrags", count_live(-1, NULL));
        count_live(-1, &cls);
        CHECK(!strcmp(cls, "monster_wizard"), "swarm members are Scrags");
        CHECK(arena_cells_ok("after the exit", 4, 0), "the rest keep distinct cells");
        {
            /* Every grid full but the last: the next monster can only land
             * there, after the pick passes the full grids it draws first
             * (psq_gridspot's wrap), numbered after all the others' cells. */
            int ng = arena_grids();
            float c[3];
            int k, free_last = 0, base = arena_total_cells(ng) - psq_grid_cells(&arena_grids_g[ng - 1]);
            block_free_cells(ng - 1);
            for (k = base; k < arena_total_cells(ng); k++)
                free_last += arena_cell(ng, k, c) != NULL;
            CHECK(ng >= 2 && nblockers > 0 && free_last > 0,
                  "every free cell blocked but the last grid's (%d blocked, %d cells there)",
                  nblockers, free_last);
        }
        arena_push(3);
        break;
    }
    case 5:
        arena_push(3);   /* this poll's think places 3007 */
        break;
    case 6: {
        int ng = arena_grids();
        int total = arena_total_cells(ng);
        int base = total - psq_grid_cells(&arena_grids_g[ng - 1]);
        int i, found = 0;

        for (i = 1; i < sv.num_edicts; i++) {
            edict_t *e = EDICT_NUM(i);
            float cell, c[3];
            if (e->free || pid_of(e) != 3007 || e->v.health <= 0)
                continue;
            found++;
            cell = field_of(e, "psq_gridcell");
            Sys_Printf("psq-test: last grid free: pid 3007 %s at (%g %g %g), cell %g (last grid "
                       "holds %d..%d)\n", PR_GetString(e->v.classname), e->v.origin[0],
                       e->v.origin[1], e->v.origin[2], cell, base, total - 1);
            CHECK(cell >= base && cell < total && arena_cell(ng, (int) cell, c)
                  && fabsf(e->v.origin[0] - c[0]) < 0.5f && fabsf(e->v.origin[1] - c[1]) < 0.5f,
                  "it lands on the last grid, on its cell's centre, numbered after every "
                  "other grid's cells (cell %g of %d..%d)", cell, base, total - 1);
        }
        CHECK(found == 1, "the new process got a monster (%d)", found);
        CHECK(arena_cells_ok("last grid", 5, 0), "no grid cell was given twice");
        block_free_cells(-1);
        CHECK(nblockers > 0, "then every free cell of every grid blocked (%d)", nblockers);
        arena_push(4);
        break;
    }
    case 7:
        arena_push(4);   /* this poll's think places 3008 */
        break;
    case 8: {
        psq_grid g;
        int i;
        int found = 0;

        for (i = 1; i < sv.num_edicts; i++) {
            edict_t *e = EDICT_NUM(i);
            if (e->free || pid_of(e) != 3008 || e->v.health <= 0)
                continue;
            found++;
            arena_grid(&g);
            Sys_Printf("psq-test: grid full: pid 3008 %s at (%g %g %g), cell %g\n",
                       PR_GetString(e->v.classname), e->v.origin[0], e->v.origin[1],
                       e->v.origin[2], field_of(e, "psq_gridcell"));
        }
        /* No player on a dedicated server: the fallback rings the player
         * start, which is in the start room's safe zone (psq_safe), so the
         * process waits for a cell rather than arrive among the arches. */
        CHECK(found == 0, "grid full: no monster placed in the start room; the new process "
              "waits for a free cell (%d placed)", found);
        CHECK(arena_cells_ok("grid full", 5, 0), "and no grid cell was given twice");
        {
            int i2, in_room = 0;
            for (i2 = 1; i2 < sv.num_edicts; i2++) {
                edict_t *e = EDICT_NUM(i2);
                if (!e->free && pid_of(e) > 1 && e->v.health > 0 && e->v.origin[1] < -592)
                    in_room++;
            }
            CHECK(in_room == 0, "no process monster in the start room (%d)", in_room);
        }
        CHECK(pr_global_struct->total_monsters == counted_all(),
              "the total follows (%g)", pr_global_struct->total_monsters);
        unblock_cells();
        Cbuf_AddText("psq_test_hit -1 1000\n");
        arena_push(4);
        break;
    }
    case 9: {
        int i;
        float cell = -2;
        for (i = 1; i < sv.num_edicts; i++) {
            edict_t *e = EDICT_NUM(i);
            if (!e->free && pid_of(e) == 3008 && e->v.health > 0)
                cell = field_of(e, "psq_gridcell");
        }
        CHECK(cell >= 0, "cells free again: pid 3008 arrived on one (cell %g)", cell);
    }
        CHECK(count_live(-1, NULL) == 0 && psq_policy_swarm_live() == 0, "swarm killed");
        CHECK(proc_fake_renice_count() == (want_renice ? 1 : 0)
              && proc_fake_kill_count() == (want_kill ? 1 : 0),
              "swarm deaths acted on nothing");
        CHECK(pr_global_struct->total_monsters == counted_all(),
              "the total still matches (%g)", pr_global_struct->total_monsters);
        CHECK(PSQ_SaveRule() == PSQ_SAVE_REFUSED_ARENA, "the save rule here is the arena's");
        finished = 1;
        /* With psq_testarena on, lq_e1m1 would get a controller of its own;
         * the exit checks are about leaving the arena. */
        if (want_testarena)
            Cbuf_AddText("psq_testarena 0\n");
        Cbuf_AddText("save psqarena_try\npsq_test_parms new\n"
                     "changelevel lq_e1m1\npsq_test_after_exit\npsq_test_parms change\n"
                     "save psq_campaign\npsq_test_finish\n");
        break;
    }
}

/* psq_test_stray: a psq_arena controller that a third-party map or a
 * crafted save brings to a level that is neither the arena nor a test
 * arena (it thinks; it was never precached for). */
static void PSQ_Test_Stray_f(void)
{
    dfunction_t *f = find_function("psq_think");
    edict_t *e;

    if (!f)
        Sys_Error("psq-test: no psq_think in progs");
    e = ED_Alloc();
    e->v.classname = PR_SetEngineString("psq_arena");
    e->v.think = f - pr_functions;
    e->v.nextthink = sv.time + 1;
    Sys_Printf("psq-test: stray controller on %s\n", sv.name);
}

/* psq_test_newgame: what Single Player > New Game runs (menu.c). */
static void PSQ_Test_NewGame_f(void)
{
    newgame_map = psq_new_game_map(COM_FileExists(PSQ_ARENA_BSP, NULL));
    Sys_Printf("psq-test: New Game opens %s\n", newgame_map);
    if (strcmp(newgame_map, PSQ_ARENA_MAP)) {
        printf("psq-test: SKIP: no arena map in the data (run tools/setup-build.sh)\n");
        exit(77);
    }
    arena_run = 1;
    forget_saves();
    /* The arena's own processes, in place of lq_e1m1's first snapshot. */
    proc_fake_reset();
    arena_push(1);
    Cbuf_AddText(va("map %s\n", newgame_map));
}

/* psq_test_parms new|change: give the player slot an arena loadout, run
 * the engine's changelevel parms step, and check what the next level gets. */
static void PSQ_Test_Parms_f(void)
{
    client_t *c = &svs.clients[0];
    edict_t  *e = c->edict;
    int       want_new = !strcmp(Cmd_Argv(1), "new");
    float     items = IT_AXE | IT_SHOTGUN | IT_SUPER_SHOTGUN | IT_NAILGUN;

    e->v.items = items;
    e->v.health = 77;
    e->v.armorvalue = 50;
    e->v.ammo_shells = 40;
    c->active = true;
    SV_SaveSpawnparms();
    c->active = false;
    Sys_Printf("psq-test: leaving %s: parms items %g health %g armor %g shells %g\n",
               sv.name, c->spawn_parms[0], c->spawn_parms[1], c->spawn_parms[2],
               c->spawn_parms[3]);
    if (want_new)
        CHECK(c->spawn_parms[0] == (IT_AXE | IT_SHOTGUN) && c->spawn_parms[1] == 100
              && c->spawn_parms[2] == 0 && c->spawn_parms[3] == 25,
              "leaving the arena resets the parms: no super shotgun, nailgun or armour");
    else
        CHECK(c->spawn_parms[0] == items && c->spawn_parms[1] == 77
              && c->spawn_parms[2] == 50 && c->spawn_parms[3] == 40,
              "leaving %s keeps the parms", sv.name);
}

static void PSQ_Test_AfterExit_f(void)
{
    int i;
    int tagged = 0;
    int n = 0;

    CHECK(!strcmp(sv.name, "lq_e1m1"), "the exit opened lq_e1m1 (%s)", sv.name);
    for (i = 1; i < sv.num_edicts; i++)
        if (!EDICT_NUM(i)->free && pid_of(EDICT_NUM(i)) != 0)
            tagged++;
    arena_controller(&n);
    CHECK(tagged == 0, "no process monster followed (%d tagged)", tagged);
    CHECK(n == 0, "no controller on the campaign map (%d)", n);
    CHECK(!psq_policy_level_active(), "the process level ended");
    CHECK(PSQ_SaveRule() == PSQ_SAVE_ALLOWED, "the save rule allows saving here");
}

/* Lines of the console log holding s. */
static int log_count(const char *s)
{
    char path[MAX_OSPATH];
    char line[1024];
    FILE *f;
    int n = 0;

    q_snprintf(path, sizeof(path), "%s/qconsole.log", host_parms->userdir);
    f = fopen(path, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f))
        if (strstr(line, s))
            n++;
    fclose(f);
    return n;
}

static void PSQ_Test_Finish_f(void)
{
    char path[MAX_OSPATH];
    struct stat st;

    q_snprintf(path, sizeof(path), "%s/psqarena_try.sav", com_gamedir);
    CHECK(stat(path, &st) != 0, "no save was written in the arena");
    q_snprintf(path, sizeof(path), "%s/psq_campaign.sav", com_gamedir);
    CHECK(stat(path, &st) == 0, "the campaign save was written: %s", path);
    CHECK(log_count("Can't save in the psQuake arena.") == 1
          && log_count("Can't save here") == 0,
          "one refusal message for one refused save (%d arena, %d process)",
          log_count("Can't save in the psQuake arena."), log_count("Can't save here"));
    Sys_Printf("psq-test: %d checks, %d failed\n", checks_run, checks_failed);
    if (checks_failed)
        Sys_Error("psq-test: %d of %d checks failed", checks_failed, checks_run);
    Sys_Printf("psq-test: ALL PASS\n");
    Cbuf_AddText("quit\n");
}

/* ------------------------------------------------------ resupply scenario */

/* psq_test_supply: New Game in the arena with six fake processes, the
 * clock at host_framerate 0.5 (ten times real time on a dedicated server),
 * checked at every controller think against the controller's own tunables
 * (the psq_supply_* and psq_unlock_* globals in psq.qc):
 *
 *   timer    no drop before psq_supply_first; while the drops are claimed
 *            at once, consecutive ones psq_supply_hard apart (six live
 *            monsters count as many) until the monsters leave, then
 *            psq_supply_every apart
 *   cap      left unclaimed for 60 s, exactly psq_supply_max drops arrive
 *            and no more; never more than that lie unclaimed
 *   weapons  each lands once, in order (super shotgun, nailgun, super
 *            nailgun, grenade and rocket launchers, lightning gun), at its
 *            unlock time; the nailgun unlocks while every spot is blocked by
 *            a solid box the spot search does not see, so it falls out of
 *            the level and must come again once they clear
 *   boxes    ammo in big boxes while monsters are many, small after
 *   pickup   a drop that is due arrives as soon as one is picked up
 *   spots    every resupplied item stands on a psq_supply spot
 *
 * A dedicated server has no player, so the low-health rule is not reached
 * here; the busy rule is the same branch. Claiming stands in for pickups. */

static int supply_run;
static int sup_drops_seen, sup_weapons_seen, sup_phase, sup_spot_bad, sup_cap_bad;
static int sup_drops_in_b, sup_drops_in_c;
static double sup_drop_t[256];
static int sup_ndrop;

static float qc_global(const char *name)
{
    ddef_t *defs = (ddef_t *) ((byte *) progs + progs->ofs_globaldefs);
    int i;

    for (i = 0; i < progs->numglobaldefs; i++)
        if (!strcmp(PR_GetString(defs[i].s_name), name)) {
            if ((defs[i].type & ~DEF_SAVEGLOBAL) != ev_float || defs[i].ofs >= progs->numglobals)
                Sys_Error("psq-test: QuakeC global %s is not a float", name);
            return G_FLOAT(defs[i].ofs);
        }
    Sys_Error("psq-test: no QuakeC global %s", name);
    return 0;
}

static const char *const sup_weapons[] = {
    "weapon_supershotgun", "weapon_nailgun", "weapon_supernailgun",
    "weapon_grenadelauncher", "weapon_rocketlauncher", "weapon_lightning" };
static const char *const sup_unlock[] = {
    "psq_unlock_supershotgun", "psq_unlock_nailgun", "psq_unlock_supernailgun",
    "psq_unlock_grenadelauncher", "psq_unlock_rocketlauncher", "psq_unlock_lightning" };

static int sup_has_model(edict_t *e)
{
    return e->v.model && PR_GetString(e->v.model)[0];
}

/* Unclaimed resupplied items of a kind (1 health/ammo, 2 weapon); with
 * cls, of that class only. */
static int sup_count(int kind, const char *cls)
{
    int i, n = 0;
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        if (!e->free && field_of(e, "psq_supplied") == kind && sup_has_model(e)
            && (!cls || !strcmp(PR_GetString(e->v.classname), cls)))
            n++;
    }
    return n;
}

/* Every unclaimed resupplied item stands on a psq_supply spot. */
static int sup_on_spots(void)
{
    int i, j, ok = 1;
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        int on = 0;
        if (e->free || field_of(e, "psq_supplied") <= 0 || !sup_has_model(e))
            continue;
        for (j = 1; j < sv.num_edicts && !on; j++) {
            edict_t *s = EDICT_NUM(j);
            on = !s->free && !strcmp(PR_GetString(s->v.classname), "psq_supply")
                 && fabsf(s->v.origin[0] - e->v.origin[0]) < 1
                 && fabsf(s->v.origin[1] - e->v.origin[1]) < 1;
        }
        if (!on) {
            Sys_Printf("psq-test: supply: %s at (%g %g %g) is on no spot\n",
                       PR_GetString(e->v.classname), e->v.origin[0], e->v.origin[1],
                       e->v.origin[2]);
            ok = 0;
        }
    }
    return ok;
}

/* What a pickup does to an item in single player: no model, not solid. */
static void sup_claim_all(void)
{
    int i;
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        if (!e->free && field_of(e, "psq_supplied") > 0 && sup_has_model(e)) {
            e->v.model = 0;
            e->v.solid = SOLID_NOT;
            SV_LinkEdict(e, false);
        }
    }
}

/* Consecutive drops in [from, to) all gap apart (within the 1 s the
 * controller thinks at); how many gaps were judged. */
static int sup_gaps(double from, double to, float gap, const char *what)
{
    int i, n = 0, ok = 1;
    for (i = 1; i < sup_ndrop; i++) {
        double a = sup_drop_t[i - 1], b = sup_drop_t[i];
        if (a < from || b >= to)
            continue;
        n++;
        if (fabs((b - a) - gap) > 1.01) {
            Sys_Printf("psq-test: supply: %s: drops at %.1f and %.1f are %.1f s apart, want %g\n",
                       what, a, b, b - a, gap);
            ok = 0;
        }
    }
    CHECK(ok && n >= 2, "%s: %d gaps between drops, each %g s", what, n, gap);
    return n;
}

/* Weapons that have landed (PlaceItem made them triggers), each checked
 * against the unlock order and claimed at once, so each counts once. The
 * nailgun unlocks while every spot is blocked (sup_block): it must fall out,
 * be dropped again, and count once the spots clear. */
static double sup_block_from = 58, sup_block_to = 63;
static int sup_squat[64], sup_nsquat, sup_squatted;

/* While the spots are blocked, take every freed edict slot the engine would
 * reuse (as a monster or the arrival fog spawned in the same think would),
 * so a weapon that fell out is no longer recognisable by its own fields: the
 * controller must still see that it was lost. Called inside the controller's
 * think, before its resupply step; the squatters stay until 70 s, as a monster
 * that took the slot would. */
static void sup_squat_freed(int take)
{
    int i, top = sv.num_edicts;
    if (!take) {   /* after the window: let them go */
        for (i = 0; i < sup_nsquat; i++)
            ED_Free(EDICT_NUM(sup_squat[i]));
        sup_nsquat = 0;
        return;
    }
    while (sup_nsquat < 64) {
        edict_t *e = ED_Alloc();
        if (NUM_FOR_EDICT(e) >= top) {   /* a fresh slot: nothing left to reuse */
            ED_Free(e);
            break;
        }
        e->v.classname = PR_SetEngineString("psq_test_squatter");
        e->v.model = PR_SetEngineString("progs/player.mdl");   /* a model, solid: */
        e->v.solid = SOLID_SLIDEBOX;                             /* like a monster */
        sup_squat[sup_nsquat++] = NUM_FOR_EDICT(e);
        sup_squatted++;
    }
}
static int sup_blockers[64], sup_nblockers, sup_fell_before, sup_fell_after;
static int sup_big_bad, sup_small_bad;

static void sup_count_weapons(double t)
{
    int i, k;
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        float at;
        double lo, hi;
        if (e->free || field_of(e, "psq_supplied") != 2 || !sup_has_model(e)
            || e->v.solid != SOLID_TRIGGER)
            continue;
        for (k = 0; k < 6 && strcmp(PR_GetString(e->v.classname), sup_weapons[k]); k++)
            ;
        at = k < 6 ? qc_global(sup_unlock[k]) : 0;
        lo = at;
        hi = at + 2.01;   /* dropped at the first think past it, seen the next */
        if (k == 1) {     /* the nailgun: after the blocked spots clear */
            lo = sup_block_to;
            hi = sup_block_to + 3.01;
        }
        CHECK(k == sup_weapons_seen && t >= lo && t < hi,
              "weapon %d, %s, lands once, in order, at %.1f s (unlocks at %g; due %.0f..%.0f)",
              sup_weapons_seen + 1, PR_GetString(e->v.classname), t, at, lo, hi);
        sup_weapons_seen++;
        e->v.model = 0;   /* picked up */
        e->v.solid = SOLID_NOT;
        SV_LinkEdict(e, false);
    }
}

/* Solid boxes over every spot, with no monster or client flag: the spot
 * search does not see them, but an item dropped there starts inside one. */
static void sup_block_spots(void)
{
    int i;
    sup_nblockers = 0;
    for (i = 1; i < sv.num_edicts && sup_nblockers < 64; i++) {
        edict_t *s = EDICT_NUM(i), *b;
        if (s->free || strcmp(PR_GetString(s->v.classname), "psq_supply"))
            continue;
        b = ED_Alloc();
        b->v.classname = PR_SetEngineString("psq_test_block");
        b->v.solid = SOLID_BBOX;
        b->v.movetype = MOVETYPE_NONE;
        VectorCopy(s->v.origin, b->v.origin);
        b->v.mins[0] = -24; b->v.mins[1] = -24; b->v.mins[2] = -16;
        b->v.maxs[0] = 40; b->v.maxs[1] = 40; b->v.maxs[2] = 64;
        VectorSubtract(b->v.maxs, b->v.mins, b->v.size);
        SV_LinkEdict(b, false);
        sup_blockers[sup_nblockers++] = NUM_FOR_EDICT(b);
    }
}

static void sup_unblock_spots(void)
{
    int i;
    for (i = 0; i < sup_nblockers; i++)
        ED_Free(EDICT_NUM(sup_blockers[i]));
    sup_nblockers = 0;
}

/* Unclaimed ammo drops in the big box (spawnflags 1) exactly when big. */
static void sup_check_boxes(int want_big)
{
    int i;
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        const char *c = PR_GetString(e->v.classname);
        int big;
        if (e->free || field_of(e, "psq_supplied") != 1 || !sup_has_model(e)
            || !strncmp(c, "item_health", 11))
            continue;
        big = ((int) e->v.spawnflags & 1) != 0;
        if (want_big && !big)
            sup_big_bad++;
        if (!want_big && big)
            sup_small_bad++;
    }
}

/* The exit door (psq_exit_door): a process monster shoots it at 10 s and it
 * stays shut; something of the player's class shoots it at 12 s and it
 * rises (the door's origin, which starts at 0, climbs at its speed). */
static int sup_door_step_done;
static float sup_door_z0;

/* The exit door: func_door renames it "door", so find it by its th_die,
 * psq.qc's player-only gate (which also shows the gate is installed). */
static edict_t *sup_door(void)
{
    dfunction_t *f = find_function("psq_exit_door_die");
    int i;
    if (!f)
        Sys_Error("psq-test: no psq_exit_door_die in progs");
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        eval_t *die = e->free ? NULL : GetEdictFieldValue(e, "th_die");
        if (die && die->function == (func_t) (f - pr_functions))
            return e;
    }
    return NULL;
}

static void sup_door_step(double t)
{
    edict_t *d = sup_door();

    if (sup_door_step_done == 0 && t >= 10) {
        edict_t *m = first_live(3001);
        CHECK(d && m && d->v.takedamage, "the arena has a shootable exit door, and a monster");
        if (!d || !m)
            return;
        sup_door_z0 = d->v.origin[2];
        hit_by(d, m, 20);
        sup_door_step_done = 1;
    } else if (sup_door_step_done == 1 && t >= 12) {
        edict_t *p;
        CHECK(d && d->v.origin[2] == sup_door_z0 && d->v.velocity[2] == 0 && d->v.health > 0
              && d->v.takedamage,
              "a process monster's shot leaves the exit door shut (z %g, health %g)",
              d ? d->v.origin[2] : -1, d ? d->v.health : -1);
        p = ED_Alloc();
        p->v.classname = PR_SetEngineString("player");   /* the player's stand-in */
        hit_by(d, p, 20);
        ED_Free(p);
        sup_door_step_done = 2;
    } else if (sup_door_step_done == 2 && t >= 14) {
        CHECK(d && d->v.origin[2] > sup_door_z0 + 32,
              "the player's shot opens it: it has risen %g", d ? d->v.origin[2] - sup_door_z0 : 0);
        sup_door_step_done = 3;
    }
}

static void on_poll_supply(void)
{
    float t0 = qc_global("psq_supply_t0");
    float first = qc_global("psq_supply_first"), every = qc_global("psq_supply_every");
    float hard = qc_global("psq_supply_hard"), max = qc_global("psq_supply_max");
    float last_unlock = qc_global(sup_unlock[5]);
    int drops;
    double t;

    if (t0 <= 0)
        return;   /* the controller has not thought yet */
    t = sv.time - t0;
    drops = (int) qc_global("psq_supply_drops");

    sup_door_step(t);
    /* Fine frames while the spots are blocked: an item that falls out is
     * freed 0.2 s after its drop, and its slot can be reused (after 0.5 s)
     * before the next think, as in play. */
    Cvar_SetValue("host_framerate",
                  t >= sup_block_from - 1 && t < sup_block_to + 1 ? 0.1f : 0.5f);
    sup_squat_freed(t >= sup_block_from && t < 70);   /* they stay, as a monster would */
    sup_count_weapons(t);
    while (sup_drops_seen < drops) {
        if (sup_ndrop < (int) (sizeof(sup_drop_t) / sizeof(sup_drop_t[0])))
            sup_drop_t[sup_ndrop++] = t;
        if (sup_drops_seen == 0)
            CHECK(t >= first && t < first + 1.01, "the first drop at %.1f s (first %g)", t, first);
        if (sup_phase == 1)
            sup_drops_in_b++;
        if (sup_phase == 2 && sup_drops_in_c++ == 0)
            CHECK(t < 182.01, "a due drop arrives as soon as one is picked up (at %.1f s, "
                  "picked up at 180)", t);
        sup_drops_seen++;
    }
    if (drops == 0)
        CHECK(t < first + 1.01, "no drop yet at %.1f s", t);
    if (sup_count(1, NULL) > max)
        sup_cap_bad++;
    if (!sup_on_spots())
        sup_spot_bad++;
    if (sup_phase < 2 || t >= 186)   /* drops decided while the monsters leave may be either */
        sup_check_boxes(sup_phase < 2);

    /* Every spot blocked while the nailgun unlocks. */
    if (!sup_nblockers && t >= sup_block_from && t < sup_block_to) {
        sup_fell_before = log_count("fell out of level");
        sup_block_spots();
    } else if (sup_nblockers && t >= sup_block_to) {
        sup_unblock_spots();
        sup_fell_after = log_count("fell out of level");
        CHECK(sup_fell_before == 0 && sup_fell_after > sup_fell_before,
              "with every spot blocked, drops fell out of the level (%d before, %d after)",
              sup_fell_before, sup_fell_after);
    }

    if (sup_phase == 0) {          /* busy, drops claimed at once */
        sup_claim_all();
        if (t >= 120) {
            CHECK(count_live_process_monsters() >= qc_global("psq_supply_busy"),
                  "six process monsters live: many (%d)", count_live_process_monsters());
            sup_gaps(first, 120, hard, "many monsters live");
            sup_phase = 1;
        }
    } else if (sup_phase == 1) {   /* nothing claimed */
        if (t >= 180) {
            CHECK(sup_drops_in_b == (int) max && sup_count(1, NULL) == (int) max,
                  "unclaimed for 60 s: %d drops arrived and %d lie there (cap %g)",
                  sup_drops_in_b, sup_count(1, NULL), max);
            sup_claim_all();
            { psd_proc_t none[1]; proc_fake_push_snapshot(none, 0); }   /* every process exits */
            sup_phase = 2;
        }
    } else if (sup_phase == 2) {   /* no monsters, drops claimed at once */
        sup_claim_all();
        if (t >= last_unlock + 5) {
            CHECK(count_live_process_monsters() == 0, "the monsters left");
            sup_gaps(195, t, every, "no monsters");
            CHECK(sup_weapons_seen == 6, "six weapons landed, each once (%d)", sup_weapons_seen);
            CHECK(sup_squatted > 0, "freed slots were taken while the spots were blocked (%d)",
                  sup_squatted);
            CHECK(sup_door_step_done == 3, "the exit door checks ran (%d of 3)", sup_door_step_done);
            CHECK(sup_cap_bad == 0, "never more than %g unclaimed (%d polls over)", max,
                  sup_cap_bad);
            CHECK(sup_spot_bad == 0, "every item stood on a spot (%d polls not)", sup_spot_bad);
            CHECK(sup_big_bad == 0 && sup_small_bad == 0,
                  "ammo came in big boxes while monsters were many, small after "
                  "(%d small too soon, %d big too late)", sup_big_bad, sup_small_bad);
            CHECK(log_count("fell out of level") == sup_fell_after,
                  "nothing fell out of the level but in the blocked window (%d, %d then)",
                  log_count("fell out of level"), sup_fell_after);
            finished = 1;
            Cvar_SetValue("host_framerate", 0);
            Sys_Printf("psq-test: %d checks, %d failed\n", checks_run, checks_failed);
            if (checks_failed)
                Sys_Error("psq-test: %d of %d checks failed", checks_failed, checks_run);
            Sys_Printf("psq-test: ALL PASS\n");
            Cbuf_AddText("quit\n");
        }
    }
}

/* psq_test_supply: New Game, as psq_test_newgame, for the resupply checks. */
static void PSQ_Test_Supply_f(void)
{
    newgame_map = psq_new_game_map(COM_FileExists(PSQ_ARENA_BSP, NULL));
    if (strcmp(newgame_map, PSQ_ARENA_MAP)) {
        printf("psq-test: SKIP: no arena map in the data (run tools/setup-build.sh)\n");
        exit(77);
    }
    supply_run = 1;
    forget_saves();
    proc_fake_reset();
    arena_push(1);
    Cvar_SetValue("host_framerate", 0.5f);
    Cbuf_AddText(va("map %s\n", newgame_map));
}

/* ------------------------------------------------------ spread scenario */

/* -psqtest-spread, psq_test_spread: New Game's arena at psq_cap 35 with 35
 * fake processes. Once they have all arrived, how they spread over the
 * grids: a line per grid (its floor, cells, monsters) and the central
 * area's share, the grids on the arena's lowest floor (the Temple's sunken
 * courtyard). Each grid takes arrivals by its share of the cells, so the
 * centre must get at least half (it holds 74% of the cells), and the
 * side grids together at least one. */
static int spread_run;

static void PSQ_Test_Spread_f(void)
{
    psd_proc_t procs[40];
    int i;
    static const unsigned long long mb[] = { 10, 30, 100, 300, 800, 2000 };

    newgame_map = psq_new_game_map(COM_FileExists(PSQ_ARENA_BSP, NULL));
    if (strcmp(newgame_map, PSQ_ARENA_MAP)) {
        printf("psq-test: SKIP: no arena map in the data (run tools/setup-build.sh)\n");
        exit(77);
    }
    spread_run = 1;
    proc_fake_reset();
    for (i = 0; i < 35; i++)
        procs[i] = mk(5001 + i, 0, UID, 500 + i, va("proc%02d", i), mb[i % 6]);
    proc_fake_push_snapshot(procs, 35);
    Cvar_SetValue("psq_cap", 35);
    Cbuf_AddText(va("map %s\n", newgame_map));
}

static void on_poll_spread(void)
{
    int ng, k, i, live, central = 0, side = 0, off = 0;
    int per[MAX_ARENA_GRIDS];
    float lowest = 1e9f;

    if (sv.time > 50)
        Sys_Error("psq-test: spread: timed out at %.1f s (%d live)", sv.time,
                  count_live_process_monsters());
    live = count_live_process_monsters();
    if (live < 35 && sv.time < 30)
        return;
    finished = 1;
    ng = arena_grids();
    memset(per, 0, sizeof(per));
    for (k = 0; k < ng; k++)
        if (arena_grids_g[k].origin[2] < lowest)
            lowest = arena_grids_g[k].origin[2];
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        float c[3];
        const psq_grid *g;
        if (e->free || pid_of(e) <= 1 || e->v.health <= 0)
            continue;
        g = arena_cell(ng, (int) field_of(e, "psq_gridcell"), c);
        if (field_of(e, "psq_gridcell") < 0 || !g) {
            off++;
            continue;
        }
        per[g - arena_grids_g]++;
        if (g->origin[2] == lowest)
            central++;
        else
            side++;
    }
    Sys_Printf("psq-test: spread on %s at %.1f s: %d live process monsters (cap %g)\n",
               sv.name, sv.time, live, Cvar_VariableValue("psq_cap"));
    for (k = 0; k < ng; k++)
        Sys_Printf("psq-test: spread: grid %2d at (%5g %5g %4g) %3d cells: %2d monsters%s\n", k,
                   arena_grids_g[k].origin[0], arena_grids_g[k].origin[1],
                   arena_grids_g[k].origin[2], psq_grid_cells(&arena_grids_g[k]), per[k],
                   arena_grids_g[k].origin[2] == lowest ? "  (central)" : "");
    Sys_Printf("psq-test: spread: central %d, side %d, off the grids %d: central share %.0f%%\n",
               central, side, off, live ? 100.0 * central / live : 0.0);
    CHECK(live == 35, "35 process monsters arrived (%d)", live);
    CHECK(central * 2 >= live, "at least half in the central area (%d of %d)", central, live);
    CHECK(side >= 1, "and some in the side areas (%d)", side);
    CHECK(off == 0, "every one on a grid cell (%d off)", off);
    CHECK(arena_cells_ok("spread", central + side, 0), "no cell given twice");
    Sys_Printf("psq-test: %d checks, %d failed\n", checks_run, checks_failed);
    if (checks_failed)
        Sys_Error("psq-test: %d of %d checks failed", checks_failed, checks_run);
    Sys_Printf("psq-test: ALL PASS\n");
    Cbuf_AddText("quit\n");
}

/* ------------------------------------------------- deathmatch map scenario */

/* -psqtest-dm, +map lqdm1 (no psq_testarena): a deathmatch map gets process
 * monsters but none of the arena's rules. Worldspawn makes the controller
 * (psq_level_open); with no grid the monsters arrive round the map's
 * deathmatch spots; there are no skill arches, resupply or exit door; the
 * save rule refuses for the process monsters (not as an arena) and the
 * parms carry over. Then lq_e1m1, a campaign map: no controller, nothing. */
static int dm_phase;

static int count_class(const char *cls)
{
    int i;
    int n = 0;

    for (i = 1; i < sv.num_edicts; i++)
        if (!EDICT_NUM(i)->free && !strcmp(PR_GetString(EDICT_NUM(i)->v.classname), cls))
            n++;
    return n;
}

/* The distance from p to the nearest info_player_deathmatch (1e9: none). */
static float dm_spot_distance(const float *p)
{
    float best = 1e9f;
    int i;

    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        vec3_t d;
        if (e->free || strcmp(PR_GetString(e->v.classname), "info_player_deathmatch"))
            continue;
        VectorSubtract(p, e->v.origin, d);
        if (VectorLength(d) < best)
            best = VectorLength(d);
    }
    return best;
}

static void on_poll_dm(void)
{
    int i, n = 0, live = 0, near = 0;
    edict_t *c;

    if (dm_phase != 0 || sv.time < 5)
        return;
    dm_phase = 1;
    c = arena_controller(&n);
    CHECK(!strcmp(sv.name, "lqdm1"), "on the deathmatch map (%s)", sv.name);
    CHECK(Cvar_VariableValue("psq_testarena") == 0, "psq_testarena is off");
    CHECK(psq_map_allows_processes(sv.name) && !psq_arena_is_map(sv.name),
          "%s allows process monsters and is no psQuake arena", sv.name);
    CHECK(c != NULL && n == 1, "worldspawn made the one controller (%d)", n);
    CHECK(c && !(field_of(c, "psq_cell") > 0) && count_class("psq_grid") == 0,
          "it has no grid");
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        float d;
        if (e->free || pid_of(e) <= 1 || e->v.health <= 0)
            continue;
        live++;
        d = dm_spot_distance(e->v.origin);
        Sys_Printf("psq-test: pid %g %s at (%g %g %g), %g from a deathmatch spot\n",
                   pid_of(e), PR_GetString(e->v.classname), e->v.origin[0], e->v.origin[1],
                   e->v.origin[2], d);
        if (d <= 288 + 64)
            near++;
    }
    CHECK(live >= 4, "process monsters arrived (%d live)", live);
    CHECK(near == live, "every one round a deathmatch spot (%d of %d)", near, live);
    CHECK(count_class("psq_skill_arch") == 0 && count_class("psq_supply") == 0
          && count_class("psq_exit_door") == 0 && count_class("psq_safe") == 0,
          "no arches, resupply, exit door or start room");
    CHECK(PSQ_SaveRule() == PSQ_SAVE_REFUSED_PROCESSES,
          "the save rule refuses for the process monsters, not as an arena (%d)",
          PSQ_SaveRule());
    CHECK(psq_changelevel_parms(sv.name) == PSQ_PARMS_CHANGE, "leaving keeps the parms");
    forget_saves();
    Cbuf_AddText("save psqdm_try\npsq_test_parms change\nchangelevel lq_e1m1\n"
                 "psq_test_dm_after\n");
}

static void PSQ_Test_DmAfter_f(void)
{
    char path[MAX_OSPATH];
    struct stat st;
    int n = 0, i, tagged = 0;

    CHECK(!strcmp(sv.name, "lq_e1m1"), "then the campaign map (%s)", sv.name);
    arena_controller(&n);
    for (i = 1; i < sv.num_edicts; i++)
        if (!EDICT_NUM(i)->free && pid_of(EDICT_NUM(i)) != 0)
            tagged++;
    CHECK(n == 0 && tagged == 0, "a campaign map gets no controller (%d) and no process "
          "monster (%d)", n, tagged);
    CHECK(!psq_map_allows_processes(sv.name) && PSQ_SaveRule() == PSQ_SAVE_ALLOWED,
          "and its saves are allowed");
    q_snprintf(path, sizeof(path), "%s/psqdm_try.sav", com_gamedir);
    CHECK(stat(path, &st) != 0, "no save was written on the deathmatch map");
    CHECK(log_count("Can't save here") == 1 && log_count("Can't save in the psQuake arena.") == 0,
          "it was refused once, for the process monsters");
    finished = 1;
    Sys_Printf("psq-test: %d checks, %d failed\n", checks_run, checks_failed);
    if (checks_failed)
        Sys_Error("psq-test: %d of %d checks failed", checks_failed, checks_run);
    Sys_Printf("psq-test: ALL PASS\n");
    Cbuf_AddText("quit\n");
}

/* ----------------------------------------------------- hazards scenario */

/* -psqtest-hazards, +map psqtesthazards (tools/generate_testhazards.py, no
 * psq_testarena): each hazard kills one fake process's monster, one after
 * another, and the kill mode's effect, and only it, follows: simulate
 * signals nothing, renice signals nothing (a kill under it only forgets
 * the process), live sends SIGTERM to that pid and no other. Each monster
 * is set to 1 health first, so its first hurt kills it and no survived
 * wound renices anything. Every killed process stays killed (it never
 * comes back). Before the long fall, a short one (about 100) hurts nothing.
 * The positions are generate_testhazards.py's. */
typedef struct { int pid; const char *why; float at[3]; } hazard_t;
static const hazard_t hazards[] = {
    { 4001, "lava",     { -768, 0, -200 } },
    { 4002, "slime",    { -384, 0, -200 } },
    { 4003, "drowning", {    0, 0, -200 } },
    { 4004, "fall",     {    0, 384, 900 } },
    { 4005, "pit",      {  384, 0, -100 } },
    { 4006, "crush",    {  768, 0, 24 } },
};
#define NHAZARDS ((int) (sizeof(hazards) / sizeof(hazards[0])))
static int hz_step = -1;      /* the hazard under way; -1 before, NHAZARDS after */
static float hz_since;
static int hz_kills_before;
static int hz_short_done;

static int hazard_snapshot(psd_proc_t *out)
{
    static const char *const names[] = { "lava", "slime", "drown", "fall", "pit", "crush" };
    int i;

    for (i = 0; i < NHAZARDS; i++)
        out[i] = mk(hazards[i].pid, 0, UID, 400 + i, names[i], 30);
    return NHAZARDS;
}

static void put(edict_t *e, float x, float y, float z)
{
    e->v.origin[0] = x;
    e->v.origin[1] = y;
    e->v.origin[2] = z;
    VectorCopy(vec3_origin, e->v.velocity);
    e->v.flags = (float) ((int) e->v.flags & ~FL_ONGROUND);
    SV_LinkEdict(e, false);
}

static void hz_use_crusher(void)
{
    dfunction_t *f = find_function("door_use");
    int i;

    if (!f)
        Sys_Error("psq-test: no door_use in progs");
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *d = EDICT_NUM(i);
        if (!d->free && !strcmp(PR_GetString(d->v.classname), "door")
            && !strcmp(PR_GetString(d->v.targetname), "crusher")) {
            pr_global_struct->time = sv.time;
            pr_global_struct->self = EDICT_TO_PROG(d);
            pr_global_struct->other = EDICT_TO_PROG(sv.edicts);
            PR_ExecuteProgram(f - pr_functions);
            return;
        }
    }
    CHECK(0, "the map has its crusher door");
}

static void hz_start(int i)
{
    edict_t *e = first_live(hazards[i].pid);
    int policy = psproc_kill_policy;

    hz_step = i;
    hz_since = sv.time;
    hz_kills_before = proc_fake_kill_count();
    CHECK(e != NULL, "%s: pid %d has a live monster", hazards[i].why, hazards[i].pid);
    if (!e)
        return;
    Sys_Printf("psq-test: hazard %s: %s pid %d to (%g %g %g), policy %d\n", hazards[i].why,
               PR_GetString(e->v.classname), hazards[i].pid, hazards[i].at[0],
               hazards[i].at[1], hazards[i].at[2], policy);
    e->v.health = 1;
    put(e, hazards[i].at[0], hazards[i].at[1], hazards[i].at[2]);
    if (!strcmp(hazards[i].why, "crush"))
        hz_use_crusher();
}

static void hz_finish(void)
{
    int i;

    for (i = 0; i < NHAZARDS; i++)
        CHECK(count_live(hazards[i].pid, NULL) == 0 && !psq_policy_present(hazards[i].pid),
              "pid %d (%s) stays killed", hazards[i].pid, hazards[i].why);
    CHECK(proc_fake_renice_count() == 0, "no hazard reniced anything (%d)",
          proc_fake_renice_count());
    CHECK(proc_fake_kill_count() == (psproc_kill_policy == PSD_KILL_LIVE ? NHAZARDS : 0),
          "%d SIGTERMs in all (policy %d)", proc_fake_kill_count(), psproc_kill_policy);
    CHECK(pr_global_struct->killed_monsters == NHAZARDS, "every hazard death counted (%g)",
          pr_global_struct->killed_monsters);
    finished = 1;
    Sys_Printf("psq-test: %d checks, %d failed\n", checks_run, checks_failed);
    if (checks_failed)
        Sys_Error("psq-test: %d of %d checks failed", checks_failed, checks_run);
    Sys_Printf("psq-test: ALL PASS\n");
    Cbuf_AddText("quit\n");
}

static void on_poll_hazards(void)
{
    int i, all = 1;

    if (sv.time > 110)
        Sys_Error("psq-test: hazards: timed out at %.1f s (step %d)", sv.time, hz_step);
    if (hz_step < 0) {
        for (i = 0; i < NHAZARDS; i++)
            if (!first_live(hazards[i].pid))
                all = 0;
        if (!all || sv.time < 4)
            return;
        CHECK(!strcmp(sv.name, "psqtesthazards") && !psq_arena_is_map(sv.name),
              "on the hazard map (%s), no arena", sv.name);
        CHECK(arena_controller(NULL) != NULL && count_class("psq_hazards") == 1,
              "worldspawn made the controller and its hazard entity");
        if (!hz_short_done) {   /* a short fall first: no harm */
            edict_t *e = first_live(4004);
            hz_short_done = 1;
            hz_since = sv.time;
            e->v.health = 1;
            put(e, 0, 384, 128);
            return;
        }
        if (sv.time < hz_since + 3)
            return;
        CHECK(first_live(4004) != NULL && log_count("hazard fall") == 0,
              "a drop of about 100 hurt nothing");
        hz_start(0);
        return;
    }
    if (hz_step >= NHAZARDS)
        return;
    {
        const hazard_t *h = &hazards[hz_step];
        int policy = psproc_kill_policy;
        int dead = count_live(h->pid, NULL) == 0;

        if (!dead && sv.time < hz_since + 25)
            return;
        CHECK(dead, "%s killed pid %d's monster (%.1f s)", h->why, h->pid, sv.time - hz_since);
        if (strcmp(h->why, "pit") && strcmp(h->why, "crush"))
            CHECK(log_count(va("hazard %s", h->why)) == 1,
                  "%s hurt it once (%d log lines)", h->why, log_count(va("hazard %s", h->why)));
        if (policy == PSD_KILL_LIVE)
            CHECK(proc_fake_kill_count() == hz_kills_before + 1
                  && proc_fake_last_kill_pid() == h->pid,
                  "%s, live kill: SIGTERM went to pid %d (last %d, %d in all)", h->why,
                  h->pid, proc_fake_last_kill_pid(), proc_fake_kill_count());
        else
            CHECK(proc_fake_kill_count() == hz_kills_before,
                  "%s, policy %d: nothing was signalled", h->why, policy);
        CHECK(proc_fake_renice_count() == 0, "%s: nothing reniced", h->why);
        CHECK(!psq_policy_present(h->pid), "%s: pid %d is killed for the policy", h->why,
              h->pid);
        if (hz_step + 1 < NHAZARDS)
            hz_start(hz_step + 1);
        else {
            hz_step = NHAZARDS;
            hz_finish();
        }
    }
}

/* Called by PF_psq_poll on every call. */
static void poll_hook(int refresh_count)
{
    if (finished)
        return;
    if (gate_closed_run) {
        if (sv.time > 6) {
            int controllers = 0;
            finished = 1;
            arena_controller(&controllers);
            CHECK(!strcmp(sv.name, "lq_e1m1"), "closed gate: on lq_e1m1 (%s)", sv.name);
            CHECK(controllers >= 1, "closed gate: a controller is polling (%d)", controllers);
            CHECK(refresh_count == 0, "closed gate: no refresh (%d)", refresh_count);
            CHECK(count_live_process_monsters() == 0, "closed gate: no monsters");
            CHECK(proc_fake_renice_count() == 0 && proc_fake_kill_count() == 0,
                  "closed gate: no action");
            Sys_Printf("psq-test: %d checks, %d failed\n", checks_run, checks_failed);
            if (checks_failed)
                Sys_Error("psq-test: %d checks failed", checks_failed);
            Sys_Printf("psq-test: ALL PASS\n");
            Cbuf_AddText("quit\n");
        }
        return;
    }
    if (spread_run) {
        on_poll_spread();
        return;
    }
    if (dm_run) {
        if (sv.time > 40)
            Sys_Error("psq-test: dm: timed out at %.1f s", sv.time);
        on_poll_dm();
        return;
    }
    if (hazards_run) {
        on_poll_hazards();
        return;
    }
    if (supply_run) {
        if (sv.time > 600)
            Sys_Error("psq-test: supply: timed out at %.1f s", sv.time);
        on_poll_supply();
        return;
    }
    if (sv.time > 40)
        Sys_Error("psq-test: timed out at refresh %d", refresh_count);
    if (refresh_count != last_refresh) {
        last_refresh = refresh_count;
        if (arena_run)
            on_refresh_arena(refresh_count);
        else
            on_refresh(refresh_count);
    }
}

/* psq_test_killmode N: what the Options > psQuake page does with the
 * live-kill warning answered yes (a dedicated server has no menu). Test
 * builds only; the app has no console command that sets the kill mode. */
static void PSQ_Test_KillMode_f(void)
{
    int r = PSQ_SetKillMode(Q_atoi(Cmd_Argv(1)), true);

    if (r != PSQ_KILLMODE_CHANGED && r != PSQ_KILLMODE_UNCHANGED)
        Sys_Error("psq-test: psq_test_killmode %s refused (%d)", Cmd_Argv(1), r);
}

void psq_test_register(void)
{
    Cmd_AddCommand("psq_test_killmode", PSQ_Test_KillMode_f);
    Cmd_AddCommand("psq_test_hit", PSQ_Test_Hit_f);
    Cmd_AddCommand("psq_test_stray", PSQ_Test_Stray_f);
    if (untrusted_run)
        PSQ_StuffText("echo from a server\n");   /* no local server yet */
    Cmd_AddCommand("psq_test_newgame", PSQ_Test_NewGame_f);
    Cmd_AddCommand("psq_test_supply", PSQ_Test_Supply_f);
    Cmd_AddCommand("psq_test_parms", PSQ_Test_Parms_f);
    Cmd_AddCommand("psq_test_after_exit", PSQ_Test_AfterExit_f);
    Cmd_AddCommand("psq_test_finish", PSQ_Test_Finish_f);
    Cmd_AddCommand("psq_test_hit_by", PSQ_Test_HitBy_f);
    Cmd_AddCommand("psq_test_dm_after", PSQ_Test_DmAfter_f);
    Cmd_AddCommand("psq_test_spread", PSQ_Test_Spread_f);
    psq_test_poll_hook = poll_hook;
}

int main(int argc, char *argv[])
{
    static char *def_argv[] = {
        "psq_headless_test", "-dedicated", "1",
        "+developer", "1", "+psq_debug", "1", "+psq_testarena", "1",
        "+map", "lq_e1m1", "-condebug", "-psquake-root", PSQ_TEST_ROOT, NULL };
    struct stat st;
    int i;

    if (stat(PSQ_TEST_RESOURCES "/id1/pak0.pak", &st) != 0) {
        printf("psq-test: SKIP: no LibreQuake data (run tools/fetch-librequake.sh)\n");
        return 77;
    }
    if (argc <= 1) {
        argc = (int) (sizeof(def_argv) / sizeof(def_argv[0])) - 1;
        argv = def_argv;
    }
    {   /* The engine builds +commands from the first CMDLINE_LENGTH - 1
         * characters of the command line; a longer one silently drops them. */
        size_t len = 0;
        for (i = 1; i < argc; i++)
            len += strlen(argv[i]) + 1;
        if (len >= 256) {
            printf("psq-test: FAIL: command line is %zu characters; the engine keeps 255\n", len);
            return 1;
        }
    }
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-dedicated") && i + 1 < argc && strcmp(argv[i + 1], "1"))
            gate_closed_run = 1;
        if (!strcmp(argv[i], "-psqtest-stray"))
            stray_run = gate_closed_run = 1;
        if (!strcmp(argv[i], "-psqtest-untrusted"))
            untrusted_run = gate_closed_run = 1;
        if (!strcmp(argv[i], "-psqtest-dm"))
            dm_run = 1;
        if (!strcmp(argv[i], "-psqtest-hazards"))
            hazards_run = 1;
        if (!strcmp(argv[i], "+psq_testarena") && i + 1 < argc && !strcmp(argv[i + 1], "1"))
            want_testarena = 1;
    }
    return psq_engine_main(argc, argv);
}
