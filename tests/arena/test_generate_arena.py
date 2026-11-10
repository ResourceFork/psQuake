#!/usr/bin/env python3
"""Tests for tools/generate_arena.py (no map compiler needed).

    test_generate_arena.py <tools dir>

The requirement: a deterministic arena map with outward-facing brush planes,
an 11 x 7 grid of 96-unit cells clear of every brush, a player start and
pickups off the grid, four skill pads the way to the hall must cross, one
exit to the first campaign map, and a psq_arena entity describing the grid;
plus the five deliberately broken variants, the prototype layouts (several
spawn grids on three levels, ramps and stairs), and input checks on the pak
and BSP readers.
"""

import io
import os
import re
import struct
import sys
import unittest

sys.path.insert(0, os.path.abspath(sys.argv.pop(1)))
import generate_arena as ga  # noqa: E402


def parse_map(text):
    """[(keys, [brush faces])], each face (p0, p1, p2, tex)."""
    ents = []
    pos = 0
    face_re = re.compile(
        r"\(\s*(-?\d+) (-?\d+) (-?\d+) \)\s*\(\s*(-?\d+) (-?\d+) (-?\d+) \)\s*"
        r"\(\s*(-?\d+) (-?\d+) (-?\d+) \)\s*(\S+) 0 0 0 1 1")
    lines = [l for l in text.splitlines() if l and not l.startswith("//")]
    depth = 0
    cur = brush = None
    for line in lines:
        if line == "{":
            depth += 1
            if depth == 1:
                cur = ({}, [])
            elif depth == 2:
                brush = []
            else:
                raise AssertionError("brace depth > 2")
        elif line == "}":
            if depth == 2:
                cur[1].append(brush)
            elif depth == 1:
                ents.append(cur)
            depth -= 1
        elif depth == 1:
            m = re.fullmatch(r'"([^"]*)" "([^"]*)"', line)
            assert m, line
            cur[0][m.group(1)] = m.group(2)
        elif depth == 2:
            m = face_re.fullmatch(line)
            assert m, line
            v = [int(x) for x in m.groups()[:9]]
            brush.append((tuple(v[0:3]), tuple(v[3:6]), tuple(v[6:9]), m.group(10)))
        pos += 1
    assert depth == 0
    return ents


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def bounds(brush):
    pts = [p for f in brush for p in f[:3]]
    # Every face's p1 lies on the face; the box is spanned by them.
    p1s = [f[1] for f in brush]
    return (tuple(min(p[i] for p in p1s) for i in range(3)),
            tuple(max(p[i] for p in p1s) for i in range(3)))


def overlaps(box, lo, hi):
    bmin, bmax = box
    return all(bmin[i] < hi[i] and lo[i] < bmax[i] for i in range(3))


def arena(kind=None):
    world, ents = ga.build(kind)
    return parse_map(ga.map_text(world, ents, "psqarena.wad"))


