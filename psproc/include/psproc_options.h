/*
 * psproc -- user-tunable process options (engine-free).
 *
 * Imported from psDoom's psdoom_options.h (436ba83), cut down to the parts
 * with no engine dependency: the settings, the policy queries the game logic
 * asks of them, and command-line parsing over a plain argc/argv. Doom's
 * config-file binding (M_BindIntVariable) and the psDoom menu mutators are
 * not carried; each game binds these ints to its own config and menu.
 */

#ifndef PSPROC_OPTIONS_H
#define PSPROC_OPTIONS_H

#ifdef __cplusplus
extern "C" {
#endif

/* What happens to the real process behind a monster. Ordered by increasing
 * real-world impact. The default is RENICE: safe (no process is ever killed)
 * but still functional. Real killing is opt-in (-<prefix>-live, or a menu). */
enum
{
    PSD_KILL_SIMULATE = 0, /* no real process effect at all (pure game)       */
    PSD_KILL_RENICE,       /* wound -> renice; monster death -> nothing (safe)*/
    PSD_KILL_LIVE,         /* wound -> renice; monster death -> SIGTERM        */
    PSD_KILL_NUM
};

/* Label draw distance. The mapping to a renderer threshold is game-specific
 * (psDoom's 320-space vissprite scale is not carried). */
enum
{
    PSD_LABELS_NEAR = 0,  /* only very close monsters                         */
    PSD_LABELS_NORMAL,
    PSD_LABELS_FAR,
    PSD_LABELS_ALL,       /* label everything, however distant                */
    PSD_LABELRANGE_NUM
};

/* Live-monster cap range. */
#define PSD_MONSTER_CAP_MIN 5
#define PSD_MONSTER_CAP_MAX 35

/* Which process metric drives monster toughness. */
enum
{
    PSD_CLASSIFY_MEM = 0,  /* memory footprint (default)                      */
    PSD_CLASSIFY_CPU,      /* recent CPU load                                 */
    PSD_CLASSIFY_NUM
};

/* Settings. Plain ints so a game can bind them to its own config storage. */
extern int psproc_kill_policy;    /* PSD_KILL_*               */
extern int psproc_monster_cap;    /* live-monster cap (5..35) */
extern int psproc_classify_by;    /* PSD_CLASSIFY_*           */
extern int psproc_all_users;      /* 0 = our uid only         */
extern int psproc_show_labels;    /* 0 / 1                    */
extern int psproc_show_pid;       /* 0 / 1 (off: name only)   */
extern int psproc_label_range;    /* PSD_LABELS_*             */

/*
 * Apply command-line overrides. `prefix` names the flag family without the
 * leading dash ("psquake" accepts -psquake-live, -psquake-safe, ...). Call
 * once at startup, after the game's config has been loaded (so flags win).
 * Unknown arguments are ignored. Flags, in increasing order of precedence for
 * the kill policy: -<prefix>-safe, -<prefix>-simulate, -<prefix>-live.
 * Others: -<prefix>-allusers, -<prefix>-nolabels, -<prefix>-showpid,
 * -<prefix>-cpu.
 */
void psproc_options_parse_args(int argc, char **argv, const char *prefix);

/* Clamp config-loaded values into their valid ranges; an unknown kill policy
 * becomes PSD_KILL_SIMULATE (act on nothing). parse_args calls it. */
void psproc_options_sanitize(void);

/* Policy queries used by the game logic (1 = yes). */
int psproc_should_kill(void);       /* send SIGTERM when a monster dies?      */
int psproc_should_renice(void);     /* renice on a non-fatal hit?             */
int psproc_labels_enabled(void);
int psproc_pid_enabled(void);       /* show the PID line in a label?          */
int psproc_all_users_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* PSPROC_OPTIONS_H */
