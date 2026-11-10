/*
 * psQuake -- host tests for psQuake's adaptations to the imported psproc
 * layer: the prefixed command-line flags, option sanitizing, and the
 * prefixed script-backend environment variables.
 *
 * Links psproc_fake (PSD_NO_NATIVE_BACKEND). Nothing here renices or signals
 * anything; the script backend is installed with no respond command.
 *
 * Returns 0 if every check passes, 1 otherwise.
 */

#include "proc_backend.h"
#include "proc_script.h"
#include "psproc_options.h"

#include <stdio.h>
#include <stdlib.h>

static int checks_run;
static int checks_failed;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks_run++;                                                        \
        if (!(cond)) {                                                       \
            checks_failed++;                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

static void reset_defaults(void)
{
    psproc_kill_policy = PSD_KILL_RENICE;
    psproc_monster_cap = 6;
    psproc_classify_by = PSD_CLASSIFY_MEM;
    psproc_all_users   = 0;
    psproc_show_labels = 1;
    psproc_show_pid    = 0;
}

/* Before any reset: the initializers themselves. Runs first. */
static void test_initial_cap(void)
{
    CHECK(psproc_monster_cap == 6);
    psproc_options_sanitize();
    CHECK(psproc_monster_cap == 6);        /* in range: left alone */
    psproc_monster_cap = PSD_MONSTER_CAP_MIN;
    psproc_options_sanitize();
    CHECK(psproc_monster_cap == PSD_MONSTER_CAP_MIN);
    psproc_monster_cap = PSD_MONSTER_CAP_MAX;
    psproc_options_sanitize();
    CHECK(psproc_monster_cap == PSD_MONSTER_CAP_MAX);
    psproc_monster_cap = PSD_MONSTER_CAP_MIN - 1;
    psproc_options_sanitize();
    CHECK(psproc_monster_cap == PSD_MONSTER_CAP_MIN);
    psproc_monster_cap = PSD_MONSTER_CAP_MAX + 1;
    psproc_options_sanitize();
    CHECK(psproc_monster_cap == PSD_MONSTER_CAP_MAX);
}

static void test_defaults_are_safe(void)
{
    char *argv[] = { "psquake", NULL };

    reset_defaults();
    psproc_options_parse_args(1, argv, "psquake");
    CHECK(psproc_should_renice());
    CHECK(!psproc_should_kill());          /* kill is opt-in */
    CHECK(!psproc_all_users_enabled());
}

static void test_prefixed_flags(void)
{
    char *live[]  = { "psquake", "-psquake-live", "-psquake-allusers",
                      "-psquake-cpu", "-psquake-showpid", "-psquake-nolabels",
                      NULL };
    char *other[] = { "psquake", "-psdoom-live", "-psquake-liv",
                      "psquake-live", "-psquake-live-x", NULL };

    reset_defaults();
    psproc_options_parse_args(5, other, "psquake");   /* none match exactly */
    CHECK(psproc_kill_policy == PSD_KILL_RENICE);

    psproc_options_parse_args(6, live, "psquake");
    CHECK(psproc_should_kill());
    CHECK(psproc_all_users_enabled());
    CHECK(psproc_classify_by == PSD_CLASSIFY_CPU);
    CHECK(psproc_pid_enabled());
    CHECK(!psproc_labels_enabled());

    reset_defaults();
    psproc_options_parse_args(6, live, NULL);         /* no prefix: ignored */
    CHECK(!psproc_should_kill());
}

static void test_live_wins_over_safe(void)
{
    char *both[] = { "psquake", "-psquake-live", "-psquake-safe", NULL };
    char *sim[]  = { "psquake", "-psquake-simulate", NULL };

    reset_defaults();
    psproc_options_parse_args(3, both, "psquake");    /* as in psDoom */
    CHECK(psproc_kill_policy == PSD_KILL_LIVE);

    reset_defaults();
    psproc_options_parse_args(2, sim, "psquake");
    CHECK(!psproc_should_renice());
    CHECK(!psproc_should_kill());
}

static void test_sanitize(void)
{
    reset_defaults();
    psproc_kill_policy = 7;                /* corrupt config value */
    psproc_monster_cap = 1000;
    psproc_options_sanitize();
    CHECK(psproc_kill_policy == PSD_KILL_SIMULATE);
    CHECK(psproc_monster_cap == PSD_MONSTER_CAP_MAX);
    CHECK(!psproc_should_renice() && !psproc_should_kill());

    psproc_kill_policy = -1;
    psproc_monster_cap = 0;
    psproc_classify_by = 9;
    psproc_label_range = -3;
    psproc_options_sanitize();
    CHECK(psproc_kill_policy == PSD_KILL_SIMULATE);
    CHECK(psproc_monster_cap == PSD_MONSTER_CAP_MIN);
    CHECK(psproc_classify_by == PSD_CLASSIFY_MEM);
    CHECK(psproc_label_range == PSD_LABELS_NORMAL);
}

static void test_script_env_prefix(void)
{
    const proc_backend_t *before;

    proc_backend_set(NULL);
    before = proc_backend();

    unsetenv("PSQUAKE_POLL_CMD");
    setenv("PSDOOM_POLL_CMD", "true", 1);  /* psDoom's name: not read */
    CHECK(proc_script_install_from_env("PSQUAKE") == 0);
    CHECK(proc_backend() == before);
    CHECK(proc_script_install_from_env(NULL) == 0);
    CHECK(proc_script_install_from_env("") == 0);

    setenv("PSQUAKE_POLL_CMD", "true", 1);
    unsetenv("PSQUAKE_RESPOND_CMD");
    CHECK(proc_script_install_from_env("PSQUAKE") == 1);
    CHECK(proc_backend() != before);

    proc_backend_set(NULL);
    unsetenv("PSQUAKE_POLL_CMD");
    unsetenv("PSDOOM_POLL_CMD");
}

int main(void)
{
    test_initial_cap();
    test_defaults_are_safe();
    test_prefixed_flags();
    test_live_wins_over_safe();
    test_sanitize();
    test_script_env_prefix();

    printf("%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
