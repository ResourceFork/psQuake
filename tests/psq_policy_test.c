/*
 * psQuake -- host tests for the process-monster policy (src/core/psq_policy.c)
 * against psproc's fake backend.
 *
 * The requirement (docs/architecture.md, phase 2): every process query and
 * action is a no-op unless the game is local single player; QuakeC can act
 * only on a pid the latest poll handed out, and only as the identity that
 * poll recorded; every wound sends post and every death delete, in every
 * kill mode, with the mode it maps to (dryrun, post-only, delete), each
 * through the pid-reuse-checked wrappers; the pool keeps the backend's
 * order; a grade picks the class over the classes the data has;
 * the ladder follows the data's available classes with promote-fast /
 * demote-slow hysteresis; fork bursts owe a capped swarm; killed processes
 * stay killed for the level.
 *
 * Returns 0 if every check passes, 1 otherwise.
 */

#include "psq_policy.h"

#include "proc_backend.h"
#include "proc_fake.h"
#include "proc_script.h"
#include "proc_select.h"
#include "psproc_options.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

#define UID 501
#define MB(n) ((unsigned long long) (n) << 20)

static psd_proc_t mk(int pid, int ppid, unsigned int uid,
                     unsigned long long start, const char *name,
                     unsigned long long mem, int cpu)
{
    psd_proc_t p;

    memset(&p, 0, sizeof(p));
    p.pid = pid;
    p.ppid = ppid;
    p.uid = uid;
    p.start_token = start;
    p.footprint = mem;
    p.cpu_percent = cpu;
    p.is_daemon = 0;
    strncpy(p.name, name, sizeof(p.name) - 1);
    return p;
}

static psd_proc_t mine(int pid, unsigned long long mem)
{
    char name[32];

    snprintf(name, sizeof(name), "app%d", pid);
    return mk(pid, 0, UID, 1000 + (unsigned long long) pid, name, mem, 0);
}

static int all_models(const char *m) { (void) m; return 1; }

/* The models a data set without the registered classes has. */
static int base_models(const char *m)
{
    return strcmp(m, "progs/enforcer.mdl") != 0
        && strcmp(m, "progs/hknight.mdl") != 0
        && strcmp(m, "progs/shalrath.mdl") != 0;
}

static double now;

/* Fresh fake backend, a new level, the gate open, renice-only policy. */
static void setup(int (*models)(const char *))
{
    proc_fake_install(UID);
    psd_select_init();
    psproc_kill_policy = PSD_KILL_RENICE;
    psproc_classify_by = PSD_CLASSIFY_MEM;
    psproc_monster_cap = PSD_MONSTER_CAP_MAX;   /* the pool is the count */
    psq_policy_level_reset(models);
    psq_policy_set_gate(1);
    now = 10.0;
}

/* A refresh through the script backend: its poll runs on a worker thread,
 * so think again (same game time, as a stalled think would) until the
 * refresh lands, up to 5 s. */
static int poll_script(double t)
{
    int before = psq_policy_poll_count();
    int n      = 0;
    int waited;

    for (waited = 0; waited < 5000; waited += 5)
    {
        n = psq_policy_poll(t);
        if (psq_policy_poll_count() != before)
        {
            break;
        }
        usleep(5000);
    }
    return n;
}

/* Serve `procs` to the next refresh and run one. */
static int poll_with(const psd_proc_t *procs, int n)
{
    proc_fake_push_snapshot(procs, n);
    now += 1.0;
    return psq_policy_poll(now);
}

static int pool_index(int pid)
{
    int i;
    const psd_proc_t *e;

    for (i = 0; (e = psq_policy_entry(i)) != NULL; i++)
    {
        if (e->pid == pid)
        {
            return i;
        }
    }
    return -1;
}

static const char *class_of(int pid)
{
    int i = pool_index(pid);
    return i < 0 ? "(absent)" : psq_policy_class(i);
}

/* ------------------------------------------------------------------ gate */

static void test_gate_matrix(void)
{
    int a, m, d, c, ded;

    for (a = 0; a <= 1; a++)
    for (m = 0; m <= 4; m++)
    for (d = 0; d <= 1; d++)
    for (c = 0; c <= 1; c++)
    for (ded = 0; ded <= 1; ded++) {
        psq_gate_facts f;
        int want = a && m == 1 && !d && !c && !ded;

        f.server_active = a;
        f.maxclients = m;
        f.deathmatch = d;
        f.coop = c;
        f.dedicated = ded;
        CHECK(psq_gate_allows(&f) == want,
              "gate(active=%d maxclients=%d dm=%d coop=%d dedicated=%d) != %d",
              a, m, d, c, ded, want);
    }
    CHECK(!psq_gate_allows(NULL), "gate(NULL) must be closed");
}

static void test_gate_closed_does_nothing(void)
{
    psd_proc_t procs[] = { mine(500, MB(10)), mine(501, MB(100)) };
    psq_result_t r;

    setup(all_models);
    psproc_kill_policy = PSD_KILL_LIVE;
    CHECK(poll_with(procs, 2) == 2, "open gate lists both");
    CHECK(psq_policy_level_active(), "a gated-open poll marks the level active");

    psq_policy_set_gate(0);
    CHECK(psq_policy_poll(now + 5) == 0, "closed gate: poll returns 0");
    CHECK(psq_policy_entry(0) == NULL, "closed gate: no entries");
    CHECK(strcmp(psq_policy_name(0), "") == 0, "closed gate: no names");
    CHECK(strcmp(psq_policy_class(0), "") == 0, "closed gate: no classes");
    CHECK(!psq_policy_present(500), "closed gate: nothing present");
    CHECK(psq_policy_burst(500) == 0, "closed gate: no bursts");
    r = psq_policy_wound(500, 10);
    CHECK(r.outcome == PSQ_OUT_GATED, "closed gate: wound gated (%d)", r.outcome);
    r = psq_policy_kill(500);
    CHECK(r.outcome == PSQ_OUT_GATED, "closed gate: kill gated (%d)", r.outcome);
    r = psq_policy_kill(PSQ_SWARM_PID);
    CHECK(r.outcome == PSQ_OUT_GATED, "closed gate: swarm report gated");
    CHECK(proc_fake_renice_count() == 0 && proc_fake_kill_count() == 0,
          "closed gate: no backend action (renice %d kill %d)",
          proc_fake_renice_count(), proc_fake_kill_count());

    /* A level load closes the gate and forgets everything. */
    psq_policy_set_gate(1);
    poll_with(procs, 2);
    psq_policy_level_reset(all_models);
    CHECK(!psq_policy_level_active(), "level reset clears the active flag");
    CHECK(psq_policy_poll(now + 5) == 0, "level reset closes the gate");
}

