#!/usr/bin/env python3
"""Validate the compiled psQuake arena (a Quake BSP version 29).

Checks, each reported as PASS or FAIL with its id:

  format     version 29; every lump inside the file and a whole number of records
  geometry   faces, models, visibility and lighting are all present
  textures   every texture a face uses is embedded in the BSP with its pixels
  leak       the map is sealed: no qbsp leak file (.pts/.lin) beside --map, and
             the space outside the map is solid in hulls 0, 1 and 2
  lighting   every world face carries a lightmap (a face without one renders
             black), and the mean level is not dark (24 of 255 or more)
  entities   one worldspawn (first), one info_player_start, one psq_arena with
             psq_cols x psq_rows == --cells and psq_cell == --cell-size (with
             psq_grid entities as well: every grid psq_cell == --cell-size, at
             least --min-cells in all, at most 16 grids of at most 32 x 32, and no
             two grids overlapping in plan); four skill triggers,
             trigger_setskill or psq_skill_arch (skills 0-3); a trigger_changelevel to
             --exit-map with spawnflags 1 (no intermission), and no other
             changelevel; every brush model reference exists
  grid       every cell of every grid has floor under its whole 96 x 96 square (a 5 x 5
             sample), no brush anywhere in it from the floor to 56 units up
             (hull 1 at a 3 x 3 of points), and room at its centre for the
             largest walking monster (hull 2, 64 x 64 x 88)
  start      the player start is clear for the player hull, its box off the grids
  pickups    every item_* and weapon_* is in open space, its box off the grids
  supply     with psq_supply spots (resupply), at least 4, each where an item
             fits: its box (as pickups') clear, a floor under it within 32
             units, off the grids
  startroom  with "_psq_rules" naming start-room: a walking monster (hull 1,
             steps of 18) cannot walk from the hall entry (_psq_hall_entry) to
             the player start, nor a leaping one (hull 2, jumps of up to 40),
             and the player (jumps of up to 40) can walk from the start to the
             hall, only through a skill trigger, and through every one; the
             room's area (where the player walks from the start without a
             jump, the escape hallway and balcony included) is all in psq_safe
             zones, and no grid cell or psq_supply spot is in one. With
             "_psq_rules" naming balcony too: from that area the player can
             drop into the hall (over 100: out of melee reach from below; at
             most 216, so a jump off, ~46 more, stays under the ~264 that
             hurts; and no edge over a deeper drop), 512 or
             more from the hall entry, only past a skill trigger; no walking or
             leaping monster stands within melee reach (100) of its edge; and
             no fiend leap (up at most 48, across at most 400, a clear way for
             hull 2) lands on it
  firelight  with torches or flames (light_torch_small_walltorch,
             light_flame_large_yellow): the coloured lightmap beside the BSP
             (<map>.lit, QLIT version 1, three bytes for each lightmap byte)
             is warm on the face just in front of each one: its red less its
             blue, which the white lamps add to equally, so what the fire
             itself adds, is at least FIRE_WARM (64) somewhere within a
             luxel of the spot behind it
  exitdoor   with "_psq_rules" naming exit-door: a psq_exit_door with health
             that stays open (wait -1) spans the changelevel trigger, in front
             of it, from its floor up at least 128
  reach      every cell centre can be walked to from the player start (or the
             hall entry, when the map names one) by the
             largest walking hull (hull 2): a 32-unit lattice of stand points,
             joined where they differ by at most a step (18 units) with no wall
             between, so no cell is a pocket a monster cannot leave on foot
             (cells the grid check failed are not judged again; not run when
             the start check fails)

    validate_bsp.py --bsp build/arena/psqarena.bsp --map build/arena/psqarena.map \\
        --exit-map lq_e1m1 [--cells 77 --cell-size 96]

Exit status 0 when every check passes, 1 when any fails, 2 for a usage error.
The input is untrusted: the file is capped at 64 MiB, the entity lump at 4 MiB
and the texture count at 4096; every offset and count is bounds-checked, tree
walks are bounded, parsing stops at the first error, and a malformed file is
a FAIL, never a traceback.

Python 3 standard library only.
"""

from __future__ import annotations

import argparse
import math
import os
import re
import struct
import sys

BSP_VERSION = 29
MAX_BYTES = 64 * 1024 * 1024
LUMP_NAMES = ("entities", "planes", "textures", "vertexes", "visibility",
              "nodes", "texinfo", "faces", "lighting", "clipnodes", "leafs",
              "marksurfaces", "edges", "surfedges", "models")
RECORD = {"planes": 20, "vertexes": 12, "nodes": 24, "texinfo": 40,
          "faces": 20, "clipnodes": 8, "leafs": 28, "marksurfaces": 2,
          "edges": 4, "surfedges": 4, "models": 64}

CONTENTS_EMPTY = -1
CONTENTS_SOLID = -2
CONTENTS_NAMES = {-1: "empty", -2: "solid", -3: "water", -4: "slime",
                  -5: "lava", -6: "sky"}

# Quake's collision hulls: (mins, maxs) of the box each one stands for.
MAX_GRID_DIM = 32  # psq_grid.h PSQ_GRID_MAX_DIM: cells along either side
MAX_GRIDS = 16     # grids per map (the arena tests' helpers hold that many)
STEP = 18         # the most a walking monster climbs (or steps down) at once
HULL_PLAYER = 1   # (-16 -16 -24) (16 16 32)
HULL_LARGE = 2    # (-32 -32 -24) (32 32 64)
STAND_Z = 24      # a hull 1/2 origin stands 24 units above the floor

TEX_SPECIAL = 1   # texinfo flag: sky or liquid, never lit


class BspError(Exception):
    pass


