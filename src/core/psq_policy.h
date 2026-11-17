/*
 * psQuake -- process-monster policy (engine-free).
 *
 * Everything behind the QuakeC builtins #1200-#1208 that decides or touches
 * a real process lives here, so it is host-testable against psproc's fake
 * backend:
 *
 *   - the local single-player gate: every process query and action is a
 *     no-op unless it is open;
 *   - the curated pool: the latest psd_select_collect() snapshot, in the
 *     order Summoner's trusted plugin gave it (it sorts, trims and
 *     excludes; psQuake re-ranks and filters nothing), minus the
 *     processes killed this level;
 *   - the toughness ladder and its availability (only classes whose model
 *     the loaded data has), with promote-fast / demote-slow hysteresis;
 *   - fork-burst detection and the swarm budget;
 *   - the killed set;
 *   - wound and kill: a post or delete event, with the kill policy's mode
 *     (psq_policy_mode), sent in every mode and only through
 *     psd_proc_renice_checked / psd_proc_kill_checked. The backend is the
 *     bundled Summoner (psproc's script backend), the only path to real
 *     processes; it decides what the mode lets happen.
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

/* Room for the curated pool. The pool itself is the live-monster cap
 * (psproc_monster_cap, at most 35): the backend's top N, killed ones left
 * out, are the processes that get monsters. */
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


/* The same rule for an on/off option that widens what can reach a process
 * (psq_infighting): once untrusted, it may only be turned off. */
int psq_flag_change_allowed(int untrusted, int old_value, int new_value);

/* The kill mode (PSD_KILL_*) is chosen only on the Options > psQuake page
 * and is never saved: every launch starts at PSD_KILL_RENICE. The page asks
 * this before each change. */
typedef enum
{
    PSQ_KILLMODE_CHANGED,    /* apply `requested`                          */
    PSQ_KILLMODE_UNCHANGED,  /* already `requested`                        */
    PSQ_KILLMODE_CONFIRM,    /* live, but the warning is not confirmed:
                                show it, change nothing                    */
    PSQ_KILLMODE_UNTRUSTED,  /* a raise after a server, demo or game code
                                ran commands: refused until restart        */
    PSQ_KILLMODE_INVALID     /* not a PSD_KILL_* mode                      */
} psq_killmode_result_t;

/* What a request to go from `current` to `requested` does. Lowering is
 * always allowed. Raising is refused once `untrusted`; raising to
 * PSD_KILL_LIVE also needs `live_confirmed`, which only the page's warning,
 * answered yes, passes. */
psq_killmode_result_t psq_killmode_change(int current, int requested,
                                          int live_confirmed, int untrusted);

/* 1 if text, which QuakeC passes to localcmd or stuffcmd, is one of the
 * stock game code's own commands ("bf\n", "-attack\n", "restart\n"),
 * matched whole. Anything else, even a harmless-looking fragment (the
 * console buffer joins fragments), marks the session untrusted, as a
 * server's or demo's commands do. NULL is not trusted. */
int psq_qc_command_trusted(const char *text);

/* 1 if text, which a remote server or a demo stuffs into the console
 * buffer, can run no command at all: it holds no ';' (the buffer's other
 * line break), and every line of it, after leading white space, is empty
 * or starts with "//" (a comment to the end of the line). Joined to what
 * the buffer already holds, such text can only turn the rest of a line
 * into a comment, never add a command. LibreQuake's attract demos stuff
 * "//st 15 0\n" while they play behind the main menu. Anything else marks
 * the session untrusted. NULL is not inert. */
int psq_stuff_text_inert(const char *text);

/* 1 if name, which QuakeC passes to changelevel (usually a map's own "map"
 * key), is a plain map name: 1..PSQ_MAP_NAME_MAX-1 characters of letters,
 * digits, '_', '-', '.' and '/', not starting with '/' and with no "..".
 * The engine puts it in the console buffer as `changelevel <name>`, so a
 * ';', quote, space or control character would run further commands. */
#define PSQ_MAP_NAME_MAX 64
int psq_map_name_plain(const char *name);

/* 1 if process monsters may act on this level: a map that allows them
 * (psq_map_allows_processes: deathmatch, psQuake and "arena" maps) always; any other
 * level only with psq_testarena on and the session still trusted (a server,
 * demo or game code that stuffed commands cannot turn the test arena on). */
int psq_level_allowed(int map_allowed, int testarena, int untrusted);

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

/* Entities the last refresh left out because their id is over PSQ_PID_MAX
 * (QuakeC cannot hold it). */
int psq_policy_ids_too_big(void);

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

/* The script contract's mode for a kill policy (PSD_KILL_*): simulate ->
 * "dryrun", renice -> "post-only", live -> "delete"; anything else
 * "dryrun". Sent as mode= with every post and delete. */
const char *psq_policy_mode(int kill_policy);

/* The ladder rung for a grade (0..1, clamped; NaN is 0): tier
 * min(floor(grade * N), N - 1) among the N rungs with avail[i] set, as its
 * ladder index; -1 if none is available. */
int psq_grade_rung(double grade, const int *avail, int n);

/* Outcome of psq_policy_wound / psq_policy_kill. */
typedef enum
{
    PSQ_OUT_GATED = 0,    /* gate closed: nothing considered               */
    PSQ_OUT_NOT_PROCESS,  /* pid <= 1 (an ordinary monster)                */
    PSQ_OUT_UNKNOWN,      /* pid not in the pool: refused                  */
    PSQ_OUT_KILLED,       /* already killed this level: nothing more       */
    PSQ_OUT_GONE,         /* a checked action already found it gone/reused */
    PSQ_OUT_NO_DAMAGE,    /* wound with take <= 0                          */
    PSQ_OUT_POLICY,       /* unused: every mode sends the event, with its
                           * mode (kept so the numbering stays)           */
    PSQ_OUT_SWARM,        /* a swarm member ended: budget returned         */
    PSQ_OUT_FULL,         /* the level's killed set is full: no action     */
    PSQ_OUT_ACTED,        /* a checked backend action ran; see `act`       */
} psq_outcome_t;

typedef struct
{
    psq_outcome_t    outcome;
    psd_act_result_t act;   /* valid for PSQ_OUT_ACTED */
} psq_result_t;

/* A process monster survived a hit of `take`: send `post` (amount
 * PSQ_WOUND_NICE) with the kill policy's mode, in every mode. */
psq_result_t psq_policy_wound(int pid, double take);

/* A process monster died, by any cause: send `delete` with the kill
 * policy's mode, in every mode, and remember it as killed for the level
 * (the pool drops it). PSQ_SWARM_PID returns one member to the swarm
 * budget. If the checked kill finds the id gone or reused, nothing was sent
 * and it is not remembered as killed. */
psq_result_t psq_policy_kill(int pid);

/* Live swarm members as the policy counts them (tests and the debug log). */
int psq_policy_swarm_live(void);

#ifdef __cplusplus
}
#endif

#endif /* PSQ_POLICY_H */