/* ------------------------------------------------------------------ pool */

static void test_pool_and_throttle(void)
{
    psd_proc_t a[] = {
        mine(500, MB(10)),
        mk(501, 0, 999, 1501, "other-user", MB(10), 0),
        mk(502, 0, UID, 1502, "Finder", MB(10), 0),
        mine(503, MB(10)),
    };
    psd_proc_t b[] = { mine(600, MB(10)) };

    setup(all_models);
    /* The backend (Summoner's trusted plugin) curates: psQuake filters
     * nothing it lists, not another uid and not a protected name. */
    CHECK(poll_with(a, 4) == 4, "pool keeps every listed entity");
    CHECK(pool_index(500) == 0 && pool_index(501) == 1 && pool_index(502) == 2
          && pool_index(503) == 3, "all four, in the backend's order");

    /* Within the interval the pool is reused and the backend is not read. */
    proc_fake_push_snapshot(b, 1);
    CHECK(psq_policy_poll(now + 0.5) == 4, "throttled poll keeps the pool");
    CHECK(pool_index(500) >= 0 && pool_index(600) < 0, "throttled: same pool");
    CHECK(psq_policy_poll(now + 1.0) == 1, "next interval refreshes");
    CHECK(pool_index(600) >= 0 && pool_index(500) < 0, "refreshed pool");
}

static void test_names(void)
{
    psd_proc_t a[] = { mk(500, 0, UID, 7, "bad\x01name\x7f", MB(1), 0) };
    const char *n1;
    const char *n2;

    setup(all_models);
    poll_with(a, 1);
    n1 = psq_policy_name(0);
    CHECK(strcmp(n1, "bad?name?") == 0, "name sanitized: '%s'", n1);
    poll_with(a, 1);
    n2 = psq_policy_name(0);
    CHECK(n1 == n2, "same process, same name pointer across polls");
    CHECK(strcmp(psq_policy_name(5), "") == 0, "out of range name is empty");
}

/* --------------------------------------------------------------- actions */

static void test_wound(void)
{
    psd_proc_t a[] = { mine(500, MB(10)) };
    psq_result_t r;

    setup(all_models);
    poll_with(a, 1);

    r = psq_policy_wound(500, 12);
    CHECK(r.outcome == PSQ_OUT_ACTED && r.act == PSD_ACT_DONE,
          "renice policy: wound reniced (%d/%d)", r.outcome, r.act);
    CHECK(proc_fake_renice_count() == 1 && proc_fake_last_renice_pid() == 500
          && proc_fake_last_renice_delta() == PSQ_WOUND_NICE,
          "renice of 500 by %d", PSQ_WOUND_NICE);

    r = psq_policy_wound(500, 0);
    CHECK(r.outcome == PSQ_OUT_NO_DAMAGE, "no damage, no renice");
    r = psq_policy_wound(777, 12);
    CHECK(r.outcome == PSQ_OUT_UNKNOWN, "a pid not handed out is refused");
    r = psq_policy_wound(1, 12);
    CHECK(r.outcome == PSQ_OUT_NOT_PROCESS, "pid 1 is never a target");
    r = psq_policy_wound(PSQ_SWARM_PID, 12);
    CHECK(r.outcome == PSQ_OUT_NOT_PROCESS, "a swarm member is never a target");
    CHECK(proc_fake_renice_count() == 1, "none of those reniced");

    psproc_kill_policy = PSD_KILL_SIMULATE;
    r = psq_policy_wound(500, 12);
    CHECK(r.outcome == PSQ_OUT_ACTED, "simulate policy: the wound is still sent (dryrun)");
    psproc_kill_policy = PSD_KILL_LIVE;
    r = psq_policy_wound(500, 12);
    CHECK(r.outcome == PSQ_OUT_ACTED, "live policy: the wound is sent");
    CHECK(proc_fake_renice_count() == 3, "three wounds sent in all");
    CHECK(proc_fake_kill_count() == 0, "wounds never kill");
}

static void test_kill_renice_policy(void)
{
    psd_proc_t a[] = { mine(500, MB(10)), mine(501, MB(10)) };
    psq_result_t r;

    setup(all_models);
    poll_with(a, 2);
    r = psq_policy_kill(500);
    CHECK(r.outcome == PSQ_OUT_ACTED && r.act == PSD_ACT_DONE,
          "renice policy: the death is sent (post-only: the responder deletes nothing)");
    CHECK(proc_fake_kill_count() == 1 && proc_fake_last_kill_pid() == 500,
          "one delete event, for 500");
    CHECK(!psq_policy_present(500), "a killed process is no longer present");
    CHECK(strcmp(class_of(500), "") == 0,
          "until the next poll it is still listed, with no class to spawn");
    CHECK(psq_policy_present(501), "the other one is");
    r = psq_policy_kill(500);
    CHECK(r.outcome == PSQ_OUT_KILLED, "a second kill does nothing");
    r = psq_policy_wound(500, 5);
    CHECK(r.outcome == PSQ_OUT_KILLED, "a killed process is not reniced");

    poll_with(a, 2);
    CHECK(pool_index(500) < 0, "killed process stays out of the pool");
    CHECK(pool_index(501) >= 0, "the other stays in");

    psq_policy_level_reset(all_models);
    psq_policy_set_gate(1);
    poll_with(a, 2);
    CHECK(pool_index(500) >= 0, "a new level forgets the kill");
}

