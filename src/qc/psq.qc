/*
	psq.qc -- psQuake process monsters: the controller.

	A psq_arena entity (placed by the arena map, or made by worldspawn when
	psq_testarena is set) precaches every available monster class while the
	level loads, then thinks once a second:

	  1. psq_poll()             the engine refreshes its curated process pool
	  2. retire                 a live process monster whose process is no
	                            longer present teleports out (fog, sound) and
	                            leaves the level's monster total
	  3. spawn                  each pool process without a monster gets one,
	                            up to psq_cap live, teleported onto a free cell
	                            of the arena grid (one monster per cell), or
	                            near the player when the grid is full or the
	                            controller has none (psq_testarena)
	  4. morph                  a monster whose committed class changed (the
	                            engine applies the hysteresis) becomes the new
	                            class in place, keeping its health fraction
	  5. swarm                  a parent the engine says is fork-bursting gets
	                            Scrags (psq_pid PSQ_SWARM: they act on nothing)

	Monsters count toward the level total; the client hears every change
	(svc_updatestat STAT_TOTALMONSTERS). A killed process monster dies
	normally and leaves an ordinary corpse (combat.qc Killed -> psq_died).

	Needs the engine's relaxed precache (pr_cmds.c): an already cached name
	returns quietly after load, so a stock spawn function can run mid-level.
	Near-player placement and the wake-up are the spawn spike's; the grid
	cells come from the engine (psq_grid_cell). Both are proven headless.

	MIT licensed, like the rest of psQuake's own files. Compiled only into the
	GPL-2 progs.dat.
*/

#define PSQ_SVC_UPDATESTAT		3
#define PSQ_STAT_TOTALMONSTERS	12
#define PSQ_CAP_MIN				5
#define PSQ_CAP_MAX				35

entity	psq_controller;		// the one psq_arena entity of this level
float	psq_fog_sounded;	// a teleport sound already played this think

void(string s, ...) psq_dprint = #25;	// dprint with varargs

void(string s1, string s2, string s3, string s4, string s5, string s6) psq_log =
{
	if (cvar ("psq_debug"))
		psq_dprint ("psq: ", s1, s2, s3, s4, s5, s6, "\n");
};

// --------------------------------------------------------------- classes

// Run the stock spawn function of class cls on self. 1 if cls is known.
float(string cls) psq_call =
{
	if (cls == "monster_dog") monster_dog ();
	else if (cls == "monster_army") monster_army ();
	else if (cls == "monster_enforcer") monster_enforcer ();
	else if (cls == "monster_knight") monster_knight ();
	else if (cls == "monster_ogre") monster_ogre ();
	else if (cls == "monster_hell_knight") monster_hell_knight ();
	else if (cls == "monster_demon1") monster_demon1 ();
	else if (cls == "monster_shalrath") monster_shalrath ();
	else if (cls == "monster_shambler") monster_shambler ();
	else if (cls == "monster_wizard") monster_wizard ();
	else return FALSE;
	return TRUE;
};

// Run cls's spawn function on a throwaway entity: while the level loads
// this precaches its models and sounds (so the list cannot drift from the
// monster code); later it measures its size. Leaves the entity in
// psq_probe for the caller to read and remove. 1 if cls is known.
entity	psq_probe;

float(string cls) psq_make_probe =
{
	local entity oself;
	local float tm, ok;

	oself = self;
	tm = total_monsters;
	psq_probe = spawn ();
	psq_probe.classname = cls;
	self = psq_probe;
	ok = psq_call (cls);
	self = oself;
	total_monsters = tm;	// a probe is not a monster
	psq_probe.solid = SOLID_NOT;
	psq_probe.think = SUB_Null;
	psq_probe.nextthink = 0;
	setorigin (psq_probe, psq_probe.origin);
	return ok;
};

void() psq_precache_all =
{
	local float t;
	local string cls;

	t = -1;	// the swarm class, then the ladder
	while (1)
	{
		cls = psq_tier_class (t);
		if (cls == "")
		{
			if (t >= 0)
				break;
		}
		else
		{
			psq_make_probe (cls);
			remove (psq_probe);
		}
		t = t + 1;
	}
};