class GeneratedMap(unittest.TestCase):
    def setUp(self):
        self.ents = arena()
        self.world = self.ents[0]
        self.by_class = {}
        for keys, brushes in self.ents:
            self.by_class.setdefault(keys["classname"], []).append((keys, brushes))

    def test_deterministic(self):
        a = ga.map_text(*ga.build(), "psqarena.wad")
        b = ga.map_text(*ga.build(), "psqarena.wad")
        self.assertEqual(a, b)

    def test_planes_face_outward(self):
        for keys, brushes in self.ents:
            for brush in brushes:
                self.assertEqual(len(brush), 6)
                lo, hi = bounds(brush)
                centre = tuple((a + b) / 2 for a, b in zip(lo, hi))
                for p0, p1, p2, _ in brush:
                    n = cross(sub(p0, p1), sub(p2, p1))
                    self.assertNotEqual(n, (0, 0, 0))
                    self.assertLess(dot(n, sub(centre, p1)), 0,
                                    f"{keys['classname']} face {p0} {p1} {p2} faces inward")

    def test_textures_are_known(self):
        used = {f[3] for _, brushes in self.ents for b in brushes for f in b}
        self.assertTrue(used <= set(ga.TEXTURES), used - set(ga.TEXTURES))
        self.assertNotIn(ga.TEX_MISSING, used)

    def test_worldspawn(self):
        keys, brushes = self.world
        self.assertEqual(keys["classname"], "worldspawn")
        self.assertEqual(keys["wad"], "psqarena.wad")
        self.assertGreater(len(brushes), 20)
        self.assertEqual(len(self.by_class["worldspawn"]), 1)

    def test_psq_arena_entity(self):
        (keys, brushes), = self.by_class["psq_arena"]
        self.assertEqual(brushes, [])
        self.assertEqual(int(keys["psq_cols"]) * int(keys["psq_rows"]), 77)
        self.assertEqual(int(keys["psq_cell"]), 96)
        self.assertEqual(keys["origin"], "0 0 0")

    def test_grid_cells_clear_of_brushes(self):
        world_boxes = [bounds(b) for b in self.world[1]]
        for row in range(ga.GRID_ROWS):
            for col in range(ga.GRID_COLS):
                x, y = ga.cell_centre(col, row)
                # The whole cell, floor to 88 units up (hull 2's height).
                h = ga.GRID_CELL / 2
                lo, hi = (x - h, y - h, 0), (x + h, y + h, 88)
                hits = [b for b in world_boxes if overlaps(b, lo, hi)]
                self.assertEqual(hits, [], f"cell ({col},{row})")
                floor = [b for b in world_boxes
                         if b[0][0] <= x <= b[1][0] and b[0][1] <= y <= b[1][1]
                         and b[1][2] == 0]
                self.assertTrue(floor, f"no floor under cell ({col},{row})")

    def test_start_and_pickups_off_grid(self):
        def off_grid(x, y):
            return abs(x) >= ga.GRID_HALF_X or abs(y) >= ga.GRID_HALF_Y
        (start, _), = self.by_class["info_player_start"]
        x, y, _ = (float(v) for v in start["origin"].split())
        self.assertTrue(off_grid(x, y))
        items = [k for k, _ in self.ents
                 if k["classname"].startswith(("item_", "weapon_"))]
        self.assertGreaterEqual(len(items), 4)
        for k in items:
            x, y, _ = (float(v) for v in k["origin"].split())
            self.assertTrue(off_grid(x, y), k)

    def test_skill_pads_block_the_way(self):
        pads = self.by_class["trigger_setskill"]
        self.assertEqual(sorted(k["message"] for k, _ in pads), ["0", "1", "2", "3"])
        spans = sorted((bounds(b[0])[0][0], bounds(b[0])[1][0]) for _, b in pads)
        x0, _, _, x1, _, _ = ga.ANTE
        edges = [x0] + [v for s in spans for v in s] + [x1]
        gaps = [edges[i + 1] - edges[i] for i in range(0, len(edges), 2)]
        # A player is 32 units wide: it cannot pass a gap narrower than that.
        self.assertTrue(all(g < 32 for g in gaps), gaps)

    def test_one_exit_to_campaign(self):
        (keys, brushes), = self.by_class["trigger_changelevel"]
        self.assertEqual(keys["map"], "lq_e1m1")
        self.assertEqual(int(keys["spawnflags"]) & 1, 1)
        self.assertEqual(len(brushes), 1)
        self.assertEqual({f[3] for f in brushes[0]}, {"trigger"})

    def test_lights(self):
        self.assertGreaterEqual(len(self.by_class["light"]), 15)
        # White only: a coloured light needs a .lit the bundle does not carry.
        for keys, _ in self.by_class["light"]:
            self.assertNotIn("_color", keys)
            self.assertNotIn("color", keys)


