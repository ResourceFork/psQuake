/*
 * psQuake -- process-monster policy (engine-free). See psq_policy.h.
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#include "psq_policy.h"

#include "proc_select.h"
#include "psproc_options.h"

#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ ladder */

/*
 * Toughness ladder, weakest first. Memory bounds are bytes, CPU bounds are
 * percent of one core; a process takes the first class whose bound it is
 * under. The shareware-era base ladder (dog, army, knight, ogre, demon1,
 * shambler at 16/64/192/512/1536 MB and 5/25/60/120/250 %) is split by the
 * three registered classes; a class whose model the data lacks gives its
 * range to the next lighter class the data has, so without them the ladder
 * is exactly the base one.
 */
typedef struct
{
    const char        *classname;
    const char        *model;
    unsigned long long mem_max;
    unsigned long long cpu_max;
} psq_rung_t;

#define PSQ_MB(n) ((unsigned long long) (n) << 20)

static const psq_rung_t psq_ladder[] =
{
    { "monster_dog",         "progs/dog.mdl",      PSQ_MB(16),   5 },
    { "monster_army",        "progs/soldier.mdl",  PSQ_MB(40),   15 },
    { "monster_enforcer",    "progs/enforcer.mdl", PSQ_MB(64),   25 },
    { "monster_knight",      "progs/knight.mdl",   PSQ_MB(192),  60 },
    { "monster_ogre",        "progs/ogre.mdl",     PSQ_MB(384),  90 },
    { "monster_hell_knight", "progs/hknight.mdl",  PSQ_MB(512),  120 },
    { "monster_demon1",      "progs/demon.mdl",    PSQ_MB(1024), 180 },
    { "monster_shalrath",    "progs/shalrath.mdl", PSQ_MB(1536), 250 },
    { "monster_shambler",    "progs/shambler.mdl", ~0ULL,        ~0ULL },
};
#define PSQ_LADDER_N ((int) (sizeof(psq_ladder) / sizeof(psq_ladder[0])))

static const psq_rung_t psq_swarm_rung =
    { "monster_wizard", "progs/wizard.mdl", 0, 0 };

/* ------------------------------------------------------------------- state */

typedef struct
{
    int                pid;
    unsigned long long start;
    char               name[PSQ_NAME_LEN];
} psq_ident_t;

/* What the policy remembers about one pool process across polls. */
typedef struct
{
    psd_proc_t  proc;        /* identity and metrics from the latest poll   */
    const char *iname;       /* interned, sanitized name                    */
    int         committed;   /* ladder index QuakeC should show             */
    int         pending;     /* ladder index accumulating votes             */
    int         streak;      /* consecutive polls voting for `pending`      */
    int         children;    /* child count at the last poll; -1 = unseen   */
    int         cooldown_until; /* poll number before which it cannot swarm */
    int         owed;        /* swarm members owed this poll                */
    int         gone;        /* a checked action found it gone or reused    */
} psq_track_t;

static int          psq_gate_open;
static int          psq_level_active;
static int          psq_avail[PSQ_LADDER_N];
static int          psq_swarm_avail;

static psq_track_t  psq_pool[PSQ_POOL_MAX];
static int          psq_pool_n;
static psq_track_t  psq_next[PSQ_POOL_MAX];   /* scratch while rebuilding  */
/* Collected with room to spare: killed processes are dropped after it. */
static psd_proc_t   psq_collected[PSQ_POOL_MAX * 2];

static psq_ident_t  psq_killed[PSQ_KILLED_MAX];
static int          psq_killed_n;

/* Interned names: the raw identity it was made for, and the sanitized copy
 * QuakeC is given. */
typedef struct
{
    psq_ident_t ident;
    char        shown[PSQ_NAME_LEN];
} psq_name_t;

static psq_name_t   psq_names[PSQ_NAMES_MAX];
static int          psq_names_n;
static const char   psq_name_overflow[] = "process";