// --------------------------------------------------------------- helpers

entity() psq_player =
{
	local entity p;
	p = find (world, classname, "player");
	while (p)
	{
		if (p.health > 0)
			return p;
		p = find (p, classname, "player");
	}
	return world;
};

void() psq_send_total =
{
	WriteByte (MSG_ALL, PSQ_SVC_UPDATESTAT);
	WriteByte (MSG_ALL, PSQ_STAT_TOTALMONSTERS);
	WriteLong (MSG_ALL, total_monsters);
};

// Teleport flash at org; only the first in a think also plays the sound, so
// a burst of arrivals and exits does not take every sound channel.
void(vector org) psq_fog =
{
	if (!psq_fog_sounded)
	{
		psq_fog_sounded = TRUE;
		spawn_tfog (org);
		return;
	}
	WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
	WriteByte (MSG_BROADCAST, TE_TELEPORT);
	WriteCoord (MSG_BROADCAST, org_x);
	WriteCoord (MSG_BROADCAST, org_y);
	WriteCoord (MSG_BROADCAST, org_z);
};

// The live process monster for pid, or world.
entity(float pid) psq_find =
{
	local entity e;
	e = nextent (world);
	while (e)
	{
		if (e.psq_pid == pid && e.health > 0)
			return e;
		e = nextent (e);
	}
	return world;
};

float() psq_live_count =
{
	local entity e;
	local float n;
	n = 0;
	e = nextent (world);
	while (e)
	{
		if (e.psq_pid > 1 && e.health > 0)
			n = n + 1;
		e = nextent (e);
	}
	return n;
};

// Pool index of pid in this poll, or -1.
float(float pid, float n) psq_index_of =
{
	local float i;
	i = 0;
	while (i < n)
	{
		if (psq_proc_pid (i) == pid)
			return i;
		i = i + 1;
	}
	return -1;
};

// ------------------------------------------------------------- placement

// Is pos clear for self (model and size already set)? Leaves self at the
// tested spot. The caller keeps self SOLID_NOT during the search, so a
// rejected spot inside a trigger fires nothing.
float(vector pos, float wantwater) psq_trypos =
{
	local float pc, pf;

	pc = pointcontents (pos);
	if (pc == CONTENT_SOLID || pc == CONTENT_SKY || pc == CONTENT_LAVA || pc == CONTENT_SLIME)
		return FALSE;
	setorigin (self, pos);
	if (!droptofloor ())
		return FALSE;
	pc = pointcontents (self.origin);
	pf = pointcontents (self.origin + '0 0 1' * (self.mins_z + 2));
	if (pc == CONTENT_LAVA || pc == CONTENT_SLIME || pf == CONTENT_LAVA || pf == CONTENT_SLIME)
		return FALSE;
	if (wantwater && pc != CONTENT_WATER)
		return FALSE;
	if (!walkmove (0, 0))
		return FALSE;		// hull overlaps the world or another entity
	return TRUE;
};

// 8 directions at each distance from dmin to dmax around center, starting at
// yaw; every spot must be in line of sight from eye.
float(vector center, vector eye, float yaw, entity ignore, float dmin, float dmax) psq_ring =
{
	local float d, i;
	local vector pos;

	d = dmin;
	while (d <= dmax)
	{
		i = 0;
		while (i < 8)
		{
			makevectors ('0 1 0' * (yaw + i * 45 + (random () - 0.5) * 20));
			pos = center + v_forward * d + '0 0 16';
			traceline (eye, pos, TRUE, ignore);
			if (trace_fraction == 1 && !trace_startsolid)
				if (psq_trypos (pos, FALSE))
					return TRUE;
			i = i + 1;
		}
		d = d + 96;
	}
	return FALSE;
};

float(string cls) psq_spots_of =
{
	local entity e;
	e = find (world, classname, cls);
	while (e)
	{
		if (psq_ring (e.origin, e.origin + '0 0 16', random () * 360, world, 0, 288))
			return TRUE;
		e = find (e, classname, cls);
	}
	return FALSE;
};