static void test_kill_live_policy(void)
{
    psd_proc_t a[] = { mine(500, MB(10)), mine(501, MB(10)) };
    psd_proc_t live_only_501[] = { mine(501, MB(10)) };
    psq_result_t r;

    setup(all_models);
    psproc_kill_policy = PSD_KILL_LIVE;
    poll_with(a, 2);
    r = psq_policy_kill(500);
    CHECK(r.outcome == PSQ_OUT_ACTED && r.act == PSD_ACT_DONE,
          "live policy: kill acted (%d/%d)", r.outcome, r.act);
    CHECK(proc_fake_kill_count() == 1 && proc_fake_last_kill_pid() == 500,
          "SIGTERM to 500");
    CHECK(!psq_policy_present(500), "killed: not present");

    /* 501 exits between the poll and the kill: nothing is signalled. */
    proc_fake_set_live(NULL, 0);
    r = psq_policy_kill(501);
    CHECK(r.outcome == PSQ_OUT_ACTED && r.act == PSD_ACT_GONE,
          "exited process: kill found it gone (%d/%d)", r.outcome, r.act);
    CHECK(proc_fake_kill_count() == 1, "no SIGTERM for an exited process");
    CHECK(!psq_policy_present(501), "gone: not present (its monster retires)");
    r = psq_policy_kill(501);
    CHECK(r.outcome == PSQ_OUT_GONE, "gone: further kills refused");
    (void) live_only_501;
}

static void test_pid_reuse(void)
{
    psd_proc_t a[] = { mine(500, MB(10)) };
    psd_proc_t reused[] = { mk(500, 0, UID, 9999, "intruder", MB(10), 0) };
    psq_result_t r;

    setup(all_models);
    psproc_kill_policy = PSD_KILL_LIVE;
    poll_with(a, 1);

    /* Between polls the pid is handed to another process. */
    proc_fake_set_live(reused, 1);
    r = psq_policy_wound(500, 5);
    CHECK(r.outcome == PSQ_OUT_ACTED && r.act == PSD_ACT_REUSED,
          "reused pid: renice refused by the identity check (%d/%d)",
          r.outcome, r.act);
    r = psq_policy_kill(500);
    CHECK(r.outcome == PSQ_OUT_GONE, "reused pid: kill refused (%d)", r.outcome);
    CHECK(proc_fake_renice_count() == 0 && proc_fake_kill_count() == 0,
          "the new owner of pid 500 got nothing");
    CHECK(!psq_policy_present(500), "the old monster's process is not present");

    /* The poll that sees the new owner holds it out once ... */
    poll_with(reused, 1);
    CHECK(pool_index(500) < 0, "new owner of a reused pid held out one poll");
    /* ... then admits it as itself. */
    poll_with(reused, 1);
    CHECK(pool_index(500) >= 0, "new owner admitted on the next poll");
    proc_fake_set_live(reused, 1);
    r = psq_policy_wound(500, 5);
    CHECK(r.outcome == PSQ_OUT_ACTED && r.act == PSD_ACT_DONE,
          "the new owner is acted on as itself");
}

static void test_reuse_seen_by_poll(void)
{
    psd_proc_t a[] = { mine(500, MB(10)) };
    psd_proc_t reused[] = { mk(500, 0, UID, 4242, "newcomer", MB(10), 0) };

    setup(all_models);
    poll_with(a, 1);
    CHECK(psq_policy_present(500), "500 present");
    poll_with(reused, 1);
    CHECK(!psq_policy_present(500),
          "a poll that sees pid 500 under a new identity drops it");
    CHECK(psq_policy_wound(500, 5).outcome == PSQ_OUT_UNKNOWN,
          "and nothing can act on pid 500 that poll");
}

static void test_duplicates(void)
{
    psd_proc_t a[] = { mine(500, MB(10)), mine(500, MB(10)) };

    setup(all_models);
    CHECK(poll_with(a, 2) <= 1, "one pid listed twice is pooled once");
}

static void test_pid_range(void)
{
    psd_proc_t a[] = {
        mk(PSQ_PID_MAX, 0, UID, 0, "edge", MB(1), 0),
        mk(PSQ_PID_MAX + 1, 0, UID, 0, "too-big", MB(1), 0),
    };

    setup(all_models);
    poll_with(a, 2);
    CHECK(pool_index(PSQ_PID_MAX) >= 0, "2^24 is handed out");
    CHECK(pool_index(PSQ_PID_MAX + 1) < 0,
          "an id a QuakeC float cannot hold exactly is never handed out");
    CHECK(psq_policy_poll_count() == 1, "one refresh counted");
}

/* ---------------------------------------------------------------- ladder */

static void test_ladder(void)
{
    static const struct { unsigned long long mem; const char *full; const char *base; } cases[] = {
        { MB(1),    "monster_dog",         "monster_dog" },
        { MB(16),   "monster_army",        "monster_army" },
        { MB(50),   "monster_enforcer",    "monster_army" },
        { MB(100),  "monster_knight",      "monster_knight" },
        { MB(300),  "monster_ogre",        "monster_ogre" },
        { MB(400),  "monster_hell_knight", "monster_ogre" },
        { MB(600),  "monster_demon1",      "monster_demon1" },
        { MB(1200), "monster_shalrath",    "monster_demon1" },
        { MB(4096), "monster_shambler",    "monster_shambler" },
    };
    unsigned i;
    int t;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        psd_proc_t p[1];

        p[0] = mine(500, cases[i].mem);
        setup(all_models);
        poll_with(p, 1);
        CHECK(strcmp(class_of(500), cases[i].full) == 0,
              "full data, %llu MB -> %s, want %s", cases[i].mem >> 20,
              class_of(500), cases[i].full);
        setup(base_models);
        poll_with(p, 1);
        CHECK(strcmp(class_of(500), cases[i].base) == 0,
              "base data, %llu MB -> %s, want %s", cases[i].mem >> 20,
              class_of(500), cases[i].base);
    }

    /* CPU classification. */
    {
        psd_proc_t p[] = { mk(500, 0, UID, 1, "busy", MB(1), 130) };

        setup(all_models);
        psproc_classify_by = PSD_CLASSIFY_CPU;
        poll_with(p, 1);
        CHECK(strcmp(class_of(500), "monster_demon1") == 0,
              "130%% CPU -> %s", class_of(500));
    }

    setup(all_models);
    for (t = 0; *psq_policy_tier_class(t) != '\0'; t++)
        ;
    CHECK(t == 9, "full data has 9 ladder classes, got %d", t);
    CHECK(strcmp(psq_policy_tier_class(-1), "monster_wizard") == 0, "swarm class");
    setup(base_models);
    for (t = 0; *psq_policy_tier_class(t) != '\0'; t++)
        ;
    CHECK(t == 6, "base data has 6 ladder classes, got %d", t);
    CHECK(strcmp(psq_policy_tier_class(0), "monster_dog") == 0
          && strcmp(psq_policy_tier_class(5), "monster_shambler") == 0,
          "base ladder runs dog to shambler");
    CHECK(strcmp(psq_policy_tier_class(-2), "") == 0, "t < -1 is empty");
}

