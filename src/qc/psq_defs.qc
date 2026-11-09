/*
	psq_defs.qc -- psQuake process monsters: builtins, fields, constants.

	Included right after defs.qc (third_party/librequake-qc/qcsrc/progs.src)
	so combat.qc's hooks can use them. The builtins are the engine's
	#1200-#1208 (src/engine/psq_builtins.c). Every one is a no-op unless the
	game is local single player; the engine decides every process question
	and acts only on pids its latest poll handed out.

	MIT licensed, like the rest of psQuake's own files. Compiled only into the
	GPL-2 progs.dat.
*/

float()                 psq_poll       = #1200;	// refresh (1 Hz in the engine); pool size
float(float i)          psq_proc_pid   = #1201;	// pid of pool entry i
string(float i)         psq_proc_name  = #1202;	// its name, stable for the level
string(float i)         psq_class      = #1203;	// its committed monster classname
float(float pid)        psq_burst      = #1204;	// swarm members owed now (consumed)
void(float pid, float take) psq_wound  = #1205;	// a hit it survived
void(float pid)         psq_kill       = #1206;	// it died (PSQ_SWARM: a swarm member ended)
float(float pid)        psq_present    = #1207;	// still listed and not killed
string(float t)         psq_tier_class = #1208;	// t-th available class; -1 the swarm class

// psq_pid values. 0 is an ordinary entity; > 1 a live process monster.
#define PSQ_SWARM	-1	// a fork-swarm member: stands for no process
#define PSQ_CORPSE	-3	// a process monster or swarm member that died

.float	psq_pid;
.string	psq_name;
.float	psq_counted;	// what this monster added to total_monsters
.void()	psq_startfn;	// the monster's own start think, run by psq_go

void(entity targ) psq_died;	// psq.qc: Killed calls it before th_die