// Place self on a free cell of the arena grid: the engine names free cells
// (no solid entity reaches into them, none on or against the player) from a
// random start, and each is tested here for walls and the floor. Sets
// self.psq_gridcell. FALSE if the controller has no grid or none of the first
// PSQ_GRID_TRIES free cells fits (each try scans the grid once).
#define PSQ_GRID_TRIES	8

float() psq_gridspot =
{
	local float cells, start, k, c;

	if (!psq_controller)
		return FALSE;
	cells = psq_controller.psq_cols * psq_controller.psq_rows;
	start = floor (random () * cells);
	k = 0;
	while (k < PSQ_GRID_TRIES)
	{
		c = psq_grid_cell (psq_controller, start, k);
		if (c < 0)
			return FALSE;
		// Stand 24 units up (every class's mins_z is -24), plus a little
		// for droptofloor to settle it.
		if (psq_trypos (psq_grid_spot (psq_controller, c) + '0 0 32', FALSE))
		{
			self.psq_gridcell = c;
			return TRUE;
		}
		psq_log ("grid cell ", ftos (c), " does not fit ", self.classname, "", "");
		k = k + 1;
	}
	return FALSE;
};

// Place self near the player, in sight; else around the map's spawn spots.
float() psq_findspot =
{
	local entity p;

	p = psq_player ();
	if (p)
		if (psq_ring (p.origin, p.origin + p.view_ofs, p.angles_y - 22, p, 160, 448))
			return TRUE;
	if (psq_spots_of ("info_teleport_destination")) return TRUE;
	if (psq_spots_of ("info_player_deathmatch")) return TRUE;
	if (psq_spots_of ("info_player_coop")) return TRUE;
	if (psq_spots_of ("info_player_start")) return TRUE;
	return FALSE;
};

// Runs in place of the monster's own start think: runs it, then wakes the
// monster hunting the player, as monster_use does for a map's teleport-ins.
void() psq_go =
{
	local void() f;
	local float before;
	local entity p;

	f = self.psq_startfn;
	self.psq_startfn = SUB_Null;
	before = total_monsters;
	f ();
	if (total_monsters != before)
	{
		self.psq_counted = self.psq_counted + total_monsters - before;
		psq_send_total ();
	}
	if (self.think == walkmonster_start_go || self.think == flymonster_start_go)
	{	// a deferred start: run that next, through psq_go again
		self.psq_startfn = self.think;
		self.think = psq_go;
		return;
	}
	if (self.health <= 0)
		return;
	p = psq_player ();
	if (!p)
		return;
	if (p.flags & FL_NOTARGET || p.items & IT_INVISIBILITY)
		return;
	self.enemy = p;
	FoundTarget ();
};

// Teleport a new monster of class cls in. Placed near `near` if it is not
// world (a swarm around its parent), else near the player. world if no spot.
entity(string cls, entity near) psq_spawn_class =
{
	local entity oself, mon, p;
	local float before, ok;

	oself = self;
	mon = spawn ();
	mon.classname = cls;
	self = mon;
	self.nextthink = time;		// the start functions add 0.1-0.6 s to this
	before = total_monsters;
	if (!psq_call (cls))
	{
		self = oself;
		remove (mon);
		return world;
	}
	mon.psq_counted = total_monsters - before;

	mon.solid = SOLID_NOT;		// search without touching triggers
	mon.psq_gridcell = -1;
	if (near != world)
		ok = psq_ring (near.origin, near.origin + '0 0 16', random () * 360, near, 64, 256);
	else
	{
		ok = psq_gridspot ();
		if (!ok)
		{
			if (psq_controller.psq_cell > 0)
				psq_log ("grid full for ", cls, ": placing near the player", "", "", "");
			ok = psq_findspot ();
		}
	}
	mon.solid = SOLID_SLIDEBOX;
	setorigin (mon, mon.origin);
	if (!ok)
	{
		total_monsters = before;
		self = oself;
		remove (mon);
		psq_log ("no clear spot for ", cls, "", "", "", "");
		return world;
	}

	p = psq_player ();
	if (p)
		mon.angles_y = vectoyaw (p.origin - mon.origin);
	mon.psq_startfn = mon.think;
	mon.think = psq_go;
	psq_fog (mon.origin);
	self = oself;
	return mon;
};