static int no_dog_or_wizard(const char *m)
{
    return strcmp(m, "progs/dog.mdl") != 0 && strcmp(m, "progs/wizard.mdl") != 0;
}

static int no_models(const char *m) { (void) m; return 0; }

static void test_ladder_degrades(void)
{
    psd_proc_t p[] = { mine(500, MB(1)) };

    setup(no_dog_or_wizard);
    poll_with(p, 1);
    CHECK(strcmp(class_of(500), "monster_army") == 0,
          "no lighter class: the next heavier available (%s)", class_of(500));
    CHECK(strcmp(psq_policy_tier_class(-1), "") == 0, "no wizard: no swarm class");

    setup(no_models);
    poll_with(p, 1);
    CHECK(strcmp(class_of(500), "") == 0, "no classes at all: empty class");
}

static void test_hysteresis(void)
{
    psd_proc_t small[] = { mine(500, MB(1)) };
    psd_proc_t big[] = { mine(500, MB(300)) };
    int i;

    setup(all_models);
    poll_with(small, 1);
    CHECK(strcmp(class_of(500), "monster_dog") == 0, "starts as a dog");
    poll_with(big, 1);
    CHECK(strcmp(class_of(500), "monster_dog") == 0, "one poll big: still a dog");
    poll_with(big, 1);
    CHECK(strcmp(class_of(500), "monster_ogre") == 0,
          "two polls big: promoted (%s)", class_of(500));

    for (i = 1; i < PSQ_REGRADE_DOWN_HOLD; i++) {
        poll_with(small, 1);
        CHECK(strcmp(class_of(500), "monster_ogre") == 0,
              "%d polls small: still an ogre", i);
    }
    poll_with(small, 1);
    CHECK(strcmp(class_of(500), "monster_dog") == 0,
          "%d polls small: demoted (%s)", PSQ_REGRADE_DOWN_HOLD, class_of(500));

    /* A flapping metric never moves it. */
    for (i = 0; i < 10; i++) {
        poll_with(i % 2 ? small : big, 1);
        CHECK(strcmp(class_of(500), "monster_dog") == 0, "flapping: stays a dog");
    }
}

/* ----------------------------------------------------------------- swarm */

/* Parent 600 (ours) with `kids` children owned by another user, so the
 * children are counted but never pooled. */
static int family(psd_proc_t *out, int kids, int first_kid)
{
    int i;

    out[0] = mine(600, MB(10));
    for (i = 0; i < kids; i++)
        out[1 + i] = mk(first_kid + i, 600, 999, 50 + (unsigned long long) i,
                        "kid", MB(1), 0);
    return 1 + kids;
}

/* The pool is only the monster count, so a forking parent's children are
 * not in it; the backend's reported child count (children=) still shows
 * the burst. */
static void test_swarm_from_reported_children(void)
{
    psd_proc_t p[1];

    setup(all_models);
    psproc_monster_cap = PSD_MONSTER_CAP_MIN;
    p[0] = mine(600, MB(10));
    p[0].has_children = 1;
    p[0].children = 2;
    poll_with(p, 1);
    CHECK(psq_policy_burst(600) == 0, "first sight: baseline");
    p[0].children = 40;
    poll_with(p, 1);
    CHECK(psq_policy_burst(600) == PSQ_SWARM_PER_BURST,
          "a reported jump of 38 children owes a swarm, none of them listed");
    psproc_monster_cap = PSD_MONSTER_CAP_MAX;
}

static void test_swarm(void)
{
    psd_proc_t fam[64];
    int n;
    int i;
    psq_result_t r;

    setup(all_models);
    n = family(fam, 0, 700);
    poll_with(fam, n);
    CHECK(psq_policy_burst(600) == 0, "first sight: baseline, no burst");

    n = family(fam, 4, 700);
    poll_with(fam, n);
    CHECK(psq_policy_burst(600) == 0, "+4 children is not a burst");

    n = family(fam, 14, 700);
    poll_with(fam, n);
    CHECK(psq_policy_burst(600) == PSQ_SWARM_PER_BURST,
          "+10 children owes the per-burst cap");
    CHECK(psq_policy_burst(600) == 0, "owed members are consumed once");
    CHECK(psq_policy_swarm_live() == PSQ_SWARM_PER_BURST, "swarm counted");

    n = family(fam, 30, 700);
    poll_with(fam, n);
    CHECK(psq_policy_burst(600) == 0, "cooldown: no second swarm at once");
    for (i = 0; i < PSQ_SWARM_COOLDOWN; i++) {
        n = family(fam, 0, 700);
        poll_with(fam, n);
    }
    n = family(fam, 20, 700);
    poll_with(fam, n);
    CHECK(psq_policy_burst(600) == PSQ_SWARM_PER_BURST, "after cooldown: swarms again");

    r = psq_policy_kill(PSQ_SWARM_PID);
    CHECK(r.outcome == PSQ_OUT_SWARM, "a swarm member's end returns budget");
    CHECK(psq_policy_swarm_live() == 2 * PSQ_SWARM_PER_BURST - 1, "count drops");
    CHECK(proc_fake_kill_count() == 0 && proc_fake_renice_count() == 0,
          "swarm members act on nothing");

    CHECK(psq_policy_burst(777) == 0, "an unknown pid owes nothing");
}

static void test_option_changes(void)
{
    /* After a server or demo stuffed commands, a flag only goes off. (The
     * kill mode's own rule is psq_killmode_change.) */
    CHECK(psq_flag_change_allowed(0, 0, 1), "a local session may turn infighting on");
    CHECK(!psq_flag_change_allowed(1, 0, 1), "untrusted: infighting on refused");
    CHECK(psq_flag_change_allowed(1, 1, 0), "untrusted: infighting off allowed");
    CHECK(psq_flag_change_allowed(1, 1, 1) && psq_flag_change_allowed(1, 0, 0),
          "untrusted: no change allowed");
}

/* Stuffed text that runs nothing leaves the session trusted; anything that
 * could run a command does not. */