class BrokenVariants(unittest.TestCase):
    def test_missing_texture(self):
        used = {f[3] for _, brushes in arena("missing-texture") for b in brushes for f in b}
        self.assertIn(ga.TEX_MISSING, used)

    def test_leak_drops_a_wall(self):
        self.assertLess(len(arena("leak")[0][1]), len(arena()[0][1]))

    def test_no_exit(self):
        classes = [k["classname"] for k, _ in arena("no-exit")]
        self.assertNotIn("trigger_changelevel", classes)

    def test_cell_in_brush(self):
        x, y = ga.cell_centre(5, 3)
        world = arena("cell-in-brush")[0][1]
        self.assertTrue(any(overlaps(bounds(b), (x - 1, y - 1, 1), (x + 1, y + 1, 2))
                            for b in world))

    def test_cell_corner(self):
        x, y = ga.cell_centre(2, 2)
        x, y = x + ga.GRID_CELL / 2, y + ga.GRID_CELL / 2
        world = arena("cell-corner")[0][1]
        posts = [bounds(b) for b in world
                 if overlaps(bounds(b), (x - 1, y - 1, 1), (x + 1, y + 1, 2))]
        self.assertEqual(len(posts), 1)
        # Clear of every cell centre's hull-2 box, so only a whole-cell check sees it.
        for col in range(ga.GRID_COLS):
            for row in range(ga.GRID_ROWS):
                cx, cy = ga.cell_centre(col, row)
                self.assertFalse(overlaps(posts[0], (cx - 32, cy - 32, 0), (cx + 32, cy + 32, 88)))

    def test_pocket(self):
        ents = arena("pocket")
        grids = [k for k, _ in ents if k["classname"] == "psq_grid"]
        self.assertEqual([g["origin"] for g in grids], ["-304 -464 64"])
        world = ents[0][1]
        plats = [bounds(b) for b in world
                 if overlaps(bounds(b), (-305, -465, 63), (-303, -463, 64))]
        self.assertEqual(len(plats), 1, "a 64-high platform under the grid, no way up")

    def test_unknown_kind(self):
        with self.assertRaises(ga.ArenaError):
            ga.build("everything")

    def test_entity_values_cannot_inject(self):
        with self.assertRaises(ga.ArenaError):
            ga.map_text([], [({"classname": 'x" "y'}, [])], "w.wad")
        with self.assertRaises(ga.ArenaError):
            ga.map_text([], [({"classname": "x\n}"}, [])], "w.wad")


def pak_bytes(entries, dirofs=None, magic=b"PACK"):
    body = b"".join(data for _, data in entries)
    d = b""
    ofs = 12
    for name, data in entries:
        d += struct.pack("<56sii", name.encode(), ofs, len(data))
        ofs += len(data)
    if dirofs is None:
        dirofs = 12 + len(body)
    return struct.pack("<4sii", magic, dirofs, len(d)) + body + d


