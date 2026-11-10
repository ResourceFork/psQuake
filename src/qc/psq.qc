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
	  6. resupply               on an arena with psq_supply spots, health, ammo
	                            and weapons teleport in on a timer
	                            (psq_resupply, below)

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
// Inside a psq_safe zone (the arenas' start room)? No monster is placed
// there, by the grid, the near-player fallback or a swarm.
float(vector p) psq_in_safe =
{
	local entity z;

	z = find (world, classname, "psq_safe");
	while (z)
	{
		if (p_x >= z.absmin_x && p_x <= z.absmax_x && p_y >= z.absmin_y
			&& p_y <= z.absmax_y && p_z >= z.absmin_z && p_z <= z.absmax_z)
			return TRUE;
		z = find (z, classname, "psq_safe");
	}
	return FALSE;
};

float(vector pos, float wantwater) psq_trypos =
{
	local float pc, pf;

	pc = pointcontents (pos);
	if (pc == CONTENT_SOLID || pc == CONTENT_SKY || pc == CONTENT_LAVA || pc == CONTENT_SLIME)
		return FALSE;
	if (psq_in_safe (pos))
		return FALSE;
	setorigin (self, pos);
	if (!droptofloor ())
		return FALSE;
	if (psq_in_safe (self.origin))
		return FALSE;		// it dropped into one (the escape balcony's window sill)
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

// The arena's spawn grids: the controller's own, then each psq_grid entity
// the map places (one grid per floor region, tools/generate_arena.py
// --layout). psq_gridcell numbers the cells of every grid in that order, so
// on a map with one grid it is that grid's cell.
// A grid the engine accepts (psq_grid_from_keys): a finite origin, whole cols
// and rows in 1..32, a cell of 64..1024 units. Any other counts no cells, so
// it is never picked and the cell numbering matches the engine's. Each test
// is written so a NaN fails it (every comparison with NaN is false).
float(entity g) psq_grid_size =
{
	if (!(g.origin_x - g.origin_x == 0 && g.origin_y - g.origin_y == 0
		&& g.origin_z - g.origin_z == 0))
		return 0;	// NaN or infinite
	if (!(g.psq_cell >= 64 && g.psq_cell <= 1024))
		return 0;
	if (!(g.psq_cols >= 1 && g.psq_cols <= 32 && g.psq_cols == floor (g.psq_cols)))
		return 0;
	if (!(g.psq_rows >= 1 && g.psq_rows <= 32 && g.psq_rows == floor (g.psq_rows)))
		return 0;
	return g.psq_cols * g.psq_rows;
};

entity(entity g) psq_grid_next =
{
	if (g == psq_controller)
		return find (world, classname, "psq_grid");
	return find (g, classname, "psq_grid");
};

// Place self on a free cell of one of the arena's grids. A grid is picked by
// its share of all the cells; the engine names the first free cell in it from
// a random start (no solid entity reaches into it, none on or against the
// player), or the next grid's when it is full; each is tested here for walls
// and the floor. Sets self.psq_gridcell. FALSE if there is no controller or no
// usable grid, every grid is full, or none of PSQ_GRID_TRIES picks fits.
#define PSQ_GRID_TRIES	8

float() psq_gridspot =
{
	local float total, grids, pick, base, k, c, seen;
	local entity g;

	if (!psq_controller)
		return FALSE;
	total = 0;
	grids = 0;
	g = psq_controller;
	while (g)
	{
		total = total + psq_grid_size (g);
		grids = grids + 1;
		g = psq_grid_next (g);
	}
	if (total <= 0)
		return FALSE;
	k = 0;
	while (k < PSQ_GRID_TRIES)
	{
		pick = floor (random () * total);
		if (pick >= total)
			pick = total - 1;	// random () can return 1
		g = psq_controller;
		base = 0;
		while (g && pick >= base + psq_grid_size (g))
		{
			base = base + psq_grid_size (g);
			g = psq_grid_next (g);
		}
		if (!g)
			return FALSE;	// cannot happen: pick < total
		c = psq_grid_cell (g, pick - base, 0);
		seen = 1;
		while (c < 0 && seen < grids)
		{	// full: the next grid, wrapping to the controller's
			base = base + psq_grid_size (g);
			g = psq_grid_next (g);
			if (!g)
			{
				g = psq_controller;
				base = 0;
			}
			c = psq_grid_cell (g, floor (random () * psq_grid_size (g)), 0);
			seen = seen + 1;
		}
		if (c < 0)
			return FALSE;
		// Stand 24 units up (every class's mins_z is -24), plus a little
		// for droptofloor to settle it.
		if (psq_trypos (psq_grid_spot (g, c) + '0 0 32', FALSE))
		{
			self.psq_gridcell = base + c;
			return TRUE;
		}
		psq_log ("grid cell ", ftos (base + c), " does not fit ", self.classname, "", "");
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
			if (psq_controller.psq_cell > 0 || find (world, classname, "psq_grid"))
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

// combat.qc T_Damage, before the damage: with psq_infighting 0 (the
// default) a monster's attack on a process monster or Scrag, or theirs on
// another monster, does no damage, so only the player wounds and kills
// process monsters. They may still turn on each other, as a hit would make
// them. Ordinary monsters keep stock infighting. TRUE means: no damage.
float(entity targ, entity attacker) psq_infight_blocked =
{
	local entity oself;

	if (cvar ("psq_infighting"))
		return FALSE;
	if (attacker == targ || !(attacker.flags & FL_MONSTER) || !(targ.flags & FL_MONSTER))
		return FALSE;
	if (!attacker.psq_pid && !targ.psq_pid)
		return FALSE;
	if (attacker.health > 0 && targ.health > 0 && targ.enemy != attacker)
		if (targ.classname != attacker.classname || targ.classname == "monster_army")
		{
			oself = self;
			self = targ;
			if (self.enemy.classname == "player")
				self.oldenemy = self.enemy;
			self.enemy = attacker;
			FoundTarget ();
			self = oself;
		}
	return TRUE;
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

// --------------------------------------------------------------- resupply
//
// The arena maps place psq_supply spots (tools/generate_arena.py). There
// the controller teleports in more health and ammo as the fight goes on,
// with the arrival fog and sound the process monsters have:
//
//   - a health pack or an ammo box every psq_supply_every seconds, from
//     psq_supply_first seconds into the arena; every psq_supply_hard
//     seconds instead while the player is low (under psq_supply_low health)
//     or psq_supply_busy or more process monsters are live, and then ammo
//     comes in big boxes;
//   - never while psq_supply_max of them lie unclaimed (a drop that is due
//     waits for one to be picked up);
//   - each stronger weapon once, as soon as the arena has run as long as its
//     unlock time.
//
// The threat measure is the time spent in this arena: it rises in every
// game on every machine, while the monsters' strength is set by the user's
// processes and does not rise during play. Items go to a free spot (none
// already holding an unclaimed drop, none within 96 units of the player).
// Only maps with psq_supply spots get any: the campaign stays stock. Items
// need no save handling (saves are refused in the arenas).
//
// The tunables, in seconds, health points and counts; the headless test
// reads these same globals (var: fteqcc would fold a never-written float
// into a nameless constant).
var float	psq_supply_first = 20;		// the first drop, seconds into the arena
var float	psq_supply_every = 25;		// seconds between drops
var float	psq_supply_hard = 12;		// ... while the player is low or monsters are many
var float	psq_supply_low = 50;		// health under which the player counts as low
var float	psq_supply_busy = 5;		// live process monsters that count as many
var float	psq_supply_max = 4;		// unclaimed health and ammo drops at most
var float	psq_unlock_supershotgun = 30;	// seconds into the arena, each weapon once
var float	psq_unlock_nailgun = 60;
var float	psq_unlock_supernailgun = 100;
var float	psq_unlock_grenadelauncher = 140;
var float	psq_unlock_rocketlauncher = 200;
var float	psq_unlock_lightning = 280;

// State, per level (every level load reloads the progs, so these start at 0).
float	psq_supply_t0;		// when the controller first thought
float	psq_supply_next;	// when the next health or ammo drop is due
float	psq_supply_drops;	// health and ammo drops so far
float	psq_weapons_given;	// weapons dropped so far (the next to unlock)
float	psq_supply_cached;	// every item's model and sound precached
entity	psq_weapon_pending;	// the weapon dropped last think, not yet counted

#define PSQ_SUPPLY_ITEM		1	// .psq_supplied: health or ammo
#define PSQ_SUPPLY_WEAPON	2	// .psq_supplied: a weapon

.float	psq_supplied;	// a resupplied item, which kind
.entity	psq_spot;		// on an item: the spot it arrived at
.entity	psq_held;		// on a spot: the item that arrived there

string(float i) psq_unlock_class =
{
	if (i == 0) return "weapon_supershotgun";
	if (i == 1) return "weapon_nailgun";
	if (i == 2) return "weapon_supernailgun";
	if (i == 3) return "weapon_grenadelauncher";
	if (i == 4) return "weapon_rocketlauncher";
	if (i == 5) return "weapon_lightning";
	return "";
};

float(float i) psq_unlock_time =
{
	if (i == 0) return psq_unlock_supershotgun;
	if (i == 1) return psq_unlock_nailgun;
	if (i == 2) return psq_unlock_supernailgun;
	if (i == 3) return psq_unlock_grenadelauncher;
	if (i == 4) return psq_unlock_rocketlauncher;
	return psq_unlock_lightning;
};

// Run the stock spawn function of item class cls on self.
void(string cls) psq_supply_call =
{
	if (cls == "item_health") item_health ();
	else if (cls == "item_shells") item_shells ();
	else if (cls == "item_spikes") item_spikes ();
	else if (cls == "item_rockets") item_rockets ();
	else if (cls == "item_cells") item_cells ();
	else if (cls == "weapon_supershotgun") weapon_supershotgun ();
	else if (cls == "weapon_nailgun") weapon_nailgun ();
	else if (cls == "weapon_supernailgun") weapon_supernailgun ();
	else if (cls == "weapon_grenadelauncher") weapon_grenadelauncher ();
	else if (cls == "weapon_rocketlauncher") weapon_rocketlauncher ();
	else if (cls == "weapon_lightning") weapon_lightning ();
};

// While the level loads: run each item's spawn function once on a probe and
// remove it, so every model and sound a drop needs is precached (the relaxed
// precache only lets an already cached name through later).
void(string cls, float flags) psq_supply_probe =
{
	local entity oself, e;

	oself = self;
	e = spawn ();
	e.classname = cls;
	e.spawnflags = flags;
	self = e;
	psq_supply_call (cls);
	self = oself;
	remove (e);
};

void() psq_supply_precache =
{
	local float i;

	if (psq_supply_cached)
		return;
	psq_supply_cached = TRUE;
	psq_supply_probe ("item_health", 0);
	psq_supply_probe ("item_shells", 0);
	psq_supply_probe ("item_shells", 1);
	psq_supply_probe ("item_spikes", 0);
	psq_supply_probe ("item_spikes", 1);
	psq_supply_probe ("item_rockets", 0);
	psq_supply_probe ("item_rockets", 1);
	psq_supply_probe ("item_cells", 0);
	psq_supply_probe ("item_cells", 1);
	i = 0;
	while (psq_unlock_class (i) != "")
	{
		psq_supply_probe (psq_unlock_class (i), 0);
		i = i + 1;
	}
};

// Unclaimed health and ammo drops; removes claimed drops (picked up, the
// item keeps its edict with no model in single player).
float() psq_supply_loose =
{
	local entity e, next;
	local float n;

	n = 0;
	e = nextent (world);
	while (e)
	{
		next = nextent (e);
		if (e.psq_supplied && e.psq_spot.classname == "psq_supply")
		{
			if (e.model == "" && e != psq_weapon_pending)
				remove (e);
			else if (e.psq_supplied == PSQ_SUPPLY_ITEM)
				n = n + 1;
		}
		e = next;
	}
	return n;
};

// A monster or the player at or beside the spot: an item dropped there
// would start inside it and fall out of the level.
float(entity sp) psq_spot_crowded =
{
	local entity e;

	e = findradius (sp.origin + '8 8 0', 112);   // a hull-2 box on either box's corner
	while (e)
	{
		if (e.flags & (FL_MONSTER | FL_CLIENT))
			return TRUE;
		e = e.chain;
	}
	return FALSE;
};

// A free spot from a random start, or world: none holding an unclaimed drop,
// none crowded, none within 96 units of the player.
entity() psq_supply_spot =
{
	local entity sp, p;
	local float n, k, pick;

	n = 0;
	sp = find (world, classname, "psq_supply");
	while (sp)
	{
		n = n + 1;
		sp = find (sp, classname, "psq_supply");
	}
	if (n == 0)
		return world;
	p = psq_player ();
	pick = floor (random () * n);
	if (pick >= n)
		pick = n - 1;	// random () can return 1
	sp = find (world, classname, "psq_supply");
	k = 0;
	while (k < pick)
	{
		sp = find (sp, classname, "psq_supply");
		k = k + 1;
	}
	k = 0;
	while (k < n)
	{
		if (!(sp.psq_held && sp.psq_held.psq_spot == sp && sp.psq_held.psq_supplied
			&& sp.psq_held.model != ""))
			if (!p || vlen (sp.origin - p.origin) > 96)
				if (!psq_spot_crowded (sp))
					return sp;
		sp = find (sp, classname, "psq_supply");
		if (!sp)
			sp = find (world, classname, "psq_supply");
		k = k + 1;
	}
	return world;
};

entity(string cls, float flags, float kind, entity spot) psq_supply_drop =
{
	local entity oself, e;

	oself = self;
	e = spawn ();
	e.classname = cls;
	e.spawnflags = flags;
	setorigin (e, spot.origin);
	self = e;
	psq_supply_call (cls);
	self = oself;
	e.psq_supplied = kind;
	e.psq_spot = spot;
	spot.psq_held = e;
	psq_fog (spot.origin);
	psq_log ("supply ", cls, " at ", vtos (spot.origin), " t ", ftos (time - psq_supply_t0));
	return e;
};

// Which health or ammo comes next: health when the player is hurt, else the
// ammo the player's weapons are shortest of; with no player, health and the
// unlocked weapons' ammo in turn. Sets psq_pick_cls.
string	psq_pick_cls;

void(entity p) psq_supply_pick =
{
	local float f, best, k;

	if (!p)
	{
		k = psq_supply_drops - floor (psq_supply_drops / 5) * 5;
		psq_pick_cls = "item_health";
		if (k == 1) psq_pick_cls = "item_shells";
		if (k == 2 && psq_weapons_given > 1) psq_pick_cls = "item_spikes";
		if (k == 3 && psq_weapons_given > 3) psq_pick_cls = "item_rockets";
		if (k == 4 && psq_weapons_given > 5) psq_pick_cls = "item_cells";
		return;
	}
	if (p.health < 75)
	{
		psq_pick_cls = "item_health";
		return;
	}
	psq_pick_cls = "item_shells";
	best = p.ammo_shells / 100;
	if (p.items & (IT_NAILGUN | IT_SUPER_NAILGUN))
	{
		f = p.ammo_nails / 200;
		if (f < best) { best = f; psq_pick_cls = "item_spikes"; }
	}
	if (p.items & (IT_GRENADE_LAUNCHER | IT_ROCKET_LAUNCHER))
	{
		f = p.ammo_rockets / 100;
		if (f < best) { best = f; psq_pick_cls = "item_rockets"; }
	}
	if (p.items & IT_LIGHTNING)
	{
		f = p.ammo_cells / 100;
		if (f < best) { best = f; psq_pick_cls = "item_cells"; }
	}
	if (best >= 1)
		psq_pick_cls = "item_health";	// every weapon full
};

// Once a second, from the controller's think.
void() psq_resupply =
{
	local float t, hard, flags;
	local entity spot, p, dropped;
	local string cls;

	if (!find (world, classname, "psq_supply"))
		return;
	if (!psq_supply_t0)
	{
		psq_supply_t0 = time;
		psq_supply_next = time + psq_supply_first;
	}
	t = time - psq_supply_t0;
	// A weapon counts once it has landed (PlaceItem made it a trigger) or
	// been picked up; one that fell out of the level is dropped again, so no
	// unlock is ever lost. Fallen out, its edict is freed (no model, no
	// think, but psq_supplied kept), or already reused by something spawned
	// since (every field cleared): either way it no longer reads as ours.
	if (psq_weapon_pending)
	{
		if (psq_weapon_pending.psq_supplied != PSQ_SUPPLY_WEAPON
			|| psq_weapon_pending.psq_spot.classname != "psq_supply")
		{
			psq_weapons_given = psq_weapons_given - 1;	// reused: lost, again
			psq_log ("supply: ", psq_unlock_class (psq_weapons_given), " lost; again", "", "", "");
			psq_weapon_pending = world;
		}
		else if (psq_weapon_pending.model != "" && psq_weapon_pending.solid == SOLID_TRIGGER)
			psq_weapon_pending = world;
		else if (psq_weapon_pending.model == "" && psq_weapon_pending.nextthink != -1)
			psq_weapon_pending = world;	// picked up already
		else if (psq_weapon_pending.model == "")
		{
			psq_weapons_given = psq_weapons_given - 1;	// lost: again
			psq_log ("supply: ", psq_unlock_class (psq_weapons_given), " fell out; again", "", "", "");
			psq_weapon_pending = world;
		}
	}
	cls = psq_unlock_class (psq_weapons_given);
	if (!psq_weapon_pending && cls != "" && t >= psq_unlock_time (psq_weapons_given))
	{
		spot = psq_supply_spot ();
		if (spot)
		{
			psq_weapon_pending = psq_supply_drop (cls, 0, PSQ_SUPPLY_WEAPON, spot);
			psq_weapons_given = psq_weapons_given + 1;
		}
	}
	if (time < psq_supply_next || psq_supply_loose () >= psq_supply_max)
		return;
	spot = psq_supply_spot ();
	if (!spot)
		return;
	p = psq_player ();
	hard = psq_live_count () >= psq_supply_busy;
	if (p)
		if (p.health < psq_supply_low)
			hard = TRUE;
	psq_supply_pick (p);
	flags = 0;
	if (hard && psq_pick_cls != "item_health")
		flags = 1;	// the big box
	dropped = psq_supply_drop (psq_pick_cls, flags, PSQ_SUPPLY_ITEM, spot);
	psq_supply_drops = psq_supply_drops + 1;
	if (hard)
		psq_supply_next = time + psq_supply_hard;
	else
		psq_supply_next = time + psq_supply_every;
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
	psq_resupply ();
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

/*QUAKED psq_grid (0 .5 .8) (-8 -8 -8) (8 8 8)
A further spawn grid of the arena, with psq_arena's keys (origin its centre
on the floor, psq_cols, psq_rows, psq_cell). The level's controller places
process monsters on its cells as well as on its own grid's.
*/
void() psq_grid =
{
	if (deathmatch || coop)
	{
		remove (self);
		return;
	}
	self.solid = SOLID_NOT;
	self.movetype = MOVETYPE_NONE;
};

/*QUAKED psq_supply (0 .8 .3) (-8 -8 -8) (8 8 8)
A spot where the arena's controller teleports in health, ammo and, as time
goes on, weapons (psq_resupply). Placed by tools/generate_arena.py on open
floor, off the spawn grids; origin 16 above the floor, as an item's.
*/
void() psq_supply =
{
	if (deathmatch || coop)
	{
		remove (self);
		return;
	}
	self.solid = SOLID_NOT;
	self.movetype = MOVETYPE_NONE;
	psq_supply_precache ();
};

/*QUAKED psq_safe (0 .8 .3) ?
A zone no process monster is placed in (psq_in_safe): the arenas' start
room, which a lip keeps walking monsters from entering, must not have them
spawn inside it instead (the near-player fallback, while the player stands
there and every grid is full). Invisible and not solid.
*/
void() psq_safe =
{
	setmodel (self, self.model);	// absmin and absmax from the brush
	self.solid = SOLID_NOT;
	self.movetype = MOVETYPE_NONE;
	self.modelindex = 0;
	self.model = "";
};

/*QUAKED psq_exit_door (0 .5 .8) ?
An arena's exit door (tools/generate_arena.py): a stock func_door with
health, which opens when shot, so leaving the arena is deliberate. Only the
player's damage opens it: T_Damage sets damage_attacker before Killed runs
th_die, and anything else (a process monster's stray shot, a swarm Scrag's
bolt, a grenade a monster threw) leaves it shut and whole for the next hit.
*/
void() psq_exit_door_die =
{
	if (damage_attacker.classname != "player")
	{
		self.health = self.max_health;
		return;
	}
	door_killed ();
};

void() psq_exit_door =
{
	func_door ();
	self.th_die = psq_exit_door_die;
};

/*QUAKED psq_skill_arch (.5 .5 .5) ?
A start room arch (tools/generate_arena.py): trigger_setskill, setting the
skill to its "message" (0 to 3), that also tells the player what was chosen.
It touches every frame the player stands in it, so each choice is told once
to each player; walking through another arch tells that one. Each line says
only what the skill changes in these progs, and nothing false: no skill
changes what a hit does (T_Damage and every attack's damage take no skill);
Easy, Normal and Hard choose which of a campaign map's monsters appear when
it loads (the engine's NOT_EASY / NOT_MEDIUM / NOT_HARD spawnflags; the
arenas place none of their own, and process monsters come whatever the
skill); Nightmare places Hard's and also has monsters attack without pause
(SUB_AttackFinished, SUB_CheckRefire) and flinch at most every 5 s
(T_Damage's pain_finished), Shamblers cast a third bolt (shambler.qc) and
Vore balls fly faster (vore.qc). Not told: Chthon's own skill checks (a
campaign boss; chthon.qc).
*/
.float	psq_skill_said;		// 1 + the skill last told this player; 0 for none (the
				// engine clears every edict, clients too, each level)

string(float s) psq_skill_line =
{
	if (s == 0) return "Easy selected. Campaign maps have fewer monsters; a hit does the same damage at every skill.";
	if (s == 1) return "Normal selected. Campaign maps have their standard monsters; a hit does the same damage at every skill.";
	if (s == 2) return "Hard selected. Campaign maps have more monsters; a hit does the same damage at every skill.";
	return "Nightmare selected. Campaign maps have Hard's monsters; monsters attack without pause and rarely flinch, Shamblers cast an extra bolt and Vore balls fly faster.";
};

void() psq_skill_arch_touch =
{
	local float s;
	local string said;

	if (other.classname != "player")
		return;
	cvar_set ("skill", self.message);
	s = cvar ("skill");	// what the engine made of the message
	if (s == other.psq_skill_said - 1)
		return;
	other.psq_skill_said = s + 1;
	said = psq_skill_line (s);
	sprint (other, PRINT_HIGH, said);
	sprint (other, PRINT_HIGH, "\n");
	psq_log ("skill: ", said, "", "", "", "");
};

void() psq_skill_arch =
{
	if (deathmatch)
	{
		remove (self);
		return;
	}
	InitTrigger ();
	self.touch = psq_skill_arch_touch;
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