static int          psq_polled;        /* any refresh yet on this level     */
static double       psq_last_poll;
static int          psq_poll_count;
static int          psq_swarm_live;

/* ---------------------------------------------------------------- helpers */

int psq_gate_allows(const psq_gate_facts *f)
{
    return f != NULL && f->server_active && f->maxclients == 1
        && !f->deathmatch && !f->coop && !f->dedicated;
}

int psq_option_change_allowed(int untrusted, int old_policy, int new_policy,
                              int old_all_users, int new_all_users)
{
    return !untrusted
        || (new_policy <= old_policy && new_all_users <= old_all_users);
}

void psq_policy_set_gate(int open)
{
    psq_gate_open = open != 0;
}

static int psq_same_ident(const psq_ident_t *k, const psd_proc_t *p)
{
    /* psproc's rule: with no start token (a script entity) the id alone is
     * the identity, so a relabelled entity is still the one killed. */
    return psd_proc_same(p, k->pid, k->start, k->name);
}

static void psq_ident_from(psq_ident_t *k, const psd_proc_t *p)
{
    k->pid = p->pid;
    k->start = p->start_token;
    memcpy(k->name, p->name, sizeof(k->name) - 1);
    k->name[sizeof(k->name) - 1] = '\0';
}

static int psq_is_killed(const psd_proc_t *p)
{
    int i;

    for (i = 0; i < psq_killed_n; i++)
    {
        if (psq_same_ident(&psq_killed[i], p))
        {
            return 1;
        }
    }
    return 0;
}

/* The interned, sanitized copy of p's name for this level. Process names
 * and script labels are untrusted: only printable ASCII survives. */
static const char *psq_intern(const psd_proc_t *p)
{
    psq_name_t *k;
    int         i;

    for (i = 0; i < psq_names_n; i++)
    {
        if (psq_same_ident(&psq_names[i].ident, p))
        {
            return psq_names[i].shown;
        }
    }
    if (psq_names_n >= PSQ_NAMES_MAX)
    {
        return psq_name_overflow;
    }
    k = &psq_names[psq_names_n++];
    psq_ident_from(&k->ident, p);
    for (i = 0; k->ident.name[i] != '\0'; i++)
    {
        unsigned char c = (unsigned char) k->ident.name[i];
        k->shown[i] = (c < 0x20 || c >= 0x7f) ? '?' : (char) c;
    }
    k->shown[i] = '\0';
    return k->shown;
}

/* Ladder index for a metric: the first rung it is under, then down to the
 * nearest rung the data has (up if none lighter has one). -1 if none. */
static int psq_classify(const psd_proc_t *p)
{
    unsigned long long metric;
    int ideal = PSQ_LADDER_N - 1;
    int i;

    metric = (psproc_classify_by == PSD_CLASSIFY_CPU)
        ? (unsigned long long) (p->cpu_percent < 0 ? 0 : p->cpu_percent)
        : p->footprint;
    for (i = 0; i < PSQ_LADDER_N; i++)
    {
        unsigned long long max = (psproc_classify_by == PSD_CLASSIFY_CPU)
            ? psq_ladder[i].cpu_max : psq_ladder[i].mem_max;
        if (metric < max)
        {
            ideal = i;
            break;
        }
    }
    for (i = ideal; i >= 0; i--)
    {
        if (psq_avail[i])
        {
            return i;
        }
    }
    for (i = ideal + 1; i < PSQ_LADDER_N; i++)
    {
        if (psq_avail[i])
        {
            return i;
        }
    }
    return -1;
}

static psq_track_t *psq_find(int pid)
{
    int i;

    if (!psq_gate_open || pid <= 1)
    {
        return NULL;
    }
    for (i = 0; i < psq_pool_n; i++)
    {
        if (psq_pool[i].proc.pid == pid)
        {
            return &psq_pool[i];
        }
    }
    return NULL;
}

