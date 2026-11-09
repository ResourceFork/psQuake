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
             psq_cols x psq_rows == --cells and psq_cell == --cell-size; four
             trigger_setskill pads (skills 0-3); a trigger_changelevel to
             --exit-map with spawnflags 1 (no intermission), and no other
             changelevel; every brush model reference exists
  grid       every cell has floor under its whole 96 x 96 square (a 5 x 5
             sample), no brush anywhere in it from the floor to 56 units up
             (hull 1 at a 3 x 3 of points), and room at its centre for the
             largest walking monster (hull 2, 64 x 64 x 88)
  start      the player start is clear for the player hull, its box off the grid
  pickups    every item_* and weapon_* is in open space, its box off the grid

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


def validate(bsp_path, map_path, exit_map, cells, cell_size):
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
        ents = _check_entities(r, bsp, exit_map, cells, cell_size)
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


def _check_entities(r, bsp, exit_map, cells, cell_size):
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
    if len(arenas) != 1:
        problems.append(f"{len(arenas)} psq_arena, expected 1")
    else:
        a = arenas[0]
        try:
            cols = as_int(a.get("psq_cols"), "psq_arena psq_cols")
            rows = as_int(a.get("psq_rows"), "psq_arena psq_rows")
            cell = as_int(a.get("psq_cell"), "psq_arena psq_cell")
            vec3(a.get("origin", ""), "psq_arena origin")
            if cols <= 0 or rows <= 0 or cols * rows != cells:
                problems.append(f"psq_arena grid is {cols} x {rows}, expected {cells} cells")
            if cell != cell_size:
                problems.append(f"psq_arena cell is {cell}, expected {cell_size}")
        except BspError as e:
            problems.append(str(e))
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
            f"{len(ents)} entities; exit to {exit_map}; skills 0-3; one psq_arena")
    return None if problems else ents


def _check_placement(r, bsp, ents, cell_size):
    a = next(e for e in ents if e.get("classname") == "psq_arena")
    ox, oy, oz = vec3(a["origin"], "psq_arena origin")
    cols, rows = int(a["psq_cols"]), int(a["psq_rows"])
    half_x, half_y = cols * cell_size / 2, rows * cell_size / 2

    def box_on_grid(x0, y0, x1, y1):
        """True if the box [x0, x1] x [y0, y1] overlaps the grid's footprint."""
        return (x0 < ox + half_x and ox - half_x < x1
                and y0 < oy + half_y and oy - half_y < y1)

    # Per cell: floor under, and open space just above, a 5 x 5 sample of
    # the whole cell (1 unit inside its edges); hull 1 clear at a 3 x 3 of
    # stand points 32 apart, whose boxes together cover the full cell from
    # the floor to 56 units up; and hull 2 (the largest walking monster)
    # clear at the centre, to 88 units up.
    edge = cell_size / 2 - 1
    floor_offsets = [edge * k / 2 for k in (-2, -1, 0, 1, 2)]
    stand_offsets = [-(cell_size / 2 - 16), 0.0, cell_size / 2 - 16]
    bad = []
    for row in range(rows):
        for col in range(cols):
            x = ox + (col - (cols - 1) / 2) * cell_size
            y = oy + (row - (rows - 1) / 2) * cell_size
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
            if why:
                more = f" (+{len(why) - 3} more)" if len(why) > 3 else ""
                bad.append(f"cell ({col},{row}) at ({x:g} {y:g}): "
                           f"{', '.join(why[:3])}{more}")
    r.check("grid", not bad,
            "; ".join(bad) if bad else
            f"{cols * rows} cells floored and clear to their edges; hull 2 fits each centre")

    # The player's box is 32 x 32 around its origin.
    starts = [e for e in ents if e.get("classname") == "info_player_start"]
    sx, sy, sz = vec3(starts[0].get("origin", ""), "info_player_start origin")
    problems = []
    if bsp.contents(HULL_PLAYER, (sx, sy, sz)) != CONTENTS_EMPTY:
        problems.append("the player start is inside a brush")
    if box_on_grid(sx - 16, sy - 16, sx + 16, sy + 16):
        problems.append("the player start is on the grid")
    r.check("start", not problems, "; ".join(problems) if problems else
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


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--bsp", required=True)
    ap.add_argument("--map", help="the .map it was compiled from (leak files sit beside it)")
    ap.add_argument("--exit-map", required=True, help="the campaign map the exit must lead to")
    ap.add_argument("--cells", type=int, default=77)
    ap.add_argument("--cell-size", type=int, default=96)
    args = ap.parse_args(argv)
    r = validate(args.bsp, args.map, args.exit_map, args.cells, args.cell_size)
    if r.failed:
        print(f"CHECK FAIL ({', '.join(dict.fromkeys(r.failed))})")
        return 1
    print("CHECK PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
