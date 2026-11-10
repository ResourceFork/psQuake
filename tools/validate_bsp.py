#!/usr/bin/env python3
"""Validate the compiled psQuake arena (a Quake BSP version 29).

Checks, each reported as PASS or FAIL with its id:

  format     version 29; every lump inside the file and a whole number of records
  geometry   faces, models, visibility and lighting are all present
  textures   every texture a face uses is embedded in the BSP with its pixels
  leak       the map is sealed: no qbsp leak file (.pts/.lin) beside --map, and
             the space outside the map is solid in hulls 0, 1 and 2
  lighting   at least 95% of world faces carry a lightmap, and it is not dark
  entities   one worldspawn (first), one info_player_start, one psq_arena with
             psq_cols x psq_rows == --cells and psq_cell == --cell-size (with
             psq_grid entities as well: every grid psq_cell == --cell-size, at
             least --min-cells in all, and no two grids overlapping in plan); four
             trigger_setskill pads (skills 0-3); a trigger_changelevel to
             --exit-map with spawnflags 1 (no intermission), and no other
             changelevel; every brush model reference exists
  grid       every cell of every grid has floor under its whole 96 x 96 square (a 5 x 5
             sample), no brush anywhere in it from the floor to 56 units up
             (hull 1 at a 3 x 3 of points), and room at its centre for the
             largest walking monster (hull 2, 64 x 64 x 88)
  start      the player start is clear for the player hull, its box off the grids
  pickups    every item_* and weapon_* is in open space, its box off the grids
  reach      every cell centre can be walked to from the player start by the
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
    frac = lit / total if total else 0.0
    mean = level / samples if samples else 0.0
    ok = total > 0 and frac >= 0.95 and mean >= 24
    r.check("lighting", ok, f"{lit}/{total} faces lit ({frac:.0%}), mean level {mean:.0f}/255")


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
                if cols <= 0 or rows <= 0:
                    problems.append(f"{what} grid is {cols} x {rows}")
                elif not extra and cols * rows != cells:
                    problems.append(f"psq_arena grid is {cols} x {rows}, expected {cells} cells")
                if cell != cell_size:
                    problems.append(f"{what} cell is {cell}, expected {cell_size}")
                total += max(cols, 0) * max(rows, 0)
                rects.append((what, ox - cols * cell / 2, oy - rows * cell / 2,
                              ox + cols * cell / 2, oy + rows * cell / 2))
            except BspError as e:
                problems.append(str(e))
        if extra and total < min_cells:
            problems.append(f"{total} cells in {len(rects)} grids, fewer than {min_cells}")
        for i, (wa, ax0, ay0, ax1, ay1) in enumerate(rects):
            for wb, bx0, by0, bx1, by1 in rects[i + 1:]:
                if ax0 < bx1 and bx0 < ax1 and ay0 < by1 and by0 < ay1:
                    problems.append(f"grids overlap in plan: {wa} at ({ax0:g} {ay0:g}) "
                                    f"and {wb} at ({bx0:g} {by0:g})")
    skills = sorted(e.get("message", "") for e in of("trigger_setskill"))
    if skills != ["0", "1", "2", "3"]:
        problems.append(f"trigger_setskill skills {skills}, expected 0 1 2 3")
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
    for e in exits + of("trigger_setskill"):
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
        if box_on_grid(x - 16, y - 16, x + 32, y + 32):
            problems.append(f"{e['classname']} at ({x:g} {y:g}) is on the grid")
    r.check("pickups", not problems and bool(items),
            "; ".join(problems) if problems else
            f"{len(items)} pickups clear and off the grid" if items else "no pickups")

    if start_ok:   # reach is walked from the start; a bad start is reported once
        _check_reach(r, bsp, (sx, sy, sz), clear)   # cells the grid check passed


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


def _check_reach(r, bsp, start, centres):
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
            f"{len(centres)} cells walkable from the start in hull 2 ({levels}; "
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