static void test_stuff_text_inert(void)
{
    CHECK(psq_stuff_text_inert("//st 15 0\n"), "LibreQuake's demo comment is inert");
    CHECK(psq_stuff_text_inert("") && psq_stuff_text_inert("\n\n") && psq_stuff_text_inert("  \t\n"),
          "empty and blank text is inert");
    CHECK(psq_stuff_text_inert("  // indented\n//two\n// three"), "comment lines are inert");
    CHECK(!psq_stuff_text_inert(NULL), "NULL is not inert");
    CHECK(!psq_stuff_text_inert("psq_allusers 1\n"), "a command is not inert");
    CHECK(!psq_stuff_text_inert("// a ; psq_allusers 1\n"), "a ';' after a comment is not inert");
    CHECK(!psq_stuff_text_inert("//c\npsq_allusers 1\n"), "a command on a later line is not");
    CHECK(!psq_stuff_text_inert("/ /x\n") && !psq_stuff_text_inert("/\n"),
          "a single slash is not a comment");
    CHECK(!psq_stuff_text_inert("\"//\"\n"), "a quoted // is a token");
    CHECK(!psq_stuff_text_inert("bf\n"), "even a stock command is not inert");
    CHECK(psq_stuff_text_inert("\r//x\r\n"), "a carriage return is white space");
}

/* The options page's kill-mode rule: live only through the confirmed
 * warning, lowering always, raising never once untrusted. */
static void test_killmode_change(void)
{
    int cur, req, conf, untr;

    CHECK(psq_killmode_change(PSD_KILL_RENICE, PSD_KILL_LIVE, 0, 0) == PSQ_KILLMODE_CONFIRM,
          "renice -> live without the warning confirmed asks for it");
    CHECK(psq_killmode_change(PSD_KILL_SIMULATE, PSD_KILL_LIVE, 0, 0) == PSQ_KILLMODE_CONFIRM,
          "simulate -> live without the warning confirmed asks for it");
    CHECK(psq_killmode_change(PSD_KILL_RENICE, PSD_KILL_LIVE, 1, 0) == PSQ_KILLMODE_CHANGED,
          "renice -> live with the warning confirmed changes");
    CHECK(psq_killmode_change(PSD_KILL_RENICE, PSD_KILL_LIVE, 1, 1) == PSQ_KILLMODE_UNTRUSTED,
          "untrusted: even a confirmed live is refused");
    CHECK(psq_killmode_change(PSD_KILL_SIMULATE, PSD_KILL_RENICE, 0, 1) == PSQ_KILLMODE_UNTRUSTED,
          "untrusted: raising simulate to renice is refused");
    CHECK(psq_killmode_change(PSD_KILL_SIMULATE, PSD_KILL_RENICE, 0, 0) == PSQ_KILLMODE_CHANGED,
          "raising simulate to renice needs no warning");
    CHECK(psq_killmode_change(PSD_KILL_LIVE, PSD_KILL_SIMULATE, 0, 1) == PSQ_KILLMODE_CHANGED,
          "untrusted: live -> simulate is allowed");
    CHECK(psq_killmode_change(PSD_KILL_LIVE, PSD_KILL_LIVE, 0, 1) == PSQ_KILLMODE_UNCHANGED,
          "live -> live is no change");
    CHECK(psq_killmode_change(PSD_KILL_RENICE, 3, 1, 0) == PSQ_KILLMODE_INVALID
          && psq_killmode_change(PSD_KILL_RENICE, -1, 1, 0) == PSQ_KILLMODE_INVALID
          && psq_killmode_change(7, PSD_KILL_SIMULATE, 1, 0) == PSQ_KILLMODE_INVALID,
          "modes outside 0..2 are invalid");
    /* Exhaustive: no input reaches live from below without the confirmed
     * warning in a trusted session. */
    for (cur = -1; cur <= PSD_KILL_NUM; cur++)
        for (req = -1; req <= PSD_KILL_NUM; req++)
            for (conf = 0; conf <= 1; conf++)
                for (untr = 0; untr <= 1; untr++) {
                    psq_killmode_result_t r = psq_killmode_change(cur, req, conf, untr);
                    if (r == PSQ_KILLMODE_CHANGED && req == PSD_KILL_LIVE)
                        CHECK(conf && !untr && cur >= 0 && cur < PSD_KILL_LIVE,
                              "live reached only confirmed and trusted (%d %d %d %d)",
                              cur, req, conf, untr);
                    if (r == PSQ_KILLMODE_CHANGED && req > cur)
                        CHECK(!untr, "a raise only when trusted (%d %d %d %d)",
                              cur, req, conf, untr);
                }
}

static void test_qc_commands(void)
{
    /* The stock game code's own commands stay trusted. */
    CHECK(psq_qc_command_trusted("bf\n") && psq_qc_command_trusted("-attack\n")
          && psq_qc_command_trusted("restart\n"), "stock QuakeC commands are trusted");
    /* Anything else marks the session, fragments included. */
    CHECK(!psq_qc_command_trusted("psq_killpolicy 2\n"), "a policy raise is not trusted");
    CHECK(!psq_qc_command_trusted("psq_kill"), "a fragment is not trusted");
    CHECK(!psq_qc_command_trusted("bf\npsq_allusers 1\n"), "a stock prefix is not enough");
    CHECK(!psq_qc_command_trusted("bf"), "an unterminated stock command is not trusted");
    CHECK(!psq_qc_command_trusted("BF\n"), "matching is exact");
    CHECK(!psq_qc_command_trusted(""), "empty text is not trusted");
    CHECK(!psq_qc_command_trusted(NULL), "NULL is not trusted");
}