static void psq_clear_pool(void)
{
    psq_pool_n = 0;
}

/* ------------------------------------------------------------- lifecycle */

void psq_policy_init(void)
{
    psd_select_init();
    psq_policy_level_reset(NULL);
}

/* Forget everything recorded about the level's processes (not the gate,
 * not which classes the data has). */
static void psq_reset_state(void)
{
    psq_clear_pool();
    psq_killed_n = 0;
    psq_polled = 0;
    psq_last_poll = 0.0;
    psq_poll_count = 0;
    psq_swarm_live = 0;
}

void psq_policy_level_reset(int (*model_exists)(const char *model))
{
    int i;

    psq_gate_open = 0;
    psq_level_active = 0;
    psq_reset_state();
    psq_names_n = 0;   /* QuakeC strings point here: only between levels */
    for (i = 0; i < PSQ_LADDER_N; i++)
    {
        psq_avail[i] = model_exists == NULL || model_exists(psq_ladder[i].model);
    }
    psq_swarm_avail = model_exists == NULL || model_exists(psq_swarm_rung.model);
}

int psq_policy_poll_count(void)
{
    return psq_poll_count;
}

int psq_policy_level_active(void)
{
    return psq_level_active;
}

/* --------------------------------------------------------------- polling */

/* Carry `t` (an existing track for this identity, or NULL) into slot `n`
 * for process `p`, applying hysteresis and fork detection. */
static void psq_track_update(psq_track_t *out, const psq_track_t *t,
                             const psd_proc_t *p, int *owed_total)
{
    int desired = psq_classify(p);
    int children = psd_select_child_count(p->pid);

    if (t == NULL)
    {
        memset(out, 0, sizeof(*out));
        out->committed = desired;
        out->pending = desired;
        out->children = children;   /* baseline: first sight never swarms */
        out->cooldown_until = 0;
    }
    else
    {
        int burst;

        *out = *t;
        out->owed = 0;

        /* Hysteresis: promote after UP_HOLD polls, demote after DOWN_HOLD. */
        if (desired == out->committed)
        {
            out->streak = 0;
            out->pending = desired;
        }
        else
        {
            int hold;

            if (out->streak == 0 || out->pending != desired)
            {
                out->pending = desired;
                out->streak = 1;
            }
            else
            {
                out->streak++;
            }
            hold = (desired > out->committed) ? PSQ_REGRADE_UP_HOLD
                                              : PSQ_REGRADE_DOWN_HOLD;
            if (out->committed < 0 || out->streak >= hold)
            {
                out->committed = desired;
                out->streak = 0;
            }
        }

        /* Fork burst: the child count jumped since the last poll. */
        burst = children - out->children;
        out->children = children;
        if (psq_swarm_avail && burst >= PSQ_FORK_BURST
            && psq_poll_count >= out->cooldown_until)
        {
            int room = PSQ_SWARM_MAX_LIVE - psq_swarm_live - *owed_total;
            int want = burst < PSQ_SWARM_PER_BURST ? burst : PSQ_SWARM_PER_BURST;

            if (want > room)
            {
                want = room;
            }
            if (want > 0)
            {
                out->owed = want;
                *owed_total += want;
            }
        }
    }
    out->proc = *p;
    out->iname = psq_intern(p);
}

static const psq_track_t *psq_old_track(int pid)
{
    int i;

    for (i = 0; i < psq_pool_n; i++)
    {
        if (psq_pool[i].proc.pid == pid)
        {
            return &psq_pool[i];
        }
    }
    return NULL;
}

