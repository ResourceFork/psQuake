/*
 * psQuake -- host tests for the process-monster policy (src/core/psq_policy.c)
 * against psproc's fake backend.
 *
 * The requirement (docs/architecture.md, phase 2): every process query and
 * action is a no-op unless the game is local single player; QuakeC can act
 * only on a pid the latest poll handed out, and only as the identity that
 * poll recorded; wound renices and kill signals only as the kill policy
 * allows (default renice-only), each through the pid-reuse-checked wrappers;
 * the ladder follows the data's available classes with promote-fast /
 * demote-slow hysteresis; fork bursts owe a capped swarm; killed processes
 * stay killed for the level.
 *
 * Returns 0 if every check passes, 1 otherwise.
 */

#include "psq_policy.h"

#include "proc_fake.h"
#include "proc_select.h"
#include "psproc_options.h"

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
    psproc_all_users = 0;
    psq_policy_level_reset(models);
    psq_policy_set_gate(1);
    now = 10.0;
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
    CHECK(poll_with(a, 4) == 2, "pool keeps our two processes");
    CHECK(pool_index(500) >= 0 && pool_index(503) >= 0, "500 and 503 listed");
    CHECK(pool_index(501) < 0, "another user's process is not listed");
    CHECK(pool_index(502) < 0, "a protected process is not listed");

    /* Within the interval the pool is reused and the backend is not read. */
    proc_fake_push_snapshot(b, 1);
    CHECK(psq_policy_poll(now + 0.5) == 2, "throttled poll keeps the pool");
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
    CHECK(r.outcome == PSQ_OUT_POLICY, "simulate policy: no renice");
    psproc_kill_policy = PSD_KILL_LIVE;
    r = psq_policy_wound(500, 12);
    CHECK(r.outcome == PSQ_OUT_ACTED, "live policy: wound renices");
    CHECK(proc_fake_renice_count() == 2, "two renices in all");
    CHECK(proc_fake_kill_count() == 0, "wounds never kill");
}

static void test_kill_renice_policy(void)
{
    psd_proc_t a[] = { mine(500, MB(10)), mine(501, MB(10)) };
    psq_result_t r;

    setup(all_models);
    poll_with(a, 2);
    r = psq_policy_kill(500);
    CHECK(r.outcome == PSQ_OUT_POLICY, "renice policy: kill sends nothing");
    CHECK(proc_fake_kill_count() == 0, "no SIGTERM under renice policy");
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
    /* Trusted session: anything goes. */
    CHECK(psq_option_change_allowed(0, PSD_KILL_RENICE, PSD_KILL_LIVE, 0, 1),
          "a local session may arm live kills and all users");
    /* After a server or demo stuffed commands: only down. */
    CHECK(!psq_option_change_allowed(1, PSD_KILL_RENICE, PSD_KILL_LIVE, 0, 0),
          "untrusted: renice to live refused");
    CHECK(!psq_option_change_allowed(1, PSD_KILL_SIMULATE, PSD_KILL_RENICE, 0, 0),
          "untrusted: simulate to renice refused");
    CHECK(!psq_option_change_allowed(1, PSD_KILL_RENICE, PSD_KILL_RENICE, 0, 1),
          "untrusted: all users refused");
    CHECK(psq_option_change_allowed(1, PSD_KILL_LIVE, PSD_KILL_SIMULATE, 1, 0),
          "untrusted: lowering is allowed");
    CHECK(psq_option_change_allowed(1, PSD_KILL_RENICE, PSD_KILL_RENICE, 0, 0),
          "untrusted: no change is allowed");
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
    CHECK(psq_policy_kill(500).outcome == PSQ_OUT_POLICY, "script entity killed");
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
    test_swarm_global_cap();
    test_option_changes();
    test_qc_commands();
    test_level_allowed();
    test_script_identity_killed();
    test_killed_set_full();

    printf("psq_policy_test: %d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}
