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
 *   last     `save` is refused on this level
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
#include "psq_policy.h"
#include "proc_fake.h"
#include "psproc_options.h"

#include <sys/stat.h>
#include <unistd.h>

#define UID 501

static int checks_run;
static int checks_failed;
static int gate_closed_run;
static int last_refresh;
static int finished;

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

static void hit(edict_t *e, float dmg)
{
    dfunction_t *f = find_function("T_Damage");

    if (!f)
        Sys_Error("psq-test: no T_Damage in progs");
    pr_global_struct->time = sv.time;
    pr_global_struct->self = EDICT_TO_PROG(sv.edicts);
    pr_global_struct->other = EDICT_TO_PROG(sv.edicts);
    G_INT(OFS_PARM0) = EDICT_TO_PROG(e);
    G_INT(OFS_PARM1) = EDICT_TO_PROG(sv.edicts);
    G_INT(OFS_PARM2) = EDICT_TO_PROG(sv.edicts);
    G_FLOAT(OFS_PARM3) = dmg;
    PR_ExecuteProgram(f - pr_functions);
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

static void finish(void)
{
    char path[MAX_OSPATH];
    struct stat st;

    q_snprintf(path, sizeof(path), "%s/psqtest.sav", com_gamedir);
    CHECK(stat(path, &st) != 0, "save was refused: no %s", path);
    CHECK(PSQ_SaveRefused(), "and it was psQuake's rule that refused it");
    CHECK(PSQ_CvarSetRefused("psq_killpolicy") && PSQ_CvarSetRefused("PSQ_AllUsers")
          && !PSQ_CvarSetRefused("psq_cap"),
          "QuakeC cvar_set may not touch the kill policy or all-users");
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
        push_step(1);
        break;
    case 2:
        CHECK(count_live_process_monsters() == 5, "five process monsters spawned");
        CHECK(is_class(2001, "monster_dog"), "10 MB editor is a dog");
        CHECK(is_class(2002, "monster_ogre"), "300 MB browser is an ogre");
        CHECK(is_class(2003, "monster_knight"), "100 MB indexer is a knight");
        CHECK(is_class(2004, "monster_army"), "30 MB builder is a grunt");
        CHECK(is_class(2005, "monster_dog"), "10 MB forker is a dog");
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
        break;
    case 8:
        finished = 1;
        finish();
        break;
    }
}

/* Called by PF_psq_poll on every call. */
static void poll_hook(int refresh_count)
{
    if (finished)
        return;
    if (gate_closed_run) {
        if (sv.time > 6) {
            finished = 1;
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
        on_refresh(refresh_count);
    }
}

void psq_test_register(void)
{
    Cmd_AddCommand("psq_test_hit", PSQ_Test_Hit_f);
    psq_test_poll_hook = poll_hook;
}

int main(int argc, char *argv[])
{
    static char *def_argv[] = {
        "psq_headless_test", "-dedicated", "1",
        "-psquake-root", PSQ_TEST_ROOT,
        "+developer", "1", "+psq_debug", "1", "+psq_testarena", "1",
        "+map", "lq_e1m1", NULL };
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
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-dedicated") && i + 1 < argc && strcmp(argv[i + 1], "1"))
            gate_closed_run = 1;
    return psq_engine_main(argc, argv);
}