int psq_policy_poll(double now)
{
    int n;
    int i;
    int w = 0;
    int owed_total = 0;

    if (!psq_gate_open)
    {
        psq_clear_pool();
        return 0;
    }
    if (psq_polled && now < psq_last_poll)
    {
        /* The clock went backwards: a new server without a level reset.
         * Start over rather than trust anything recorded. */
        psq_reset_state();
    }
    /* A little slack: QuakeC time is a float, so a think exactly one
     * interval later can read as a hair under it. */
    if (psq_polled && now - psq_last_poll < PSQ_POLL_INTERVAL - 0.05)
    {
        return psq_pool_n;
    }
    psq_polled = 1;
    psq_last_poll = now;
    psq_poll_count++;
    psq_level_active = 1;

    n = psd_select_collect(psq_collected, PSQ_POOL_MAX * 2);
    for (i = 0; i < n && w < PSQ_POOL_MAX; i++)
    {
        const psd_proc_t  *p = &psq_collected[i];
        const psq_track_t *t;
        int                j;
        int                dup = 0;

        /* QuakeC holds a pid as a float, exact only up to 2^24: a larger id
         * (a script entity) could round onto another one, so it is never
         * handed out. */
        if (p->pid <= 1 || p->pid > PSQ_PID_MAX || psq_is_killed(p))
        {
            continue;
        }
        for (j = 0; j < w; j++)
        {
            if (psq_next[j].proc.pid == p->pid)
            {
                dup = 1;   /* a backend listed one pid twice: keep the first */
                break;
            }
        }
        if (dup)
        {
            continue;
        }
        t = psq_old_track(p->pid);
        if (t != NULL && !psd_proc_same(p, t->proc.pid, t->proc.start_token,
                                        t->proc.name))
        {
            /* The pid now belongs to another process. Hold the newcomer out
             * for one poll so the old monster retires before any monster
             * can stand for the new process under the same pid. */
            continue;
        }
        if (t != NULL && t->gone)
        {
            /* A checked action found this identity gone or reused: hold it
             * out for one poll so its monster retires (present is 0). */
            continue;
        }
        psq_track_update(&psq_next[w], t, p, &owed_total);
        w++;
    }
    memcpy(psq_pool, psq_next, sizeof(psq_pool[0]) * (size_t) w);
    psq_pool_n = w;
    return psq_pool_n;
}

const psd_proc_t *psq_policy_entry(int i)
{
    if (!psq_gate_open || i < 0 || i >= psq_pool_n)
    {
        return NULL;
    }
    return &psq_pool[i].proc;
}

const char *psq_policy_name(int i)
{
    if (!psq_gate_open || i < 0 || i >= psq_pool_n)
    {
        return "";
    }
    return psq_pool[i].iname;
}

const char *psq_policy_class(int i)
{
    if (!psq_gate_open || i < 0 || i >= psq_pool_n || psq_pool[i].committed < 0
        || psq_pool[i].gone || psq_is_killed(&psq_pool[i].proc))
    {
        return "";   /* nothing to spawn for it */
    }
    return psq_ladder[psq_pool[i].committed].classname;
}

int psq_policy_burst(int pid)
{
    psq_track_t *t = psq_find(pid);
    int          owed;

    if (t == NULL || t->gone)
    {
        return 0;
    }
    owed = t->owed;
    t->owed = 0;
    if (owed > 0)
    {
        if (owed > PSQ_SWARM_MAX_LIVE - psq_swarm_live)
        {
            owed = PSQ_SWARM_MAX_LIVE - psq_swarm_live;
        }
        if (owed < 0)
        {
            owed = 0;
        }
        psq_swarm_live += owed;
        t->cooldown_until = psq_poll_count + PSQ_SWARM_COOLDOWN;
    }
    return owed;
}

int psq_policy_present(int pid)
{
    psq_track_t *t = psq_find(pid);

    return t != NULL && !t->gone && !psq_is_killed(&t->proc);
}

const char *psq_policy_tier_class(int t)
{
    int i;
    int k = 0;

    if (t == -1)
    {
        return psq_swarm_avail ? psq_swarm_rung.classname : "";
    }
    for (i = 0; i < PSQ_LADDER_N; i++)
    {
        if (psq_avail[i])
        {
            if (k == t)
            {
                return psq_ladder[i].classname;
            }
            k++;
        }
    }
    return "";
}

