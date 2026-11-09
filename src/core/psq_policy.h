/*
 * psQuake -- process-monster policy (engine-free).
 *
 * Everything behind the QuakeC builtins #1200-#1208 that decides or touches
 * a real process lives here, so it is host-testable against psproc's fake
 * backend:
 *
 *   - the local single-player gate: every process query and action is a
 *     no-op unless it is open;
 *   - the curated pool: the latest psd_select_collect() snapshot (triage,
 *     protected list, self and launcher chain already excluded), minus the
 *     processes killed this level;
 *   - the toughness ladder and its availability (only classes whose model
 *     the loaded data has), with promote-fast / demote-slow hysteresis;
 *   - fork-burst detection and the swarm budget;
 *   - the killed set;
 *   - wound and kill, gated by psproc's kill policy and made only through
 *     psd_proc_renice_checked / psd_proc_kill_checked.
 *
 * QuakeC names processes only by pid. A pid is acted on only while it is in
 * the latest pool, and only as the identity (pid, start token, name) the pool
 * recorded for it, so QuakeC can never reach a process this policy did not
 * hand out. The engine glue is src/engine/psq_builtins.c.
 *
 * MIT licensed, like the rest of psQuake's own files.
 */

#ifndef PSQ_POLICY_H
#define PSQ_POLICY_H

#include "proc_types.h"
#include "proc_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Size of the curated pool: the most relevant processes kept each poll. The
 * live-monster cap (psproc_monster_cap, at most 35) picks from these. */
#define PSQ_POOL_MAX       64
#define PSQ_KILLED_MAX     1024
#define PSQ_NAMES_MAX      2048  /* interned names per level              */
#define PSQ_NAME_LEN       32    /* bytes, including the terminator       */

#define PSQ_POLL_INTERVAL  1.0   /* seconds of game time between refreshes */
#define PSQ_WOUND_NICE     4     /* priority drop per non-fatal hit        */
#define PSQ_REGRADE_UP_HOLD   2  /* polls a tougher class must persist     */
#define PSQ_REGRADE_DOWN_HOLD 5  /* polls a lighter class must persist     */
#define PSQ_FORK_BURST     5     /* child-count jump per poll that is a burst */
#define PSQ_SWARM_PER_BURST 6    /* most swarm members owed for one burst  */
#define PSQ_SWARM_MAX_LIVE 24    /* most swarm members alive at once       */
#define PSQ_SWARM_COOLDOWN 4     /* polls before a parent may swarm again  */

/* Largest pid handed to QuakeC: a QuakeC float holds integers exactly only
 * up to 2^24. */
#define PSQ_PID_MAX     16777216

/* psq_pid tags QuakeC uses. > 1 is a process monster. */
#define PSQ_SWARM_PID   (-1)     /* a fork-swarm member: acts on nothing   */

/* What the engine knows about the session, for the gate. */
typedef struct
{
    int server_active;   /* sv.active                                   */
    int maxclients;      /* svs.maxclients                              */
    int deathmatch;      /* deathmatch.value != 0                       */
    int coop;            /* coop.value != 0                             */
    int dedicated;       /* a dedicated server (no local player)        */
} psq_gate_facts;

/* 1 only for a local single-player game: an active server, one client, not
 * deathmatch or coop, not dedicated. Anything else could let a remote
 * client make this machine act on its own processes. */
int psq_gate_allows(const psq_gate_facts *facts);

/* 1 if an options change from (old_policy, old_all_users) to (new_policy,
 * new_all_users) may stand. After a remote server or demo has stuffed
 * commands (`untrusted`), the kill policy may only go down (PSD_KILL_* is
 * ordered by real-world impact) and all-users only off. */
int psq_option_change_allowed(int untrusted, int old_policy, int new_policy,
                              int old_all_users, int new_all_users);

/* Set the gate for the calls that follow (the engine glue calls this before
 * each builtin). Closed by default and after psq_policy_level_reset(). */