/* changelevel's map name reaches the console buffer: only plain names. */
static void test_map_names(void)
{
    char longname[PSQ_MAP_NAME_MAX + 1];

    CHECK(psq_map_name_plain("lq_e1m1") && psq_map_name_plain("e1m1")
          && psq_map_name_plain("psqarena") && psq_map_name_plain("start"),
          "stock and LibreQuake map names are plain");
    CHECK(psq_map_name_plain("ad_tears-2.v1") && psq_map_name_plain("dir/map"),
          "dashes, dots and a subfolder are plain");
    CHECK(!psq_map_name_plain("start;psq_killpolicy 2"), "a ';' is refused");
    CHECK(!psq_map_name_plain("start\npsq_allusers 1"), "a newline is refused");
    CHECK(!psq_map_name_plain("start psq_infighting"), "a space is refused");
    CHECK(!psq_map_name_plain("\"start\""), "a quote is refused");
    CHECK(!psq_map_name_plain("start\t") && !psq_map_name_plain("st\x1b" "art"),
          "control characters are refused");
    CHECK(!psq_map_name_plain("../id1/start") && !psq_map_name_plain("/start"),
          "\"..\" and a leading '/' are refused");
    CHECK(!psq_map_name_plain("") && !psq_map_name_plain(NULL), "empty and NULL are refused");
    memset(longname, 'a', sizeof(longname) - 1);
    longname[PSQ_MAP_NAME_MAX - 1] = '\0';
    CHECK(psq_map_name_plain(longname), "%d characters are plain", PSQ_MAP_NAME_MAX - 1);
    longname[PSQ_MAP_NAME_MAX - 1] = 'a';
    longname[PSQ_MAP_NAME_MAX] = '\0';
    CHECK(!psq_map_name_plain(longname), "%d characters are refused", PSQ_MAP_NAME_MAX);
}

static void test_level_allowed(void)
{
    CHECK(psq_level_allowed(1, 0, 0) && psq_level_allowed(1, 1, 1),
          "the arena always allows process monsters");
    CHECK(psq_level_allowed(0, 1, 0), "a test arena the user turned on allows them");
    CHECK(!psq_level_allowed(0, 0, 0), "any other level does not");
    CHECK(!psq_level_allowed(0, 1, 1), "nor a test arena once the session is untrusted");
}

static void test_script_identity_killed(void)
{
    /* A script entity (no start token) is identified by its id alone: a
     * relabel after its kill must not bring it back. */
    psd_proc_t before[] = { mk(500, 0, UID, 0, "job-a", MB(1), 0) };
    psd_proc_t relabelled[] = { mk(500, 0, UID, 0, "job-a-renamed", MB(1), 0) };

    setup(all_models);
    poll_with(before, 1);
    CHECK(psq_policy_kill(500).outcome == PSQ_OUT_ACTED, "script entity killed");
    poll_with(relabelled, 1);
    CHECK(pool_index(500) < 0, "relabelled script entity stays killed");
}

static void test_killed_set_full(void)
{
    psd_proc_t procs[PSQ_POOL_MAX];
    int round;
    int i;
    int killed = 0;
    psq_result_t r;

    setup(all_models);
    psproc_kill_policy = PSD_KILL_LIVE;
    for (round = 0; killed < PSQ_KILLED_MAX; round++) {
        for (i = 0; i < PSQ_POOL_MAX; i++)
            procs[i] = mine(10000 + round * PSQ_POOL_MAX + i, MB(1));
        poll_with(procs, PSQ_POOL_MAX);
        for (i = 0; i < PSQ_POOL_MAX && killed < PSQ_KILLED_MAX; i++)
            if (psq_policy_kill(10000 + round * PSQ_POOL_MAX + i).outcome
                == PSQ_OUT_ACTED)
                killed++;
    }
    CHECK(proc_fake_kill_count() == PSQ_KILLED_MAX, "%d kills signalled",
          proc_fake_kill_count());
    procs[0] = mine(9000, MB(1));
    poll_with(procs, 1);
    r = psq_policy_kill(9000);
    CHECK(r.outcome == PSQ_OUT_FULL, "killed set full: kill refused (%d)",
          r.outcome);
    CHECK(proc_fake_kill_count() == PSQ_KILLED_MAX,
          "and no SIGTERM it could not remember");
    CHECK(!psq_policy_present(9000), "its monster stays down");
}


/* ------------------------------------------------- Summoner: the host spec */

static void test_mode_mapping(void)
{
    CHECK(strcmp(psq_policy_mode(PSD_KILL_SIMULATE), "dryrun") == 0, "simulate -> dryrun");
    CHECK(strcmp(psq_policy_mode(PSD_KILL_RENICE), "post-only") == 0, "renice -> post-only");
    CHECK(strcmp(psq_policy_mode(PSD_KILL_LIVE), "delete") == 0, "live -> delete");
    CHECK(strcmp(psq_policy_mode(-1), "dryrun") == 0
          && strcmp(psq_policy_mode(PSD_KILL_NUM), "dryrun") == 0,
          "an unknown policy -> dryrun");
}

/* The pool keeps the backend's order (Summoner's trusted plugin sorts and
 * trims): no re-ranking by memory, CPU or name, nothing filtered. */
static void test_order_preserved(void)
{
    psd_proc_t a[] = { mine(700, MB(1)), mine(701, MB(900)), mine(702, MB(50)),
                       mk(703, 0, UID + 7, 0, "other-uid", MB(2), 0),
                       mk(704, 0, UID, 0, "launchd", MB(2), 0) };
    psd_proc_t b[] = { a[3], a[0], a[4], a[2], a[1] };
    int i;

    setup(all_models);
    poll_with(a, 5);
    for (i = 0; i < 5; i++)
        CHECK(pool_index(a[i].pid) == i, "entity %d at %d (got %d)", a[i].pid, i,
              pool_index(a[i].pid));
    poll_with(b, 5);
    for (i = 0; i < 5; i++)
        CHECK(pool_index(b[i].pid) == i, "re-sorted: entity %d at %d (got %d)",
              b[i].pid, i, pool_index(b[i].pid));
}

static void test_grade_picks_class(void)
{
    psd_proc_t g[] = { mine(800, MB(1)), mine(801, MB(1)), mine(802, MB(1)),
                       mine(803, MB(1)) };
    int avail[3] = { 1, 0, 1 };

    g[0].has_grade = 1; g[0].grade = 0.0;
    g[1].has_grade = 1; g[1].grade = 1.0;
    g[2].has_grade = 1; g[2].grade = 0.5;
    g[3].has_grade = 1; g[3].grade = 7.0;   /* clamped by the parser; here too */
    setup(all_models);
    poll_with(g, 4);
    CHECK(strcmp(class_of(800), "monster_dog") == 0, "grade 0: weakest (%s)", class_of(800));
    CHECK(strcmp(class_of(801), "monster_shambler") == 0, "grade 1: toughest (%s)",
          class_of(801));
    CHECK(strcmp(class_of(802), "monster_ogre") == 0, "grade 0.5: tier 4 of 9 (%s)",
          class_of(802));
    CHECK(strcmp(class_of(803), "monster_shambler") == 0, "grade over 1: toughest");

    setup(base_models);   /* 6 rungs drawable */
    poll_with(g, 4);
    CHECK(strcmp(class_of(802), "monster_ogre") == 0, "grade 0.5: tier 3 of 6 (%s)",
          class_of(802));
    CHECK(strcmp(class_of(801), "monster_shambler") == 0, "grade 1 on base data");

    CHECK(psq_grade_rung(0.0, avail, 3) == 0 && psq_grade_rung(0.99, avail, 3) == 2
          && psq_grade_rung(0.49, avail, 3) == 0 && psq_grade_rung(0.5, avail, 3) == 2,
          "grade over the available rungs only");
    avail[0] = avail[2] = 0;
    CHECK(psq_grade_rung(0.5, avail, 3) == -1, "no rung available: none");
}