int psq_policy_swarm_live(void)
{
    return psq_swarm_live;
}

/* ---------------------------------------------------------------- actions */

static psq_result_t psq_result(psq_outcome_t o, psd_act_result_t a)
{
    psq_result_t r;

    r.outcome = o;
    r.act = a;
    return r;
}

/* Common checks for an action on `pid`; NULL with *why set if refused. */
static psq_track_t *psq_target(int pid, psq_outcome_t *why)
{
    psq_track_t *t;

    if (!psq_gate_open)
    {
        *why = PSQ_OUT_GATED;
        return NULL;
    }
    if (pid <= 1)
    {
        *why = PSQ_OUT_NOT_PROCESS;
        return NULL;
    }
    t = psq_find(pid);
    if (t == NULL)
    {
        *why = PSQ_OUT_UNKNOWN;
        return NULL;
    }
    if (psq_is_killed(&t->proc))
    {
        *why = PSQ_OUT_KILLED;
        return NULL;
    }
    if (t->gone)
    {
        *why = PSQ_OUT_GONE;
        return NULL;
    }
    return t;
}

psq_result_t psq_policy_wound(int pid, double take)
{
    psq_outcome_t    why;
    psq_track_t     *t = psq_target(pid, &why);
    psd_act_result_t a;

    if (t == NULL)
    {
        return psq_result(why, PSD_ACT_REFUSED);
    }
    if (!(take > 0.0))
    {
        return psq_result(PSQ_OUT_NO_DAMAGE, PSD_ACT_REFUSED);
    }
    if (!psproc_should_renice())
    {
        return psq_result(PSQ_OUT_POLICY, PSD_ACT_REFUSED);
    }
    a = psd_proc_renice_checked(t->proc.pid, t->proc.start_token,
                                t->proc.name, PSQ_WOUND_NICE);
    if (a == PSD_ACT_GONE || a == PSD_ACT_REUSED)
    {
        t->gone = 1;
    }
    return psq_result(PSQ_OUT_ACTED, a);
}

static void psq_mark_killed(const psd_proc_t *p)
{
    if (psq_killed_n < PSQ_KILLED_MAX && !psq_is_killed(p))
    {
        psq_ident_from(&psq_killed[psq_killed_n++], p);
    }
}

psq_result_t psq_policy_kill(int pid)
{
    psq_outcome_t    why;
    psq_track_t     *t;
    psd_act_result_t a;

    if (psq_gate_open && pid == PSQ_SWARM_PID)
    {
        if (psq_swarm_live > 0)
        {
            psq_swarm_live--;
        }
        return psq_result(PSQ_OUT_SWARM, PSD_ACT_REFUSED);
    }
    t = psq_target(pid, &why);
    if (t == NULL)
    {
        return psq_result(why, PSD_ACT_REFUSED);
    }
    if (psq_killed_n >= PSQ_KILLED_MAX)
    {
        /* The kill could not be remembered, so the process could come back
         * and be signalled again on every kill: signal nothing more. */
        t->gone = 1;
        return psq_result(PSQ_OUT_FULL, PSD_ACT_REFUSED);
    }
    if (!psproc_should_kill())
    {
        psq_mark_killed(&t->proc);
        return psq_result(PSQ_OUT_POLICY, PSD_ACT_REFUSED);
    }
    a = psd_proc_kill_checked(t->proc.pid, t->proc.start_token, t->proc.name);
    if (a == PSD_ACT_GONE || a == PSD_ACT_REUSED)
    {
        t->gone = 1;   /* nothing was signalled; not remembered as killed */
    }
    else
    {
        psq_mark_killed(&t->proc);
    }
    return psq_result(PSQ_OUT_ACTED, a);
}