void psq_policy_set_gate(int open);

/* Once at startup: psd_select_init() (uid and the game's own pid). */
void psq_policy_init(void);

/* Each level load: forget the pool, killed set, hysteresis, fork baselines,
 * swarm count, interned names and the poll clock, close the gate, and
 * decide which ladder classes the data has. `model_exists` answers for a
 * model path such as "progs/ogre.mdl"; NULL means every model exists. */
void psq_policy_level_reset(int (*model_exists)(const char *model));

/* Refresh the pool if `now` (game seconds) is at least PSQ_POLL_INTERVAL
 * past the last refresh, then return its size. Gate closed: empties the
 * pool and returns 0. */
int psq_policy_poll(double now);

/* 1 once a poll has run with the gate open on this level. */
int psq_policy_level_active(void);

/* Refreshes run on this level (0 before the first). */
int psq_policy_poll_count(void);

/* Pool entry i (0 <= i < pool size), or NULL. */
const psd_proc_t *psq_policy_entry(int i);

/* Pool entry i's name: printable ASCII, stable in memory for the level (the
 * same pointer each time for the same process), "" if i is out of range. */
const char *psq_policy_name(int i);

/* Pool entry i's committed monster classname (hysteresis applied), "" if i
 * is out of range or the data has no ladder class at all. */
const char *psq_policy_class(int i);

/* Swarm members owed for pool process `pid` this poll; consumes them. The
 * caller reports each member that ends (dies, or could not be placed) with
 * psq_policy_kill(PSQ_SWARM_PID). */
int psq_policy_burst(int pid);

/* 1 if `pid` is in the pool, not killed this level, and no checked action
 * found it gone or reused. */
int psq_policy_present(int pid);

/* Ladder class t (0 = weakest) among the classes the data has; t == -1 is
 * the swarm class. "" past the end or if unavailable. Not gated: it only
 * names classes (the arena precaches them while the level loads). */
const char *psq_policy_tier_class(int t);

/* Outcome of psq_policy_wound / psq_policy_kill. */
typedef enum
{
    PSQ_OUT_GATED = 0,    /* gate closed: nothing considered               */
    PSQ_OUT_NOT_PROCESS,  /* pid <= 1 (an ordinary monster)                */
    PSQ_OUT_UNKNOWN,      /* pid not in the pool: refused                  */
    PSQ_OUT_KILLED,       /* already killed this level: nothing more       */
    PSQ_OUT_GONE,         /* a checked action already found it gone/reused */
    PSQ_OUT_NO_DAMAGE,    /* wound with take <= 0                          */
    PSQ_OUT_POLICY,       /* the kill policy forbids the action; for a
                           * kill, the process is still marked killed     */
    PSQ_OUT_SWARM,        /* a swarm member ended: budget returned         */
    PSQ_OUT_FULL,         /* the level's killed set is full: no action     */
    PSQ_OUT_ACTED,        /* a checked backend action ran; see `act`       */
} psq_outcome_t;

typedef struct
{
    psq_outcome_t    outcome;
    psd_act_result_t act;   /* valid for PSQ_OUT_ACTED */
} psq_result_t;

/* A process monster survived a hit of `take`: renice it by PSQ_WOUND_NICE
 * if the kill policy allows (renice or live). */
psq_result_t psq_policy_wound(int pid, double take);

/* A process monster died: remember it as killed for the level (the pool
 * drops it), and send SIGTERM if the kill policy is live. PSQ_SWARM_PID
 * returns one member to the swarm budget. If the checked kill finds the pid
 * gone or reused, nothing was signalled and it is not remembered as killed. */
psq_result_t psq_policy_kill(int pid);

/* Live swarm members as the policy counts them (tests and the debug log). */
int psq_policy_swarm_live(void);

#ifdef __cplusplus
}
#endif

#endif /* PSQ_POLICY_H */