#ifndef PSQ_POLICY_SCRATCH
#error "PSQ_POLICY_SCRATCH must be defined (tests/CMakeLists.txt)"
#endif

static int pt_write_exec(const char *path, const char *body)
{
    FILE *f = fopen(path, "w");

    if (f == NULL)
        return 0;
    fputs(body, f);
    fclose(f);
    return chmod(path, 0700) == 0;
}

/* The last line of `path` (without its newline), 0 if none. */
static int pt_last_line(const char *path, char *out, size_t cap)
{
    char  line[512];
    FILE *f = fopen(path, "r");
    int   got = 0;

    if (f == NULL)
        return 0;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        line[strcspn(line, "\n")] = '\0';
        snprintf(out, cap, "%s", line);
        got = 1;
    }
    fclose(f);
    return got;
}

static int pt_wait_for(const char *path, const char *want)
{
    char last[512] = "";
    int  waited;

    for (waited = 0; waited < 3000; waited += 20)
    {
        if (pt_last_line(path, last, sizeof(last)) && strcmp(last, want) == 0)
            return 1;
        usleep(20000);
    }
    printf("  last line %s: \"%s\", wanted \"%s\"\n", path, last, want);
    return 0;
}

/* Through the real script backend (a poll script and a respond recorder of
 * the test's own, never Summoner or a real process): every wound sends
 * `post` and every death sends `delete`, in every kill mode, each with the
 * mode the kill mode maps to. A death is a death whatever caused it (the
 * player, a monster, lava, a fall...): QuakeC's Killed calls psq_died for
 * every one, which calls this same psq_policy_kill. */
static void test_events_carry_mode(void)
{
    static const struct { int policy; const char *mode; } m[] = {
        { PSD_KILL_SIMULATE, "dryrun" },
        { PSD_KILL_RENICE,   "post-only" },
        { PSD_KILL_LIVE,     "delete" },
    };
    const char *dir = PSQ_POLICY_SCRATCH;
    char poll[1024], resp[1024], out[1024], body[2048], cmd[1100], want[256];
    size_t i;

    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
    {
        CHECK(0, "cannot create %s", dir);
        return;
    }
    snprintf(poll, sizeof(poll), "%s/poll.sh", dir);
    snprintf(resp, sizeof(resp), "%s/respond.sh", dir);
    snprintf(out, sizeof(out), "%s/respond.txt", dir);
    unlink(out);
    snprintf(body, sizeof(body), "#!/bin/sh\necho \"$*\" >> '%s'\n", out);
    CHECK(pt_write_exec(resp, body), "respond recorder written");
    CHECK(pt_write_exec(poll,
              "#!/bin/sh\n"
              "printf 'id=41\\tlabel=hit\\tgrade=0.2\\n'\n"
              "printf 'id=42\\tlabel=burnt\\tgrade=0.9\\n'\n"),
          "poll script written");
    snprintf(cmd, sizeof(cmd), "'%s'", poll);

    for (i = 0; i < sizeof(m) / sizeof(m[0]); i++)
    {
        psq_result_t r;

        proc_script_install(cmd, resp, 2000);
        psd_select_init();
        psq_policy_level_reset(all_models);
        psq_policy_set_gate(1);
        psproc_kill_policy = m[i].policy;
        now += 1.0;
        CHECK(poll_script(now) == 2, "%s: two entities polled", m[i].mode);
        CHECK(pool_index(41) == 0 && pool_index(42) == 1, "%s: in the script's order",
              m[i].mode);

        r = psq_policy_wound(41, 10);
        CHECK(r.outcome == PSQ_OUT_ACTED && r.act == PSD_ACT_DONE,
              "%s: the wound is sent (%d/%d)", m[i].mode, r.outcome, r.act);
        snprintf(want, sizeof(want), "post 41 amount=%d label=hit mode=%s",
                 PSQ_WOUND_NICE, m[i].mode);
        CHECK(pt_wait_for(out, want), "%s: post carries the mode", m[i].mode);

        r = psq_policy_kill(42);
        CHECK(r.outcome == PSQ_OUT_ACTED && r.act == PSD_ACT_DONE,
              "%s: the death is sent (%d/%d)", m[i].mode, r.outcome, r.act);
        snprintf(want, sizeof(want), "delete 42 label=burnt mode=%s", m[i].mode);
        CHECK(pt_wait_for(out, want), "%s: delete carries the mode", m[i].mode);
        CHECK(!psq_policy_present(42), "%s: the dead one is killed for the level",
              m[i].mode);
    }

    proc_backend_set(NULL);
    unlink(poll);
    unlink(resp);
    unlink(out);
    (void) rmdir(dir);
}

/* Kills that stay listed (a backend that does not drop them, as Summoner
 * does) still never crowd the pool: psQuake leaves them out itself and the
 * pool fills to the count from what follows. */
static void test_kills_leave_the_pool_full(void)
{
    enum { COUNT = 10, KILLS = 10, N = COUNT + KILLS + 5 };
    static psd_proc_t procs[N];
    int i;

    setup(all_models);
    psproc_monster_cap = COUNT;
    psproc_kill_policy = PSD_KILL_SIMULATE;
    for (i = 0; i < N; i++)
        procs[i] = mine(20000 + i, MB(1));
    CHECK(poll_with(procs, N) == COUNT, "the pool fills to the count");
    for (i = 0; i < KILLS; i++)
        CHECK(psq_policy_kill(20000 + i).outcome == PSQ_OUT_ACTED, "kill %d sent", i);
    /* All still listed, the killed first. */
    CHECK(poll_with(procs, N) == COUNT, "after %d kills the pool is still full", KILLS);
    CHECK(pool_index(20000) < 0 && pool_index(20000 + KILLS) == 0
          && pool_index(20000 + KILLS + COUNT - 1) == COUNT - 1
          && pool_index(20000 + KILLS + COUNT) < 0,
          "the killed are left out, the next in line come in");
    psproc_monster_cap = PSD_MONSTER_CAP_MAX;
}

