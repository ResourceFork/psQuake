/*
 * psproc -- user-tunable process options (implementation). See psproc_options.h.
 */

#include "psproc_options.h"

#include <stdio.h>    /* snprintf */
#include <string.h>   /* strcmp   */

/* ------------------------------------------------------------------ state */

int psproc_kill_policy = PSD_KILL_RENICE;   /* SAFE default: renice, never kill
                                             * (opt into real kills via a menu
                                             * or the -<prefix>-live flag)     */
int psproc_monster_cap = 10;                /* live-monster cap                */
int psproc_classify_by = PSD_CLASSIFY_MEM;  /* default: classify by memory     */
int psproc_all_users   = 0;                 /* default: our own processes     */
int psproc_show_labels = 1;                 /* default: labels on             */
int psproc_show_pid    = 0;                 /* default: name only (less clutter)*/
int psproc_label_range = PSD_LABELS_NORMAL;

/* ------------------------------------------------------------------ startup */

/* 1 if argv[1..argc) holds exactly "-<prefix>-<name>". */
static int psproc_parm_exists(int argc, char **argv, const char *prefix,
                              const char *name)
{
    char flag[96];
    int  i;

    if (argv == NULL || prefix == NULL)
    {
        return 0;
    }
    if (snprintf(flag, sizeof(flag), "-%s-%s", prefix, name)
        >= (int) sizeof(flag))
    {
        return 0;
    }
    for (i = 1; i < argc; i++)
    {
        if (argv[i] != NULL && strcmp(argv[i], flag) == 0)
        {
            return 1;
        }
    }
    return 0;
}

void psproc_options_sanitize(void)
{
    if (psproc_kill_policy < 0 || psproc_kill_policy >= PSD_KILL_NUM)
    {
        psproc_kill_policy = PSD_KILL_RENICE;    /* unknown value: safe default */
    }
    if (psproc_monster_cap < PSD_MONSTER_CAP_MIN)
    {
        psproc_monster_cap = PSD_MONSTER_CAP_MIN;
    }
    if (psproc_monster_cap > PSD_MONSTER_CAP_MAX)
    {
        psproc_monster_cap = PSD_MONSTER_CAP_MAX;
    }
}

void psproc_options_parse_args(int argc, char **argv, const char *prefix)
{
    if (psproc_parm_exists(argc, argv, prefix, "safe"))
    {
        psproc_kill_policy = PSD_KILL_RENICE;    /* wound only, never kill     */
    }
    if (psproc_parm_exists(argc, argv, prefix, "simulate"))
    {
        psproc_kill_policy = PSD_KILL_SIMULATE;  /* no real effect at all      */
    }
    if (psproc_parm_exists(argc, argv, prefix, "live"))
    {
        psproc_kill_policy = PSD_KILL_LIVE;      /* opt in: really kill        */
    }
    if (psproc_parm_exists(argc, argv, prefix, "allusers"))
    {
        psproc_all_users = 1;
    }
    if (psproc_parm_exists(argc, argv, prefix, "nolabels"))
    {
        psproc_show_labels = 0;
    }
    if (psproc_parm_exists(argc, argv, prefix, "showpid"))
    {
        psproc_show_pid = 1;
    }
    if (psproc_parm_exists(argc, argv, prefix, "cpu"))
    {
        psproc_classify_by = PSD_CLASSIFY_CPU;
    }

    psproc_options_sanitize();
}

/* ------------------------------------------------------------------ queries */

int psproc_should_kill(void)
{
    return psproc_kill_policy == PSD_KILL_LIVE;
}

int psproc_should_renice(void)
{
    return psproc_kill_policy == PSD_KILL_LIVE
        || psproc_kill_policy == PSD_KILL_RENICE;
}

int psproc_labels_enabled(void)
{
    return psproc_show_labels != 0;
}

int psproc_pid_enabled(void)
{
    return psproc_show_pid != 0;
}

int psproc_all_users_enabled(void)
{
    return psproc_all_users != 0;
}