// ------------------------------------------------------------ lifecycle

// A live process monster or swarm member leaves: fog and sound, no gibs, no
// kill credit, and it leaves the level total.
void(entity e, string why) psq_teleport_out =
{
	psq_log ("teleport out ", e.classname, " pid ", ftos (e.psq_pid), " ", why);
	total_monsters = total_monsters - e.psq_counted;
	psq_fog (e.origin);
	// The AI drops an enemy only at health <= 0: without this, a monster
	// fighting e would chase the freed slot.
	e.health = 0;
	e.psq_pid = 0;
	remove (e);
};

// combat.qc Killed, before th_die: a process monster's process is killed
// (as the engine's policy allows) and remembered; a swarm member returns
// its budget. Either way the corpse acts on nothing from now on.
void(entity targ) psq_died =
{
	if (targ.psq_pid > 1 || targ.psq_pid == PSQ_SWARM)
	{
		psq_log ("killed ", targ.classname, " pid ", ftos (targ.psq_pid), "", "");
		psq_kill (targ.psq_pid);
		targ.psq_pid = PSQ_CORPSE;
	}
};

void() psq_retire =
{
	local entity e, next;

	e = nextent (world);
	while (e)
	{
		next = nextent (e);
		if (e.psq_pid > 1 && e.health > 0 && !psq_present (e.psq_pid))
			psq_teleport_out (e, "(process left the list)");
		e = next;
	}
};

void(float n) psq_spawn_missing =
{
	local float i, pid, live, cap;
	local string cls;
	local entity mon;

	cap = cvar ("psq_cap");
	if (cap < PSQ_CAP_MIN) cap = PSQ_CAP_MIN;
	if (cap > PSQ_CAP_MAX) cap = PSQ_CAP_MAX;
	live = psq_live_count ();
	i = 0;
	while (i < n && live < cap)
	{
		pid = psq_proc_pid (i);
		if (pid > 1 && psq_present (pid) && !psq_find (pid))
		{
			cls = psq_class (i);
			if (cls != "")
			{
				mon = psq_spawn_class (cls, world);
				if (mon)
				{
					mon.psq_pid = pid;
					mon.psq_name = psq_proc_name (i);
					live = live + 1;
					psq_log ("spawned ", cls, " for pid ", ftos (pid), " ", mon.psq_name);
					if (mon.psq_gridcell >= 0)
						psq_log ("pid ", ftos (pid), " in grid cell ", ftos (mon.psq_gridcell), " at ", vtos (mon.origin));
				}
			}
		}
		i = i + 1;
	}
};

// Turn e into class cls where it stands. Defers (returns FALSE, the next
// poll tries again) while e has not started yet or the new size does not
// fit there.
float(entity e, string cls) psq_morph =
{
	local entity oself, en, oen, goal;
	local vector org, ang, omins, omaxs, nmins, nmaxs;
	local float frac, tm, fits;
	local string from;

	if (!e.takedamage || e.think == psq_go || e.max_health <= 0)
		return FALSE;
	if (!psq_make_probe (cls))
	{
		remove (psq_probe);
		return FALSE;
	}
	nmins = psq_probe.mins;
	nmaxs = psq_probe.maxs;
	remove (psq_probe);

	oself = self;
	self = e;
	org = e.origin;
	omins = e.mins;
	omaxs = e.maxs;
	// Test the new size SOLID_NOT, as placement does: the engine then links
	// e without touching triggers, so the test can neither hurt nor
	// teleport it. walkmove may still step it, so its spot is put back.
	e.solid = SOLID_NOT;
	setsize (e, nmins, nmaxs);
	fits = walkmove (0, 0);
	setsize (e, omins, omaxs);
	e.solid = SOLID_SLIDEBOX;
	setorigin (e, org);
	if (!fits || e.health <= 0 || e.psq_pid <= 1)
	{
		self = oself;
		return FALSE;
	}

	from = e.classname;
	ang = e.angles;
	en = e.enemy;
	oen = e.oldenemy;
	goal = e.goalentity;
	frac = e.health / e.max_health;
	tm = total_monsters;

	e.classname = cls;
	psq_call (cls);			// model, size, health, th_* of the new class
	total_monsters = tm;	// its walkmonster_start counted it again
	e.health = ceil (frac * e.max_health);
	if (e.health < 1)
		e.health = 1;
	e.enemy = en;
	e.oldenemy = oen;
	e.goalentity = goal;
	e.angles = ang;
	setorigin (e, org);
	if (e.enemy)
		e.think = e.th_run;
	else
		e.think = e.th_stand;
	e.nextthink = time + 0.1;
	psq_fog (org);
	self = oself;
	psq_log ("morphed pid ", ftos (e.psq_pid), " ", from, " -> ", cls);
	return TRUE;
};