def miptex(name, w=16, h=16, fill=7):
    pix = w * h + (w // 2) * (h // 2) + (w // 4) * (h // 4) + (w // 8) * (h // 8)
    offs = [40, 40 + w * h, 40 + w * h + w * h // 4, 40 + w * h + w * h // 4 + w * h // 16]
    return struct.pack("<16sII4I", name.encode(), w, h, *offs) + bytes([fill]) * pix


def bsp_with_textures(blobs):
    lump = struct.pack("<i", len(blobs))
    ofs = 4 + 4 * len(blobs)
    body = b""
    for b in blobs:
        lump += struct.pack("<i", ofs + len(body) if b is not None else -1)
        body += b or b""
    lump += body
    header = struct.pack("<i", 29)
    lumps = [(0, 0)] * 15
    lumps[2] = (4 + 15 * 8, len(lump))
    for o, l in lumps:
        header += struct.pack("<ii", o, l)
    return header + lump


def brush_vertices(brush):
    """The corners of a convex brush: every point where three of its face
    planes meet that lies on or inside all of them."""
    planes = []
    for p0, p1, p2, _ in brush:
        n = cross(sub(p0, p1), sub(p2, p1))
        planes.append((n, dot(n, p1)))
    out = []
    for i in range(len(planes)):
        for j in range(i + 1, len(planes)):
            for k in range(j + 1, len(planes)):
                (a, da), (b, db), (c, dc) = planes[i], planes[j], planes[k]
                det = dot(a, cross(b, c))
                if det == 0:
                    continue
                v = tuple((da * x + db * y + dc * z) / det for x, y, z in
                          zip(cross(b, c), cross(c, a), cross(a, b)))
                if all(dot(n, v) - d <= 1e-6 * (abs(d) + 1) for n, d in planes):
                    out.append(v)
    return out


class Layouts(unittest.TestCase):
    """The prototype halls (--layout): the same rules as the shipped hall,
    with several spawn grids instead of one."""

    def each(self):
        for name in ga.LAYOUTS[1:]:
            world, ents, desc = ga.build_layout(name)
            yield name, parse_map(ga.map_text(world, ents, "psqarena.wad")), desc

    def test_layouts_listed(self):
        self.assertEqual(ga.LAYOUTS, ("hall", "terraces", "pit", "towers"))

    def test_deterministic(self):
        for name in ga.LAYOUTS[1:]:
            a = ga.map_text(*ga.build(layout=name), "psqarena.wad")
            b = ga.map_text(*ga.build(layout=name), "psqarena.wad")
            self.assertEqual(a, b, name)

    def test_test_hall_is_the_original_arena(self):
        # The one-grid hall, now psqtesthall, byte for byte as it shipped
        # before the multi-level arenas (8fd5ba4).
        import hashlib
        text = ga.map_text(*ga.build(), "psqarena.wad")
        self.assertEqual(hashlib.sha256(text.encode()).hexdigest(),
                         "d25c3e970e971c0d2c63883302672f0ac0d345b68377bd93e592279c5281cbbe")
        self.assertEqual(text, ga.map_text(*ga.build(layout="hall"), "psqarena.wad"))

    def test_arena_chain(self):
        names = [a[0] for a in ga.ARENAS]
        self.assertEqual(names, ["psqarena", "psqterraces", "psqtowers", "psqtesthall"])
        self.assertEqual(ga.ARENAS[0][1], "pit", "New Game opens the Pit")
        exits = {a[0]: a[2] for a in ga.ARENAS}
        # Pit, Terraces, Towers, then the campaign; nothing leads to the test hall.
        seen, at = [], "psqarena"
        while at in exits and at not in seen:
            seen.append(at)
            at = exits[at]
        self.assertEqual(seen, ["psqarena", "psqterraces", "psqtowers"])
        self.assertEqual(at, ga.EXIT_MAP)
        self.assertNotIn("psqtesthall", exits.values())
        for name, layout, exit_map in ga.ARENAS[:3]:
            maps = [k["map"] for k, _ in parse_map(ga.map_text(*ga.build(layout=layout,
                                                                        exit_map=exit_map),
                                                               "w"))
                    if k["classname"] == "trigger_changelevel"]
            self.assertEqual(maps, [exit_map], name)

    def test_texture_variety(self):
        walls = set()
        for name, ents, _ in self.each():
            used = {f[3] for _, brushes in ents for b in brushes for f in b}
            self.assertGreaterEqual(len(used), 24, f"{name}: {len(used)} textures")
            theme = ga.THEMES[name]
            for part in ("wall", "base", "frieze", "ceil", "floor", "cover", "pillar", "cap"):
                self.assertIn(theme[part], used, f"{name}: {part}")
            self.assertEqual(len(set(theme["inlay"].values())), len(theme["inlay"]), name)
            walls.add(theme["wall"])
            for t in (ga.TX_HAZARD, ga.TX_TREAD, ga.TX_LAMP, ga.TX_PANEL):
                self.assertIn(t, used, f"{name}: {t}")
        self.assertEqual(len(walls), 3, "each arena has its own walls")

    def test_lighting_has_contrast(self):
        # A low minimum light with dirt mapping (the corners fall dark), and a
        # few local lamps, not an even grid.
        for name in ga.LAYOUTS[1:]:
            _, ents, L = ga.build_layout(name)
            self.assertEqual(L["worldkeys"], {"light": str(ga.LIGHT_MIN), "_dirt": "1"}, name)
            self.assertLessEqual(ga.LIGHT_MIN, 10)
            self.assertTrue(4 <= len(L["lamps"]) <= 8, f"{name}: {len(L['lamps'])} lamps")
            text = ga.map_text(*ga.build(layout=name), "w", worldkeys_extra=L["worldkeys"])
            self.assertIn('"_dirt" "1"', text.split("}", 1)[0])

    def test_decorations(self):
        for name, ents, _ in self.each():
            by = {}
            for k, _ in ents:
                by[k["classname"]] = by.get(k["classname"], 0) + 1
            self.assertGreaterEqual(by.get("light_torch_small_walltorch", 0), 6, name)
            self.assertGreaterEqual(by.get("light_flame_large_yellow", 0), 2, name)
            self.assertGreaterEqual(by.get("light", 0), 12, name)


    def test_planes_face_outward(self):
        # Ramps and sloped bridges have five or six faces, not all axial: a
        # brush is good when its corners exist and their mean lies strictly
        # inside every face plane.
        for name, ents, _ in self.each():
            for keys, brushes in ents:
                for brush in brushes:
                    self.assertIn(len(brush), (5, 6), name)
                    verts = brush_vertices(brush)
                    self.assertGreaterEqual(len(verts), 6, f"{name}: {brush}")
                    c = tuple(sum(v[i] for v in verts) / len(verts) for i in range(3))
                    for p0, p1, p2, _ in brush:
                        n = cross(sub(p0, p1), sub(p2, p1))
                        self.assertLess(dot(n, sub(c, p1)), 0, f"{name}: face {p0} {p1} {p2}")

    def test_grids(self):
        for name, ents, desc in self.each():
            grids = [k for k, _ in ents if k["classname"] in ("psq_arena", "psq_grid")]
            self.assertEqual([k["classname"] for k in grids].count("psq_arena"), 1, name)
            self.assertEqual(grids[0]["classname"], "psq_arena", name)
            rects, total, floors = [], 0, set()
            for k in grids:
                x, y, z = (int(v) for v in k["origin"].split())
                cols, rows, cell = int(k["psq_cols"]), int(k["psq_rows"]), int(k["psq_cell"])
                self.assertEqual(cell, 96, name)
                rects.append((x - cols * 48, y - rows * 48, x + cols * 48, y + rows * 48))
                total += cols * rows
                floors.add(z)
            self.assertGreaterEqual(total, 35, f"{name}: room for psq_cap's 35")
            self.assertGreaterEqual(len(floors), 3, f"{name}: spawns on three levels")
            for i, a in enumerate(rects):
                for b in rects[i + 1:]:
                    self.assertFalse(a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3],
                                     f"{name}: grids {a} and {b} overlap in plan")
            for k, _ in ents:
                cls = k["classname"]
                if cls == "info_player_start" or cls.startswith(("item_", "weapon_")):
                    x, y, _ = (float(v) for v in k["origin"].split())
                    for r in rects:
                        self.assertFalse(x - 16 < r[2] and r[0] < x + 32 and
                                         y - 16 < r[3] and r[1] < y + 32,
                                         f"{name}: {cls} at ({x} {y}) is on a grid")

    def test_kept_from_the_hall(self):
        for name, ents, _ in self.each():
            by = {}
            for k, b in ents:
                by.setdefault(k["classname"], []).append((k, b))
            self.assertEqual(sorted(k["message"] for k, _ in by["trigger_setskill"]),
                             ["0", "1", "2", "3"], name)
            exits = by["trigger_changelevel"]
            self.assertEqual([(k["map"], k["spawnflags"]) for k, _ in exits],
                             [(ga.EXIT_MAP, "1")], name)
            self.assertEqual(by["info_player_start"][0][0]["origin"], "0 -896 24", name)
            used = {f[3] for _, brushes in ents for b in brushes for f in b}
            self.assertLessEqual(used, set(ga.TEXTURES) - {"skip"}, name)

    def test_shots_and_plan(self):
        import xml.etree.ElementTree as ET
        for name, _, desc in self.each():
            lines = ga.shots_text(desc).splitlines()
            self.assertGreaterEqual(sum(l.startswith("shot ") for l in lines), 2, name)
            self.assertEqual(sum(l.startswith("pin ") for l in lines), 6, name)
            ga._PLAN = []
            try:
                ga.build(layout=name)
                plan = ga._PLAN
            finally:
                ga._PLAN = None
            root = ET.fromstring(ga.plan_svg(name, plan, desc))
            self.assertTrue(root.tag.endswith("svg"), name)

    def test_break_needs_the_hall(self):
        with self.assertRaises(ga.ArenaError):
            ga.build("leak", layout="pit")
        with self.assertRaises(ga.ArenaError):
            ga.build_layout("maze")

    def test_stairs_and_ramps_reject_bad_sizes(self):
        with self.assertRaises(ga.ArenaError):
            ga.stairs(0, 0, 64, 100, 0, 64, "+y")      # 100 does not split into 3 treads
        with self.assertRaises(ga.ArenaError):
            ga.ramp(0, 0, 64, 64, 0, 64, 32, "+y", "trigger")   # top below its low edge
        with self.assertRaises(ga.ArenaError):
            ga.ramp(0, 0, 64, 64, 0, 0, 32, "up", "trigger")


class Readers(unittest.TestCase):
    def read(self, data):
        return ga.read_pak_dir(io.BytesIO(data), len(data))

    def test_pak_good(self):
        e = self.read(pak_bytes([("maps/a.bsp", b"xyz")]))
        self.assertEqual(e, {"maps/a.bsp": (12, 3)})

    def test_pak_bad(self):
        good = pak_bytes([("maps/a.bsp", b"xyz")])
        for bad in (b"", good[:8], b"PACX" + good[4:],
                    pak_bytes([("a", b"x")], dirofs=10 ** 6),
                    good[:12] + good[12:-4]):
            with self.assertRaises(ga.ArenaError):
                self.read(bad)
        # An entry pointing past the end of the file.
        d = struct.pack("<56sii", b"maps/a.bsp", 12, 10 ** 6)
        with self.assertRaises(ga.ArenaError):
            self.read(struct.pack("<4sii", b"PACK", 12, len(d)) + d)

    def test_miptex_extracts_whole_textures_only(self):
        good = miptex("good")
        short = miptex("short")[:-10]
        badoff = bytearray(miptex("badoff"))
        struct.pack_into("<I", badoff, 24, 99)
        odd = miptex("odd", w=24)
        got = ga.bsp_miptex(bsp_with_textures([good, None, bytes(badoff), odd, short]))
        self.assertEqual(sorted(got), ["good"])
        self.assertEqual(got["good"], good)

    def test_miptex_bad_lumps(self):
        with self.assertRaises(ga.ArenaError):
            ga.bsp_miptex(b"\x1d\0\0\0")
        data = bytearray(bsp_with_textures([miptex("a")]))
        struct.pack_into("<i", data, 4 + 2 * 8 + 4, 10 ** 6)
        with self.assertRaises(ga.ArenaError):
            ga.bsp_miptex(bytes(data))
        self.assertEqual(ga.bsp_miptex(b"\x1e\0\0\0" + bytes(120)), {})

    def test_wad2(self):
        w = ga.wad2({"b": miptex("b"), "a": miptex("a")})
        magic, n, dirofs = struct.unpack_from("<4sii", w, 0)
        self.assertEqual((magic, n), (b"WAD2", 2))
        names = []
        for i in range(n):
            ofs, dsize, size, typ, comp, _, name = struct.unpack_from(
                "<iiibbh16s", w, dirofs + 32 * i)
            self.assertEqual((typ, comp, dsize), (0x44, 0, size))
            names.append(name.rstrip(b"\0").decode())
            self.assertEqual(w[ofs:ofs + 16].rstrip(b"\0").decode(), names[-1])
        self.assertEqual(names, ["a", "b"])
        with self.assertRaises(ga.ArenaError):
            ga.wad2({"a_name_too_long_for_wad": miptex("x")})


if __name__ == "__main__":
    unittest.main(verbosity=2)