class Bsp:
    def __init__(self, data: bytes):
        self.data = data
        if len(data) < 4 + 8 * len(LUMP_NAMES):
            raise BspError(f"file is {len(data)} bytes, shorter than a BSP header")
        (self.version,) = struct.unpack_from("<i", data, 0)
        if self.version != BSP_VERSION:
            raise BspError(f"version {self.version}, expected {BSP_VERSION}")
        self.lumps = {}
        for i, name in enumerate(LUMP_NAMES):
            ofs, ln = struct.unpack_from("<ii", data, 4 + 8 * i)
            if ofs < 0 or ln < 0 or ofs + ln > len(data):
                raise BspError(f"lump {name} ({ofs}, {ln}) lies outside the file")
            if name in RECORD and ln % RECORD[name]:
                raise BspError(f"lump {name} is {ln} bytes, not whole records")
            self.lumps[name] = (ofs, ln)
        self.planes = self._records("planes", "<4fi")
        self.nodes = self._records("nodes", "<i2h6h2H")
        self.clipnodes = self._records("clipnodes", "<i2h")
        self.leafs = self._records("leafs", "<ii6h2H4B")
        self.texinfo = self._records("texinfo", "<8fii")
        self.faces = self._records("faces", "<hhihh4Bi")
        self.models = self._records("models", "<9f4i3i")

    def lump(self, name) -> bytes:
        ofs, ln = self.lumps[name]
        return self.data[ofs:ofs + ln]

    def _records(self, name, fmt):
        size = struct.calcsize(fmt)
        assert size == RECORD[name], name
        return [r for r in struct.iter_unpack(fmt, self.lump(name))]

    # --- point contents --------------------------------------------------

    def _plane_dist(self, planenum, p):
        if not 0 <= planenum < len(self.planes):
            raise BspError(f"plane {planenum} out of range")
        nx, ny, nz, dist, _ = self.planes[planenum]
        return nx * p[0] + ny * p[1] + nz * p[2] - dist

    def contents(self, hull, p, model=0):
        """Contents of point p in the given hull of a model (as the engine
        decides it: SV_HullPointContents for 1-2, Mod_PointInLeaf for 0)."""
        if not 0 <= model < len(self.models):
            raise BspError(f"model {model} out of range")
        num = self.models[model][9 + hull]
        if hull == 0:
            for _ in range(len(self.nodes) + 1):
                if num < 0:
                    leaf = -1 - num
                    if not 0 <= leaf < len(self.leafs):
                        raise BspError(f"leaf {leaf} out of range")
                    return self.leafs[leaf][0]
                if num >= len(self.nodes):
                    raise BspError(f"node {num} out of range")
                node = self.nodes[num]
                num = node[1] if self._plane_dist(node[0], p) > 0 else node[2]
            raise BspError("node tree has a cycle")
        for _ in range(len(self.clipnodes) + 1):
            if num < 0:
                return num
            if num >= len(self.clipnodes):
                raise BspError(f"clipnode {num} out of range")
            node = self.clipnodes[num]
            num = node[2] if self._plane_dist(node[0], p) < 0 else node[1]
        raise BspError("clipnode tree has a cycle")

    # --- textures ---------------------------------------------------------

    def miptex(self):
        """[(name or None, embedded with pixels?, why not)] by miptex index."""
        lump = self.lump("textures")
        if not lump:
            return []
        if len(lump) < 4:
            raise BspError("texture lump is shorter than its count")
        (count,) = struct.unpack_from("<i", lump, 0)
        if count < 0 or count > MAX_MIPTEX or 4 + 4 * count > len(lump):
            raise BspError(f"texture count {count} does not fit its lump (at most {MAX_MIPTEX})")
        out = []
        for i in range(count):
            (mofs,) = struct.unpack_from("<i", lump, 4 + 4 * i)
            if mofs < 0:
                out.append((None, False, "no texture in this slot"))
                continue
            if mofs + 40 > len(lump):
                out.append((None, False, "header outside the lump"))
                continue
            name = lump[mofs:mofs + 16].split(b"\0", 1)[0].decode("latin-1")
            w, h, *offs = struct.unpack_from("<II4I", lump, mofs + 16)
            if not (0 < w <= 4096 and 0 < h <= 4096 and w % 16 == 0 and h % 16 == 0):
                out.append((name, False, f"bad size {w}x{h}"))
                continue
            if any(o == 0 for o in offs):
                out.append((name, False, "no pixel data (not found when compiled)"))
                continue
            ok = True
            for lv, o in enumerate(offs):
                if mofs + o + (w >> lv) * (h >> lv) > len(lump):
                    ok = False
            out.append((name, ok, "" if ok else "pixel data outside the lump"))
        return out


_ENTITY_TOKEN = re.compile(r'"([^"\n]*)"|([{}])|(\S)')
MAX_ENTITY_BYTES = 4 * 1024 * 1024
MAX_MIPTEX = 4096


def parse_entities(text: str):
    """Quake entity lump -> [dict]; raises BspError on malformed input."""
    ents = []
    cur = None
    key = None
    for m in _ENTITY_TOKEN.finditer(text):  # stops at the first bad token
        quoted, brace, junk = m.groups()
        if junk and junk != "\0":
            raise BspError(f"unexpected {junk!r} in the entity lump")
        if brace == "{":
            if cur is not None:
                raise BspError("nested '{' in the entity lump")
            cur, key = {}, None
        elif brace == "}":
            if cur is None or key is not None:
                raise BspError("unbalanced '}' in the entity lump")
            ents.append(cur)
            cur = None
        elif not junk:
            if cur is None:
                raise BspError("key outside an entity")
            if key is None:
                key = quoted
            else:
                cur[key] = quoted
                key = None
    if cur is not None:
        raise BspError("entity lump ends inside an entity")
    return ents


def vec3(s, what):
    parts = s.split()
    try:
        if len(parts) != 3:
            raise ValueError
        return tuple(float(v) for v in parts)
    except ValueError:
        raise BspError(f"{what} is not three numbers: {s!r}")


def as_int(s, what):
    try:
        return int(s)
    except (TypeError, ValueError):
        raise BspError(f"{what} is not an integer: {s!r}")


class Report:
    def __init__(self):
        self.failed = []

    def check(self, cid, ok, detail):
        print(f"{'PASS' if ok else 'FAIL'} {cid}: {detail}")
        if not ok:
            self.failed.append(cid)
        return ok


def validate(bsp_path, map_path, exit_map, cells, cell_size, min_cells=35):
    r = Report()
    try:
        size = os.path.getsize(bsp_path)
        if size > MAX_BYTES:
            raise BspError(f"{size} bytes, larger than the {MAX_BYTES} byte limit")
        with open(bsp_path, "rb") as f:
            data = f.read(MAX_BYTES + 1)
        bsp = Bsp(data)
    except (OSError, BspError) as e:
        r.check("format", False, str(e))
        return r
    r.check("format", True, f"BSP {bsp.version}, {len(data)} bytes")

    lens = {n: bsp.lumps[n][1] for n in LUMP_NAMES}
    empty = [n for n in ("faces", "models", "visibility", "lighting") if lens[n] == 0]
    r.check("geometry", not empty,
            "empty: " + ", ".join(empty) if empty else
            f"{len(bsp.faces)} faces, {len(bsp.models)} models, "
            f"visibility {lens['visibility']} B, lighting {lens['lighting']} B")

    try:
        _check_textures(r, bsp)
        _check_leak(r, bsp, map_path)
        _check_lighting(r, bsp)
        ents = _check_entities(r, bsp, exit_map, cells, cell_size, min_cells)
        if ents is not None:
            _check_placement(r, bsp, ents, cell_size)
            _check_firelight(r, bsp, ents, bsp_path)
    except BspError as e:
        r.check("format", False, str(e))
    return r