/* The request on the wire: limit= grows with the level's kills, and an id
 * QuakeC cannot hold is counted as left out. */
static void test_limit_on_the_wire(void)
{
    const char *dir = PSQ_POLICY_SCRATCH;
    char poll[1024], args[1024], body[2048], cmd[1100], last[512];

    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
    {
        CHECK(0, "cannot create %s", dir);
        return;
    }
    snprintf(poll, sizeof(poll), "%s/poll-args.sh", dir);
    snprintf(args, sizeof(args), "%s/poll-args.txt", dir);
    unlink(args);
    snprintf(body, sizeof(body),
             "#!/bin/sh\n"
             "echo \"$*\" >> '%s'\n"
             "echo '# dim id=relevance kind=numeric dir=desc roles=sort unit=score label=relevance'\n"
             "printf 'id=51\\tlabel=a\\n'\n"
             "printf 'id=52\\tlabel=b\\n'\n"
             "printf 'id=16777217\\tlabel=big\\n'\n", args);
    CHECK(pt_write_exec(poll, body), "poll script written");
    snprintf(cmd, sizeof(cmd), "'%s'", poll);
    proc_script_install(cmd, NULL, 2000);
    psd_select_init();
    psq_policy_level_reset(all_models);
    psq_policy_set_gate(1);
    psproc_kill_policy = PSD_KILL_SIMULATE;
    psproc_monster_cap = 10;
    now += 1.0;
    CHECK(poll_script(now) == 2, "two usable entities");
    CHECK(psq_policy_ids_too_big() == 1, "one id too big for QuakeC, counted");
    now += 1.0;
    (void) poll_script(now);   /* dims known: the request rides on it */
    CHECK(pt_last_line(args, last, sizeof(last)) && strcmp(last, "limit=10") == 0,
          "limit is the monster count (%s)", last);
    CHECK(psq_policy_kill(51).outcome == PSQ_OUT_ACTED, "one kill");
    /* Summoner drops the ids the game killed and makes up the count
     * itself, so a kill does not change the request: no double count. */
    now += 1.0;
    (void) poll_script(now);
    now += 1.0;
    (void) poll_script(now);
    CHECK(pt_last_line(args, last, sizeof(last)) && strcmp(last, "limit=10") == 0,
          "a kill leaves limit at the monster count (%s)", last);
    /* A lower count reaches the poll after the one already started. */
    psproc_monster_cap = 5;
    now += 1.0;
    (void) poll_script(now);
    now += 1.0;
    (void) poll_script(now);
    CHECK(pt_last_line(args, last, sizeof(last)) && strcmp(last, "limit=5") == 0,
          "a lowered count reaches the wire (%s)", last);
    psproc_monster_cap = PSD_MONSTER_CAP_MAX;
    proc_backend_set(NULL);
    unlink(poll);
    unlink(args);
    (void) rmdir(dir);
}

/* The pool is the player's monster count, so a monster whose process is
 * not in it retires (QuakeC psq_retire teleports it out). Lowering the count
 * from 10 to 5 (the floor) keeps exactly the top 5 by the backend's order and
 * sends no event; raising it to 8 brings in the next 3. */
static void test_pool_is_the_monster_count(void)
{
    psd_proc_t procs[20];
    int i;

    setup(all_models);
    for (i = 0; i < 20; i++)
        procs[i] = mine(30000 + i, MB(1));
    psproc_monster_cap = 10;
    CHECK(poll_with(procs, 20) == 10, "the pool is the count (10)");
    psproc_monster_cap = 5;
    CHECK(poll_with(procs, 20) == 5, "lowered: the pool is 5");
    for (i = 0; i < 20; i++)
        CHECK(psq_policy_present(30000 + i) == (i < 5),
              "rank %d present only in the top 5", i);
    CHECK(proc_fake_renice_count() == 0 && proc_fake_kill_count() == 0,
          "pruning sends no event (%d/%d)", proc_fake_renice_count(),
          proc_fake_kill_count());
    psproc_monster_cap = 8;
    CHECK(poll_with(procs, 20) == 8, "raised: the pool is 8");
    CHECK(pool_index(30007) == 7 && pool_index(30008) < 0,
          "the next 3 in line come in");
    psproc_monster_cap = PSD_MONSTER_CAP_MAX;
}

static void test_swarm_global_cap(void)
{
    psd_proc_t fam[64];
    int n;
    int total = 0;
    int round;
    int i;

    setup(all_models);
    for (round = 0; round < 12; round++) {
        n = family(fam, 0, 700);
        poll_with(fam, n);
        n = family(fam, 40, 700);
        poll_with(fam, n);
        total += psq_policy_burst(600);
        for (i = 0; i < PSQ_SWARM_COOLDOWN; i++) {
            n = family(fam, 40, 700);
            poll_with(fam, n);
        }
    }
    CHECK(total == PSQ_SWARM_MAX_LIVE, "swarm stops at the global cap: %d", total);
    CHECK(psq_policy_swarm_live() == PSQ_SWARM_MAX_LIVE, "live count at the cap");
}

int main(void)
{
    test_gate_matrix();
    test_gate_closed_does_nothing();
    test_pool_and_throttle();
    test_names();
    test_wound();
    test_kill_renice_policy();
    test_kill_live_policy();
    test_pid_reuse();
    test_reuse_seen_by_poll();
    test_duplicates();
    test_pid_range();
    test_ladder();
    test_ladder_degrades();
    test_hysteresis();
    test_swarm();
    test_swarm_from_reported_children();
    test_swarm_global_cap();
    test_option_changes();
    test_killmode_change();
    test_stuff_text_inert();
    test_qc_commands();
    test_map_names();
    test_level_allowed();
    test_script_identity_killed();
    test_killed_set_full();
    test_mode_mapping();
    test_order_preserved();
    test_grade_picks_class();
    test_events_carry_mode();
    test_kills_leave_the_pool_full();
    test_limit_on_the_wire();
    test_pool_is_the_monster_count();

    printf("psq_policy_test: %d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
