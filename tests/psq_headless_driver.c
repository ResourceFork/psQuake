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
 * (on_refresh_arena): grid cells, wound, kill, retire, swarm, the grid-full
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
    n = snapshot(1, procs);
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
 *   poll 4   every free cell is blocked; new pid 3007 (poll 5) is placed by
 *            the near-player fallback, off the grid (checked at poll 6)
 *   poll 6   the swarm is killed
 *   then     save refused (the arena rule, one message), the exit's parms
 *            reset, changelevel lq_e1m1: no process monster or controller
 *            there, save allowed, and the campaign keeps its parms.
 */

static int arena_run;
static const char *newgame_map;
static int blockers[128];
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
    psq_grid g;
    int used[PSQ_GRID_MAX_DIM * PSQ_GRID_MAX_DIM];
    int i;
    int ok = 1;
    int gridded = 0;

    if (!arena_grid(&g))
        return 0;
    memset(used, 0, sizeof(used));
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = EDICT_NUM(i);
        float cell, c[3];
        int idx;

        if (e->free || pid_of(e) <= 1 || e->v.health <= 0)
            continue;
        cell = field_of(e, "psq_gridcell");
        idx = (int) cell;
        if (cell < 0)
            continue;   /* placed by the fallback; checked by its caller */
        gridded++;
        if (cell != (float) idx || !psq_grid_centre(&g, idx, c)) {
            Sys_Printf("psq-test: %s: pid %d has bad cell %g\n", when, (int) pid_of(e), cell);
            ok = 0;
            continue;
        }
        Sys_Printf("psq-test: %s: pid %d %s (%gx%g) in cell %d (col %d, row %d) "
                   "at (%g %g %g), centre (%g %g)\n", when, (int) pid_of(e),
                   PR_GetString(e->v.classname), e->v.size[0], e->v.size[1],
                   idx, idx % g.cols, idx / g.cols,
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
        if (e->v.origin[0] + e->v.mins[0] < c[0] - g.cell / 2
            || e->v.origin[0] + e->v.maxs[0] > c[0] + g.cell / 2
            || e->v.origin[1] + e->v.mins[1] < c[1] - g.cell / 2
            || e->v.origin[1] + e->v.maxs[1] > c[1] + g.cell / 2) {
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

static void block_free_cells(void)
{
    psq_grid g;
    int i;

    nblockers = 0;
    if (!arena_grid(&g))
        return;
    for (i = 0; i < psq_grid_cells(&g) && nblockers < (int) (sizeof(blockers) / sizeof(blockers[0])); i++) {
        int idx = i;
        float c[3];
        edict_t *e;
        int j, taken = 0;

        /* Leave a cell a monster already holds. */
        for (j = 1; j < sv.num_edicts && !taken; j++) {
            edict_t *m = EDICT_NUM(j);
            taken = !m->free && pid_of(m) > 1 && m->v.health > 0
                    && field_of(m, "psq_gridcell") == (float) idx;
        }
        if (taken)
            continue;
        psq_grid_centre(&g, idx, c);
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
    Sys_Printf("psq-test: blocked %d free cells\n", nblockers);
}

static void unblock_cells(void)
{
    int i;
    for (i = 0; i < nblockers; i++)
        ED_Free(EDICT_NUM(blockers[i]));
    nblockers = 0;
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

        CHECK(newgame_map && !strcmp(newgame_map, "psqarena"), "New Game opened %s",
              newgame_map ? newgame_map : "(nothing)");
        CHECK((Cvar_VariableValue("psq_testarena") != 0) == want_testarena,
              "psq_testarena is %g, as the command line asked",
              Cvar_VariableValue("psq_testarena"));
        CHECK(psq_arena_is_map(sv.name), "the arena is running (%s)", sv.name);
        CHECK(arena_controller(&n) != NULL, "the map's psq_arena is the one controller (%d)", n);
        CHECK(arena_grid(&g) && g.cols == 11 && g.rows == 7 && g.cell == 96
              && g.origin[0] == 0 && g.origin[1] == 0 && g.origin[2] == 0,
              "it carries the grid: %d x %d cells of %g at (%g %g %g)",
              g.cols, g.rows, g.cell, g.origin[0], g.origin[1], g.origin[2]);
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
        block_free_cells();
        CHECK(nblockers > 0, "every free cell blocked (%d)", nblockers);
        arena_push(3);
        break;
    }
    case 5:
        arena_push(3);   /* this poll's think places 3007 */
        break;
    case 6: {
        psq_grid g;
        int i;
        int found = 0;

        for (i = 1; i < sv.num_edicts; i++) {
            edict_t *e = EDICT_NUM(i);
            if (e->free || pid_of(e) != 3007 || e->v.health <= 0)
                continue;
            found++;
            arena_grid(&g);
            Sys_Printf("psq-test: grid full: pid 3007 %s at (%g %g %g), cell %g\n",
                       PR_GetString(e->v.classname), e->v.origin[0], e->v.origin[1],
                       e->v.origin[2], field_of(e, "psq_gridcell"));
            CHECK(field_of(e, "psq_gridcell") == -1, "grid full: placed off the grid");
            CHECK(SV_PointContents(e->v.origin) != CONTENTS_SOLID, "and not in a wall");
        }
        CHECK(found == 1, "grid full: the new process still got a monster (%d)", found);
        CHECK(arena_cells_ok("grid full", 4, 0), "and no grid cell was given twice");
        CHECK(pr_global_struct->total_monsters == counted_all(),
              "the total follows (%g)", pr_global_struct->total_monsters);
        unblock_cells();
        Cbuf_AddText("psq_test_hit -1 1000\n");
        arena_push(3);
        break;
    }
    case 7:
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
    Cmd_AddCommand("psq_test_parms", PSQ_Test_Parms_f);
    Cmd_AddCommand("psq_test_after_exit", PSQ_Test_AfterExit_f);
    Cmd_AddCommand("psq_test_finish", PSQ_Test_Finish_f);
    Cmd_AddCommand("psq_test_hit_by", PSQ_Test_HitBy_f);
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
        if (!strcmp(argv[i], "+psq_testarena") && i + 1 < argc && !strcmp(argv[i + 1], "1"))
            want_testarena = 1;
    }
    return psq_engine_main(argc, argv);
}