def _check_textures(r, bsp):
    mip = bsp.miptex()
    used = set()
    for face in bsp.faces:
        ti = face[4]
        if not 0 <= ti < len(bsp.texinfo):
            raise BspError(f"face uses texinfo {ti} of {len(bsp.texinfo)}")
        used.add(bsp.texinfo[ti][8])
    bad = []
    for m in sorted(used):
        if not 0 <= m < len(mip):
            bad.append(f"miptex {m} (of {len(mip)})")
            continue
        name, ok, why = mip[m]
        if not ok:
            bad.append(f"{name or f'slot {m}'}: {why}")
    names = sorted({mip[m][0] for m in used if 0 <= m < len(mip) and mip[m][0]})
    r.check("textures", not bad and bool(used),
            "missing: " + "; ".join(bad) if bad else
            f"{len(names)} embedded: {' '.join(names)}")


def _check_leak(r, bsp, map_path):
    problems = []
    if map_path:
        stem = os.path.splitext(map_path)[0]
        for ext in (".pts", ".lin"):
            if os.path.exists(stem + ext):
                problems.append(f"qbsp leak file {os.path.basename(stem + ext)}")
    m = bsp.models[0] if bsp.models else None
    if m is None:
        problems.append("no world model")
    else:
        mins, maxs = m[0:3], m[3:6]
        outside = (mins[0] - 256.5, mins[1] - 256.5, (mins[2] + maxs[2]) / 2 + 0.5)
        for hull in (0, HULL_PLAYER, HULL_LARGE):
            c = bsp.contents(hull, outside)
            if c != CONTENTS_SOLID:
                problems.append(f"outside the map is {CONTENTS_NAMES.get(c, c)} in hull {hull}")
    r.check("leak", not problems, "; ".join(problems) if problems else "sealed")


def _check_lighting(r, bsp):
    light = bsp.lump("lighting")
    lit = total = 0
    samples = 0
    level = 0
    face_means = []
    # World faces only: brush entities here are triggers, invisible in game
    # and never lit.
    first, count = (bsp.models[0][14], bsp.models[0][15]) if bsp.models else (0, 0)
    if first < 0 or count < 0 or first + count > len(bsp.faces):
        raise BspError(f"world model faces {first}+{count} of {len(bsp.faces)}")
    for face in bsp.faces[first:first + count]:
        ti = face[4]
        if bsp.texinfo[ti][9] & TEX_SPECIAL:
            continue
        total += 1
        ofs = face[9]
        if 0 <= ofs < len(light):
            lit += 1
            chunk = light[ofs:ofs + 64]
            samples += len(chunk)
            level += sum(chunk)
            if chunk:
                face_means.append(sum(chunk) / len(chunk))
    frac = lit / total if total else 0.0
    mean = level / samples if samples else 0.0
    # Every world face lit: a face light leaves with no lightmap renders black
    # (the dark arenas' corners must be dim, never black).
    ok = total > 0 and lit == total and mean >= 24
    face_means.sort()

    def pct(q):
        return face_means[min(len(face_means) - 1, int(q * len(face_means)))] if face_means else 0
    # The spread, for the record: the darkest tenth of the faces sit at or
    # under p10, the brightest tenth at or over p90.
    r.check("lighting", ok, f"{lit}/{total} faces lit ({frac:.0%}), mean level {mean:.0f}/255; "
            f"faces p10 {pct(0.1):.0f}, median {pct(0.5):.0f}, p90 {pct(0.9):.0f}")


def _check_entities(r, bsp, exit_map, cells, cell_size, min_cells=35):
    try:
        raw = bsp.lump("entities")
        if len(raw) > MAX_ENTITY_BYTES:
            raise BspError(f"entity lump is {len(raw)} bytes, over {MAX_ENTITY_BYTES}")
        text = raw.split(b"\0", 1)[0].decode("latin-1")
        ents = parse_entities(text)
    except BspError as e:
        r.check("entities", False, str(e))
        return None
    problems = []

    def of(cls):
        return [e for e in ents if e.get("classname") == cls]

    if not ents or ents[0].get("classname") != "worldspawn":
        problems.append("the first entity is not worldspawn")
    if len(of("worldspawn")) != 1:
        problems.append(f"{len(of('worldspawn'))} worldspawn entities")
    if len(of("info_player_start")) != 1:
        problems.append(f"{len(of('info_player_start'))} info_player_start, expected 1")
    arenas = of("psq_arena")
    extra = of("psq_grid")
    if len(arenas) != 1:
        problems.append(f"{len(arenas)} psq_arena, expected 1")
    else:
        rects = []
        total = 0
        for a in arenas + extra:
            what = a.get("classname")
            try:
                cols = as_int(a.get("psq_cols"), f"{what} psq_cols")
                rows = as_int(a.get("psq_rows"), f"{what} psq_rows")
                cell = as_int(a.get("psq_cell"), f"{what} psq_cell")
                ox, oy, _ = vec3(a.get("origin", ""), f"{what} origin")
                if not (1 <= cols <= MAX_GRID_DIM and 1 <= rows <= MAX_GRID_DIM):
                    problems.append(f"{what} grid is {cols} x {rows}: the engine takes 1 to "
                                    f"{MAX_GRID_DIM} cells along each side")
                elif not extra and cols * rows != cells:
                    problems.append(f"psq_arena grid is {cols} x {rows}, expected {cells} cells")
                if cell != cell_size:
                    problems.append(f"{what} cell is {cell}, expected {cell_size}")
                total += max(cols, 0) * max(rows, 0)
                rects.append((what, ox - cols * cell / 2, oy - rows * cell / 2,
                              ox + cols * cell / 2, oy + rows * cell / 2))
            except BspError as e:
                problems.append(str(e))
        if len(arenas + extra) > MAX_GRIDS:
            problems.append(f"{len(arenas + extra)} grids, more than {MAX_GRIDS}")
        if extra and total < min_cells:
            problems.append(f"{total} cells in {len(rects)} grids, fewer than {min_cells}")
        for i, (wa, ax0, ay0, ax1, ay1) in enumerate(rects):
            for wb, bx0, by0, bx1, by1 in rects[i + 1:]:
                if ax0 < bx1 and bx0 < ax1 and ay0 < by1 and by0 < ay1:
                    problems.append(f"grids overlap in plan: {wa} at ({ax0:g} {ay0:g}) "
                                    f"and {wb} at ({bx0:g} {by0:g})")
    skill_ents = [e for cls in SKILL_CLASSES for e in of(cls)]
    skills = sorted(e.get("message", "") for e in skill_ents)
    if skills != ["0", "1", "2", "3"]:
        problems.append(f"skill triggers ({' or '.join(SKILL_CLASSES)}) skills {skills}, "
                        "expected 0 1 2 3")
    exits = of("trigger_changelevel")
    good = [e for e in exits if e.get("map") == exit_map
            and as_int(e.get("spawnflags", "0"), "spawnflags") & 1]
    if not good:
        problems.append(f"no trigger_changelevel to {exit_map} with spawnflags 1")
    others = sorted({e.get("map", "") for e in exits} - {exit_map})
    if others:
        problems.append(f"trigger_changelevel to other maps: {others}")
    for e in ents:
        model = e.get("model")
        if model is not None:
            if not model.startswith("*"):
                problems.append(f"{e.get('classname')} uses external model {model!r}")
                continue
            idx = as_int(model[1:], f"{e.get('classname')} model")
            if not 1 <= idx < len(bsp.models):
                problems.append(f"{e.get('classname')} model {model} does not exist")
            elif bsp.models[idx][15] <= 0:
                problems.append(f"{e.get('classname')} model {model} has no faces")
    for e in exits + skill_ents:
        if "model" not in e:
            problems.append(f"{e.get('classname')} has no brush")
    r.check("entities", not problems,
            "; ".join(problems) if problems else
            f"{len(ents)} entities; exit to {exit_map}; skills 0-3; one psq_arena"
            + (f" and {len(extra)} psq_grid" if extra else ""))
    return None if problems else ents