void(float n) psq_morph_all =
{
	local entity e, next;
	local float i;
	local string cls;

	e = nextent (world);
	while (e)
	{
		next = nextent (e);
		if (e.psq_pid > 1 && e.health > 0)
		{
			i = psq_index_of (e.psq_pid, n);
			if (i >= 0)
			{
				cls = psq_class (i);
				if (cls != "" && cls != e.classname)
					psq_morph (e, cls);
			}
		}
		e = next;
	}
};

void() psq_swarm_all =
{
	local entity e, next, m;
	local float owed;
	local string cls;

	cls = psq_tier_class (-1);
	e = nextent (world);
	while (e)
	{
		next = nextent (e);
		if (e.psq_pid > 1 && e.health > 0)
		{
			owed = psq_burst (e.psq_pid);
			while (owed > 0)
			{
				m = world;
				if (cls != "")
					m = psq_spawn_class (cls, e);
				if (m)
				{
					m.psq_pid = PSQ_SWARM;
					m.psq_name = "";
				}
				else
					psq_kill (PSQ_SWARM);	// never placed: return its budget
				owed = owed - 1;
			}
		}
		e = next;
	}
};

void() psq_think =
{
	local float n, tm;

	self.nextthink = time + 1;
	tm = total_monsters;
	psq_fog_sounded = FALSE;
	n = psq_poll ();
	psq_retire ();
	psq_spawn_missing (n);
	psq_morph_all (n);
	psq_swarm_all ();
	if (total_monsters != tm)
		psq_send_total ();
};

/*QUAKED psq_arena (0 .5 .8) (-8 -8 -8) (8 8 8)
psQuake's process controller. Live processes become monsters while the
level runs (local single player only). One per level; extra ones remove
themselves.
*/
void() psq_arena =
{
	if (deathmatch || coop)
	{
		remove (self);
		return;
	}
	if (psq_controller)
	{	// psq_testarena made one first: it takes this one's grid
		if (self.psq_cell > 0 && !(psq_controller.psq_cell > 0))
		{
			setorigin (psq_controller, self.origin);
			psq_controller.psq_cols = self.psq_cols;
			psq_controller.psq_rows = self.psq_rows;
			psq_controller.psq_cell = self.psq_cell;
		}
		remove (self);
		return;
	}
	psq_controller = self;
	self.solid = SOLID_NOT;
	self.movetype = MOVETYPE_NONE;
	psq_precache_all ();
	if (self.psq_cell > 0)
		psq_log ("arena grid ", ftos (self.psq_cols), " x ", ftos (self.psq_rows), " cells of ", ftos (self.psq_cell));
	self.think = psq_think;
	self.nextthink = time + 1;
};

// worldspawn, last: with psq_testarena set, any map gets a controller, until
// the arena map that places psq_arena exists.
void() psq_worldspawn =
{
	local entity oself;

	psq_controller = world;
	if (!cvar ("psq_testarena") || deathmatch || coop)
		return;
	oself = self;
	self = spawn ();
	self.classname = "psq_arena";
	psq_arena ();
	self = oself;
};