def _grids(ents):
    """[(classname, ox, oy, oz, cols, rows)]: psq_arena first, then psq_grid."""
    out = []
    for cls in ("psq_arena", "psq_grid"):
        for e in ents:
            if e.get("classname") == cls:
                ox, oy, oz = vec3(e["origin"], f"{cls} origin")
                out.append((cls, ox, oy, oz, int(e["psq_cols"]), int(e["psq_rows"])))
    return out


def _cell_centres(grids, cell_size):
    """[(label, x, y, z)] for every cell of every grid."""
    out = []
    for gi, (_, ox, oy, oz, cols, rows) in enumerate(grids):
        for row in range(rows):
            for col in range(cols):
                x = ox + (col - (cols - 1) / 2) * cell_size
                y = oy + (row - (rows - 1) / 2) * cell_size
                label = f"cell ({col},{row})" if len(grids) == 1 else \
                    f"grid {gi} cell ({col},{row})"
                out.append((label, x, y, oz))
    return out


FIRES = ("light_torch_small_walltorch", "light_flame_large_yellow")
FIRE_WARM = 64     # red less blue the fire adds to the lightmap in front of it (of 255)


def _check_firelight(r, bsp, ents, bsp_path):
    """See 'firelight': each fire visibly warms the face in front of it."""
    fires = [vec3(e.get("origin", ""), f"{e['classname']} origin") for e in ents
             if e.get("classname") in FIRES]
    if not fires:
        return
    lit_path = os.path.splitext(bsp_path)[0] + ".lit"
    lofs, llen = bsp.lumps["lighting"]
    try:
        with open(lit_path, "rb") as f:
            lit = f.read(8 + 3 * llen + 1)
    except OSError:
        r.check("firelight", False, f"{len(fires)} fires, and no coloured lightmap {lit_path}")
        return
    if lit[:4] != b"QLIT" or struct.unpack_from("<i", lit, 4)[0] != 1 or len(lit) != 8 + 3 * llen:
        r.check("firelight", False, f"{lit_path} is not a QLIT version 1 matching the map")
        return
    data = bsp.data

    def records(lump, fmt):
        o, n = bsp.lumps[lump]
        size = struct.calcsize(fmt)
        return [struct.unpack_from(fmt, data, o + i * size) for i in range(n // size)]

    verts = records("vertexes", "<3f")
    edges = records("edges", "<2H")
    surf = [s[0] for s in records("surfedges", "<i")]
    faces = []
    for plane, side, first, num, tex, *_, lightofs in bsp.faces:
        if lightofs < 0 or not 0 <= plane < len(bsp.planes) or not 0 <= tex < len(bsp.texinfo):
            continue
        pts = []
        for k in range(num):
            e = surf[first + k] if 0 <= first + k < len(surf) else 0
            ei = e if e >= 0 else -e
            if ei >= len(edges) or max(edges[ei]) >= len(verts):
                break
            pts.append(verts[edges[ei][0]] if e >= 0 else verts[edges[ei][1]])
        if len(pts) < 3:
            continue
        nx, ny, nz, d, _ = bsp.planes[plane]
        if side:
            nx, ny, nz, d = -nx, -ny, -nz, -d
        vs = (bsp.texinfo[tex][0:4], bsp.texinfo[tex][4:8])
        mins, size = [], []
        for v in vs:
            vals = [p[0] * v[0] + p[1] * v[1] + p[2] * v[2] + v[3] for p in pts]
            lo_, hi_ = math.floor(min(vals) / 16), math.ceil(max(vals) / 16)
            mins.append(lo_)
            size.append(hi_ - lo_ + 1)
        lo = [min(p[i] for p in pts) for i in range(3)]
        hi = [max(p[i] for p in pts) for i in range(3)]
        faces.append(((nx, ny, nz, d), vs, mins, size, lightofs, lo, hi))
    problems, warm = [], []
    for t in fires:
        best = None
        for (nx, ny, nz, d), vs, mins, size, lofs_, lo, hi in faces:
            dist = t[0] * nx + t[1] * ny + t[2] * nz - d
            if not 0 < dist < 40:
                continue
            p = (t[0] - nx * dist, t[1] - ny * dist, t[2] - nz * dist)
            if any(p[i] < lo[i] - 1 or p[i] > hi[i] + 1 for i in range(3)):
                continue
            s = [p[0] * v[0] + p[1] * v[1] + p[2] * v[2] + v[3] for v in vs]
            ci, cj = int(s[0] // 16) - mins[0], int(s[1] // 16) - mins[1]
            for j in range(max(cj - 1, 0), min(cj + 2, size[1])):
                for i in range(max(ci - 1, 0), min(ci + 2, size[0])):
                    k = 3 * (lofs_ + j * size[0] + i)
                    if 8 + k + 2 < len(lit):
                        red, blue = lit[8 + k], lit[8 + k + 2]
                        if best is None or red - blue > best:
                            best = red - blue
        if best is None:
            problems.append(f"no lit face just in front of the fire at {_fmt(t)}")
        else:
            warm.append(best)
            if best < FIRE_WARM:
                problems.append(f"the fire at {_fmt(t)} adds red less blue {best} (at least "
                                f"{FIRE_WARM})")
    r.check("firelight", not problems, "; ".join(problems) if problems else
            f"{len(fires)} fires, each warming the face in front of it: red less blue "
            f"{min(warm)}-{max(warm)} (at least {FIRE_WARM})")


def _fmt(p):
    return "(" + " ".join(f"{v:g}" for v in p) + ")"


def _check_placement(r, bsp, ents, cell_size):
    grids = _grids(ents)

    def box_on_grid(x0, y0, x1, y1):
        """True if the box [x0, x1] x [y0, y1] overlaps any grid's footprint."""
        for _, ox, oy, _, cols, rows in grids:
            half_x, half_y = cols * cell_size / 2, rows * cell_size / 2
            if (x0 < ox + half_x and ox - half_x < x1
                    and y0 < oy + half_y and oy - half_y < y1):
                return True
        return False

    # Per cell: floor under, and open space just above, a 5 x 5 sample of
    # the whole cell (1 unit inside its edges); hull 1 clear at a 3 x 3 of
    # stand points 32 apart, whose boxes together cover the full cell from
    # the floor to 56 units up; and hull 2 (the largest walking monster)
    # clear at the centre, to 88 units up.
    edge = cell_size / 2 - 1
    floor_offsets = [edge * k / 2 for k in (-2, -1, 0, 1, 2)]
    stand_offsets = [-(cell_size / 2 - 16), 0.0, cell_size / 2 - 16]
    bad = []
    centres = _cell_centres(grids, cell_size)
    clear = []
    for label, x, y, oz in centres:
        why = []
        for dx in floor_offsets:
            for dy in floor_offsets:
                floor = bsp.contents(0, (x + dx, y + dy, oz - 1))
                above = bsp.contents(0, (x + dx, y + dy, oz + 1))
                if floor != CONTENTS_SOLID:
                    why.append(f"no floor at ({x + dx:g} {y + dy:g}) "
                               f"({CONTENTS_NAMES.get(floor, floor)} below)")
                if above != CONTENTS_EMPTY:
                    why.append(f"{CONTENTS_NAMES.get(above, above)} on the floor "
                               f"at ({x + dx:g} {y + dy:g})")
        for dx in stand_offsets:
            for dy in stand_offsets:
                if bsp.contents(HULL_PLAYER, (x + dx, y + dy, oz + STAND_Z + 1)) \
                        != CONTENTS_EMPTY:
                    why.append(f"a brush within the cell near ({x + dx:g} {y + dy:g})")
        if bsp.contents(HULL_LARGE, (x, y, oz + STAND_Z + 1)) != CONTENTS_EMPTY:
            why.append("a hull-2 monster at the centre would touch a brush")
        if not why:
            clear.append((label, x, y, oz))
        else:
            more = f" (+{len(why) - 3} more)" if len(why) > 3 else ""
            bad.append(f"{label} at ({x:g} {y:g}): "
                       f"{', '.join(why[:3])}{more}")
    levels = sorted({g[3] for g in grids})
    r.check("grid", not bad,
            "; ".join(bad) if bad else
            f"{len(centres)} cells floored and clear to their edges; hull 2 fits each centre"
            + (f" ({len(grids)} grids, floors at {' '.join(f'{z:g}' for z in levels)})"
               if len(grids) > 1 else ""))

    # The player's box is 32 x 32 around its origin.
    starts = [e for e in ents if e.get("classname") == "info_player_start"]
    sx, sy, sz = vec3(starts[0].get("origin", ""), "info_player_start origin")
    problems = []
    if bsp.contents(HULL_PLAYER, (sx, sy, sz)) != CONTENTS_EMPTY:
        problems.append("the player start is inside a brush")
    if box_on_grid(sx - 16, sy - 16, sx + 16, sy + 16):
        problems.append("the player start is on the grid")
    start_ok = r.check("start", not problems, "; ".join(problems) if problems else
                       f"clear at ({sx:g} {sy:g} {sz:g}), off the grid")

    # Quake's pickup boxes are (0 0 0)-(32 32 56) or (-16 -16 0)-(16 16 56)
    # from the origin; judge their union.
    items = [e for e in ents if e.get("classname", "").startswith(("item_", "weapon_"))]
    problems = []
    for e in items:
        x, y, z = vec3(e.get("origin", ""), f"{e['classname']} origin")
        if bsp.contents(0, (x, y, z)) != CONTENTS_EMPTY:
            problems.append(f"{e['classname']} at ({x:g} {y:g} {z:g}) is inside a brush")
        else:
            centred = e["classname"].startswith(("weapon_", "item_armor"))
            why = _item_drop_problem(bsp, x, y, z, centred)
            if why:
                problems.append(f"{e['classname']} at ({x:g} {y:g} {z:g}): {why}")
        if box_on_grid(x - 16, y - 16, x + 32, y + 32):
            problems.append(f"{e['classname']} at ({x:g} {y:g}) is on the grid")
    r.check("pickups", not problems and bool(items),
            "; ".join(problems) if problems else
            f"{len(items)} pickups clear and off the grid" if items else "no pickups")

    # Resupply spots: an item arrives at each (psq.qc psq_resupply), so its
    # box must be clear, with a floor close under it, and off the grids.
    spots = [e for e in ents if e.get("classname") == "psq_supply"]
    if spots:
        problems = []
        for e in spots:
            x, y, z = vec3(e.get("origin", ""), "psq_supply origin")
            # Health and ammo, and weapons, arrive here: both boxes must land.
            why = _item_drop_problem(bsp, x, y, z) or _item_drop_problem(bsp, x, y, z, True)
            if why:
                problems.append(f"psq_supply at ({x:g} {y:g} {z:g}): {why}")
            if box_on_grid(x - 16, y - 16, x + 32, y + 32):
                problems.append(f"psq_supply at ({x:g} {y:g}) is on the grid")
        if len(spots) < 4:
            problems.append(f"{len(spots)} psq_supply spots, fewer than 4")
        r.check("supply", not problems, "; ".join(problems) if problems else
                f"{len(spots)} resupply spots clear, floored and off the grids")

    rules = ents[0].get("_psq_rules", "").split() if ents else []
    entry = (sx, sy, sz)
    if "_psq_hall_entry" in ents[0]:
        entry = vec3(ents[0]["_psq_hall_entry"], "_psq_hall_entry")
    if start_ok:   # reach is walked from the start; a bad start is reported once
        # From the hall's entry when the map names one (behind a start room
        # monsters cannot walk into, the player start is not walkable).
        _check_reach(r, bsp, entry, clear,   # cells the grid check passed
                     start_is_entry=entry != (sx, sy, sz))
        if "start-room" in rules:
            _check_start_room(r, bsp, ents, (sx, sy, sz), entry, rules, cell_size)
    if "exit-door" in rules:
        _check_exit_door(r, bsp, ents, entry)


def _item_drop_problem(bsp, x, y, z, centred=False):
    """Why an item at (x, y, z) would not land, or "". The engine clips a
    pickup's box in hull 1 at the box's centre: health and ammo boxes span
    (0 0 0)-(32 32 56) from the origin (centre + 16 16 24), weapons and armour
    (-16 -16 0)-(16 16 56) (centred: + 0 0 24). LibreQuake's PlaceItem lifts
    the item 6 and drops it to the floor; it is removed ("fell out of level")
    if it starts in a brush."""
    off = 0 if centred else 16
    c = (x + off, y + off, z + 6 + 24)
    if bsp.contents(HULL_PLAYER, c) != CONTENTS_EMPTY:
        return "its box meets a brush (the item would fall out of the level)"
    if all(bsp.contents(HULL_PLAYER, (c[0], c[1], c[2] - 6 - d)) != CONTENTS_SOLID
           for d in range(1, 34)):
        return "no floor within 32 under it"
    return ""


EXIT_DOOR_REACH = 320   # the farthest a door may stand in front of the exit trigger
JUMP = 40          # the most the player climbs at once, jumping (a jump clears ~45)
SKILL_CLASSES = ("trigger_setskill", "psq_skill_arch")   # psq_skill_arch also says it
LEAP_UP = 48       # a fiend's leap (velocity 250 up) rises ~39: anything this much over
                   # where one stands is out of its reach
LEAP_FAR = 400     # ... and it carries ~375 forward (600 a second), level to level
FALL_SAFE = 216    # a fall from over ~264 hurts (client.qc: jump_flag under -650); a
                   # jump off adds ~46 to it
MELEE = 100        # the longest melee reach, origin to origin (fiend, ogre, dog, shambler)


def _lattice_nodes(bsp, hull):
    """{(x, y): [stand origin heights]} on the reach lattice, in hull."""
    m = bsp.models[0]
    mins, maxs = m[0:3], m[3:6]
    sp, half = REACH_SPACING, REACH_SPACING // 2

    def lattice(lo, hi):
        k = int((lo - half) // sp) + 1
        out = []
        while k * sp + half < hi:
            out.append(k * sp + half)
            k += 1
        return out

    zmin, zmax = int(mins[2]) - 1, int(maxs[2]) + 1
    nodes = {}
    for x in lattice(mins[0], maxs[0]):
        for y in lattice(mins[1], maxs[1]):
            hs = _stand_heights(bsp, hull, x, y, zmin, zmax)
            if hs:
                nodes[(x, y)] = hs
    return nodes


def _nearest_node(nodes, p):
    best = None
    for (nx, ny), hs in nodes.items():
        if abs(nx - p[0]) > REACH_SPACING or abs(ny - p[1]) > REACH_SPACING:
            continue
        for h in hs:
            d = abs(nx - p[0]) + abs(ny - p[1]) + abs(h - p[2])
            if best is None or d < best[0]:
                best = (d, (nx, ny, h))
    return best[1] if best else None


def _flood(bsp, hull, nodes, first, step, stairs=False, blocked=()):
    """Stand points reached from first, joining lattice neighbours that differ
    by at most step with no wall between. With stairs, also neighbours up to
    two steps apart with a stand point half way within a step of both (two
    risers inside 32 units). Points in blocked are never entered."""
    sp = REACH_SPACING
    seen = {first}
    todo = [first]
    while todo:
        x, y, z = todo.pop()
        for dx, dy in ((sp, 0), (-sp, 0), (0, sp), (0, -sp)):
            for h in nodes.get((x + dx, y + dy), ()):
                if (x + dx, y + dy, h) in seen or (x + dx, y + dy, h) in blocked:
                    continue
                mx, my = x + dx / 2, y + dy / 2
                if abs(h - z) > step:
                    if not stairs or abs(h - z) > 2 * step or not any(
                            abs(m - h) <= step and abs(m - z) <= step for m in
                            _stand_heights(bsp, hull, mx, my, min(h, z) - 1, max(h, z) + 1)):
                        continue
                elif bsp.contents(hull, (mx, my, max(h, z) + 1)) != CONTENTS_EMPTY:
                    continue
                seen.add((x + dx, y + dy, h))
                todo.append((x + dx, y + dy, h))
    return seen


def _model_bounds(bsp, e):
    model = e.get("model", "")
    idx = as_int(model[1:], f"{e.get('classname')} model") if model.startswith("*") else -1
    if not 1 <= idx < len(bsp.models):
        raise BspError(f"{e.get('classname')} has no brush model")
    return bsp.models[idx][0:3], bsp.models[idx][3:6]


def _check_start_room(r, bsp, ents, start, entry, rules=(), cell_size=96):
    """See 'startroom': the start room is the player's, not the monsters'."""
    nodes = _lattice_nodes(bsp, HULL_PLAYER)
    s, e = _nearest_node(nodes, start), _nearest_node(nodes, entry)
    if not s or not e:
        r.check("startroom", False, f"no stand point at the start {start} or the hall entry {entry}")
        return
    problems = []
    # A walking monster: steps of 18, up stairs too.
    walk = _flood(bsp, HULL_PLAYER, nodes, e, STEP, stairs=True)
    if s in walk:
        problems.append("a walking monster can walk from the hall into the start room")
    # A large monster that leaps (a fiend: hull 2, a jump of ~39) must not fit
    # over the lip either.
    nodes2 = _lattice_nodes(bsp, HULL_LARGE)
    s2, e2 = _nearest_node(nodes2, start), _nearest_node(nodes2, entry)
    leap = _flood(bsp, HULL_LARGE, nodes2, e2, JUMP, stairs=True) if e2 else set()
    if s2 and e2 and s2 in leap:
        problems.append("a large monster that leaps can get into the start room")
    player = _flood(bsp, HULL_PLAYER, nodes, s, JUMP)
    if e not in player:
        problems.append("the player cannot get from the start into the hall")
    skills = [x for x in ents if x.get("classname") in SKILL_CLASSES]
    inside = set()
    for t in skills:
        lo, hi = _model_bounds(bsp, t)
        # (a model's stored bounds sit 1 inside its brushes)
        through = {p for p in player if lo[0] - 2 <= p[0] <= hi[0] + 2
                   and lo[1] - 2 <= p[1] <= hi[1] + 2 and lo[2] - 2 <= p[2] <= lo[2] + STAND_Z + 2}
        if not through:
            problems.append(f"skill {t.get('message')}: the player cannot walk through it")
        inside |= through
    # Every way out of the room crosses a skill trigger (the balcony's drop
    # too: _balcony).
    noskill = _flood(bsp, HULL_PLAYER, nodes, s, JUMP, blocked=inside) if skills else None
    if skills and e in noskill:
        problems.append("the player can reach the hall without crossing a skill trigger")
    # No monster is placed in the room (psq.qc psq_in_safe).
    safe = [x for x in ents if x.get("classname") == "psq_safe"]
    if not any(_model_bounds(bsp, z)[0][0] - 1 <= start[0] <= _model_bounds(bsp, z)[1][0] + 1
               and _model_bounds(bsp, z)[0][1] - 1 <= start[1] <= _model_bounds(bsp, z)[1][1] + 1
               for z in safe):
        problems.append("no psq_safe zone round the player start")
    # The start room's protected area: wherever the player walks from the
    # start without jumping (the lip takes a jump), its escape hallway and
    # balcony included. All of it is in psq_safe zones, and no grid cell or
    # resupply spot is.
    boxes = [_model_bounds(bsp, z) for z in safe]

    def in_safe(p):
        return any(lo[0] - 2 <= p[0] <= hi[0] + 2 and lo[1] - 2 <= p[1] <= hi[1] + 2
                   and lo[2] - 2 <= p[2] <= hi[2] + 2 for lo, hi in boxes)

    area = _flood(bsp, HULL_PLAYER, nodes, s, STEP, stairs=True)
    out = sorted(p for p in area if not in_safe(p))
    if out:
        problems.append(f"{len(out)} stand points of the start room's area are in no psq_safe "
                        f"zone, the first at {out[0]}")
    for label, x, y, z in _cell_centres(_grids(ents), cell_size):
        if in_safe((x, y, z + STAND_Z)):
            problems.append(f"{label} at ({x:g} {y:g} {z:g}) is in a psq_safe zone")
    for ent in ents:
        if ent.get("classname") == "psq_supply":
            p = vec3(ent.get("origin", ""), "psq_supply origin")
            if in_safe(p):
                problems.append(f"psq_supply at ({p[0]:g} {p[1]:g} {p[2]:g}) is in a psq_safe zone")
    detail = ""
    if "balcony" in rules:
        why, detail = _balcony(bsp, nodes, area, player, walk, leap, entry, noskill)
        problems += why
    r.check("startroom", not problems, "; ".join(problems) if problems else
            f"no monster walks or leaps into the start room, and none is placed there; the "
            f"player walks out to the hall only through one of {len(skills)} skill triggers; "
            f"{len(area)} stand points protected, all in psq_safe zones, no cell or spot "
            f"there" + detail)


def _balcony(bsp, nodes, area, player, walk, leap, entry, noskill=None):
    """See 'startroom' with balcony: ([problems], detail). area: the start
    room's protected stand points; player: where the player goes from the
    start (jumps of JUMP); walk and leap: where walking monsters (hull 1,
    STEP) and leaping ones (hull 2, JUMP) go from the hall entry; noskill:
    where the player goes without crossing a skill trigger."""
    sp = REACH_SPACING
    drops, deep, exposed, raised = [], [], set(), set()
    for p in area:
        x, y, z = p
        for dx, dy in ((sp, 0), (-sp, 0), (0, sp), (0, -sp)):
            below = []
            for h in nodes.get((x + dx, y + dy), ()):
                q = (x + dx, y + dy, h)
                if q in area or q not in player:
                    continue
                exposed.add(p)
                if z - h > JUMP:
                    raised.add(p)   # an edge over the hall
                    below.append(h)
            if not below or bsp.contents(
                    HULL_PLAYER, (x + dx / 2, y + dy / 2, z + 1)) != CONTENTS_EMPTY:
                continue
            land = max(below)   # the first floor a drop there meets
            q = (x + dx, y + dy, land)
            if z - land > FALL_SAFE:
                deep.append((p, q))
            elif z - land > MELEE:
                drops.append((p, q))
    problems = []
    if deep:
        p, q = deep[0]
        problems.append(f"balcony: a drop of {p[2] - q[2]:g} from {p} hurts (at most {FALL_SAFE})")
    if not drops:
        problems.append("balcony: the player cannot drop from the start room's area into the "
                        f"hall (from over {MELEE}, out of melee reach, to at most {FALL_SAFE} "
                        "down)")
        return problems, ""
    far = min(math.hypot(p[0] - entry[0], p[1] - entry[1]) for p, _ in drops)
    if far < 512:
        problems.append(f"balcony: a jump-off point {far:.0f} from the hall entry (512 at least)")
    if noskill is not None and any(p in noskill for p, _ in drops):
        problems.append("balcony: the player can reach its drop without crossing a skill trigger")
    # Every edge over the hall is out of melee reach of every monster that
    # walks or leaps there.
    for p in sorted(raised):
        m = next((m for m in walk | leap if math.dist(p, m) <= MELEE), None)
        if m:
            problems.append(f"balcony: a monster at {m} reaches {p} ({math.dist(p, m):.0f}, "
                            f"melee {MELEE})")
            break
    # A fiend's leap: from a stand point it reaches on foot or leaping, to
    # where it lands on the area (it passes over an exposed point first), no
    # higher than LEAP_UP over its start and no farther than LEAP_FAR, the
    # way clear for hull 2 up at its start and across at the landing's height.
    for p in sorted(exposed):
        for q in leap:
            if p[2] - q[2] > LEAP_UP or math.hypot(p[0] - q[0], p[1] - q[1]) > LEAP_FAR:
                continue
            z = max(p[2], q[2]) + 1
            n = max(1, int(math.hypot(p[0] - q[0], p[1] - q[1]) // 8))
            rise = [(q[0], q[1], q[2] + 1 + (z - q[2] - 1) * k / 4) for k in range(5)]
            across = [(q[0] + (p[0] - q[0]) * k / n, q[1] + (p[1] - q[1]) * k / n, z)
                      for k in range(n + 1)]
            path = rise + across
            if all(bsp.contents(HULL_LARGE, pt) == CONTENTS_EMPTY for pt in path):
                problems.append(f"balcony: a fiend could leap from {q} to {p}")
                return problems, ""
    lo = min(p[2] - q[2] for p, q in drops)
    hi = max(p[2] - q[2] for p, q in drops)
    return problems, (f"; a balcony: {len(drops)} drops of {lo:g}-{hi:g} into the hall, "
                      f"{far:.0f} from its entry, out of every walk, climb, leap and melee reach")


def _check_exit_door(r, bsp, ents, entry):
    """See 'exitdoor': the teleporter is behind a door the player shoots open."""
    doors = [e for e in ents if e.get("classname") == "psq_exit_door"]
    exits = [e for e in ents if e.get("classname") == "trigger_changelevel"]
    problems = []
    if not doors:
        problems.append("no psq_exit_door")
    for t in exits:
        tlo, thi = _model_bounds(bsp, t)
        ok = False
        for d in doors:
            if as_int(d.get("health", "0"), "psq_exit_door health") <= 0 \
                    or d.get("wait") != "-1":
                problems.append("the psq_exit_door does not open when shot and stay open")
                continue
            dlo, dhi = _model_bounds(bsp, d)
            # Across the whole alcove (the trigger sits 16 in from its walls),
            # between the hall and the trigger, floor to at least 128 up.
            ok = ok or (dlo[0] <= tlo[0] - 15 and dhi[0] >= thi[0] + 15
                        and entry[1] < dlo[1] and dhi[1] <= tlo[1]
                        and tlo[1] - dhi[1] <= EXIT_DOOR_REACH
                        and dlo[2] <= tlo[2] and dhi[2] >= tlo[2] + 128)
        if not ok:
            problems.append(f"no exit door across the way to the exit at "
                            f"({(tlo[0] + thi[0]) / 2:g} {(tlo[1] + thi[1]) / 2:g})")
    r.check("exitdoor", not problems, "; ".join(dict.fromkeys(problems)) if problems else
            f"the exit is behind a door the player shoots open")


REACH_SPACING = 32
REACH_SCAN = 8


def _stand_heights(bsp, hull, x, y, zmin, zmax):
    """Origin heights in [zmin, zmax] at which the hull stands on a floor at
    (x, y): empty there, solid one unit lower. Top down, each to 1 unit."""
    out = []
    z = zmax
    above = bsp.contents(hull, (x, y, z))
    while z > zmin:
        zb = z - REACH_SCAN
        c = bsp.contents(hull, (x, y, zb))
        if above == CONTENTS_EMPTY and c == CONTENTS_SOLID:
            lo, hi = zb, z           # solid at lo, empty at hi
            while hi - lo > 1:
                mid = (lo + hi) // 2
                if bsp.contents(hull, (x, y, mid)) == CONTENTS_SOLID:
                    lo = mid
                else:
                    hi = mid
            out.append(hi)
        above = c
        z = zb
    return out


def _check_reach(r, bsp, start, centres, start_is_entry=False):
    """Flood the walkable lattice from the start in hull 2 (see 'reach')."""
    m = bsp.models[0]
    mins, maxs = m[0:3], m[3:6]
    sp = REACH_SPACING
    half = sp // 2

    def lattice(lo, hi):
        k = int((lo - half) // sp) + 1
        out = []
        while k * sp + half < hi:
            out.append(k * sp + half)
            k += 1
        return out

    xs, ys = lattice(mins[0], maxs[0]), lattice(mins[1], maxs[1])
    zmin, zmax = int(mins[2]) - 1, int(maxs[2]) + 1
    nodes = {}
    for x in xs:
        for y in ys:
            hs = _stand_heights(bsp, HULL_LARGE, x, y, zmin, zmax)
            if hs:
                nodes[(x, y)] = hs
    if not nodes:
        r.check("reach", False, "no stand points in hull 2")
        return

    def near(x, y, z):
        best = None
        for (nx, ny), hs in nodes.items():
            if abs(nx - x) > sp or abs(ny - y) > sp:
                continue
            for h in hs:
                d = abs(nx - x) + abs(ny - y) + abs(h - z)
                if best is None or d < best[0]:
                    best = (d, (nx, ny, h))
        return best[1] if best else None

    first = near(start[0], start[1], start[2])
    if first is None:
        r.check("reach", False, f"no hull-2 stand point near the start {start}")
        return
    seen = {first}
    todo = [first]
    while todo:
        x, y, z = todo.pop()
        for dx, dy in ((sp, 0), (-sp, 0), (0, sp), (0, -sp)):
            for h in nodes.get((x + dx, y + dy), ()):
                if abs(h - z) > STEP or (x + dx, y + dy, h) in seen:
                    continue
                mid = (x + dx / 2, y + dy / 2, max(h, z) + 1)
                if bsp.contents(HULL_LARGE, mid) != CONTENTS_EMPTY:
                    continue
                seen.add((x + dx, y + dy, h))
                todo.append((x + dx, y + dy, h))
    bad = []
    by_level = {}
    for label, x, y, oz in centres:
        # The cell centre joins the lattice points round it (inside its
        # floor) by the same rule as the lattice's own links.
        want = oz + STAND_Z
        ok = False
        kx, ky = int((x - half) // sp), int((y - half) // sp)
        for nx in (kx * sp + half, (kx + 1) * sp + half):
            for ny in (ky * sp + half, (ky + 1) * sp + half):
                for h in nodes.get((nx, ny), ()):
                    if abs(h - want) <= STEP and (nx, ny, h) in seen and bsp.contents(
                            HULL_LARGE, ((x + nx) / 2, (y + ny) / 2, max(h, want) + 1)) \
                            == CONTENTS_EMPTY:
                        ok = True
        n_ok, n_all = by_level.get(oz, (0, 0))
        by_level[oz] = (n_ok + ok, n_all + 1)
        if not ok:
            bad.append(f"{label} at ({x:g} {y:g} {oz:g})")
    more = f" (+{len(bad) - 6} more)" if len(bad) > 6 else ""
    levels = ", ".join(f"z {z:g}: {a}/{b}" for z, (a, b) in sorted(by_level.items()))
    r.check("reach", not bad,
            f"unreachable on foot from the start: {'; '.join(bad[:6])}{more} [{levels}]"
            if bad else
            f"{len(centres)} cells walkable from the {'hall entry' if start_is_entry else 'start'} in hull 2 ({levels}; "
            f"{len(seen)} of {sum(len(h) for h in nodes.values())} stand points reached)")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--bsp", required=True)
    ap.add_argument("--map", help="the .map it was compiled from (leak files sit beside it)")
    ap.add_argument("--exit-map", required=True, help="the campaign map the exit must lead to")
    ap.add_argument("--cells", type=int, default=77,
                    help="the psq_arena grid's cells, on a map with one grid (the test hall)")
    ap.add_argument("--cell-size", type=int, default=96)
    ap.add_argument("--min-cells", type=int, default=35,
                    help="with psq_grid entities: the fewest cells in all (psq_cap's top)")
    args = ap.parse_args(argv)
    r = validate(args.bsp, args.map, args.exit_map, args.cells, args.cell_size, args.min_cells)
    if r.failed:
        print(f"CHECK FAIL ({', '.join(dict.fromkeys(r.failed))})")
        return 1
    print("CHECK PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
