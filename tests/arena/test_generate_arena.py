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
import math
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
            self.assertEqual({k: L["worldkeys"][k] for k in ("light", "_minlight_dirt", "_dirt")},
                             {"light": str(ga.LIGHT_MIN), "_minlight_dirt": "0", "_dirt": "1"}, name)
            self.assertLessEqual(ga.LIGHT_MIN, 10)
            self.assertTrue(4 <= len(L["lamps"]) <= 8, f"{name}: {len(L['lamps'])} lamps")
            text = ga.map_text(*ga.build(layout=name), "w", worldkeys_extra=L["worldkeys"])
            self.assertIn('"_dirt" "1"', text.split("}", 1)[0])

    def test_health_and_ammo(self):
        # Enough to survive the first rush: five health packs (one mega), and
        # big boxes for the weapons the map places (super shotgun, nailgun).
        for name, ents, _ in self.each():
            items = [k for k, _ in ents if k["classname"].startswith(("item_", "weapon_"))]
            health = [k for k in items if k["classname"] == "item_health"]
            self.assertGreaterEqual(len(health), 5, name)
            self.assertEqual(sum(k.get("spawnflags") == "2" for k in health), 1, name)
            for cls in ("item_shells", "item_spikes"):
                boxes = [k for k in items if k["classname"] == cls]
                self.assertTrue(any(k.get("spawnflags") == "1" for k in boxes), f"{name}: {cls}")
            self.assertEqual({k["classname"] for k in items if k["classname"].startswith("weapon_")},
                             {"weapon_supershotgun", "weapon_nailgun"}, name)

    def test_resupply_spots(self):
        # Every multi-level arena gets resupply: spots on at least two levels,
        # off the grids, apart from each other and from the static pickups.
        for name, ents, _ in self.each():
            spots = [tuple(float(v) for v in k["origin"].split())
                     for k, _ in ents if k["classname"] == "psq_supply"]
            self.assertGreaterEqual(len(spots), 6, name)
            self.assertGreaterEqual(len({z for _, _, z in spots}), 2, name)
            items = [tuple(float(v) for v in k["origin"].split()) for k, _ in ents
                     if k["classname"].startswith(("item_", "weapon_"))]
            for i, a in enumerate(spots):
                for b in spots[i + 1:] + items:
                    self.assertGreater(abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]),
                                       40, f"{name}: spot {a} next to {b}")

    def test_start_room(self):
        # Four arches, each with its skill trigger and its name above it in
        # LibreQuake's font; a clear space after them; the lip into the hall.
        world, ents, L = ga.build_layout("pit")
        parsed = parse_map(ga.map_text(world, ents, "w"))
        skills = [(k["message"], bounds(b[0])) for k, b in parsed if k["classname"] == "psq_skill_arch"]
        self.assertEqual([m for m, _ in skills], ["0", "1", "2", "3"])
        xs = [lo[0] for _, (lo, hi) in skills]
        self.assertEqual(xs, sorted(xs), "easy to nightmare, left to right")
        for m, (lo, hi) in skills:
            self.assertEqual((lo[1], hi[1]), ga.ARCH_Y, "in the arch wall")
            self.assertGreaterEqual(hi[0] - lo[0], 96, "wide enough to walk through")
        tex = {f[3] for b in parsed[0][1] for f in b}
        for name in ga.SKILL_NAMES:
            for ch in name.lower():
                self.assertIn("{char_" + ch, tex, name)
        letters = [bounds(b) for b in parsed[0][1] if b[0][3].startswith("{char_")]
        self.assertEqual(len(letters), sum(len(n) for n in ga.SKILL_NAMES))
        for lo, hi in letters:   # whole letters: each box one texture tile
            self.assertEqual((hi[0] - lo[0], hi[2] - lo[2]), (16, 16))
            self.assertEqual((lo[0] % 16, lo[2] % 16), (0, 0))
        start_y = ga.START[1]
        self.assertLess(start_y, ga.ARCH_Y[0] - 200, "the arches seen from the start")
        self.assertGreaterEqual(ga.START_ROOM[4] - ga.ARCH_Y[1], 256, "a clear space after them")
        lips = [bounds(b) for b in parsed[0][1]
                if bounds(b)[0][1] == ga.START_ROOM[4] and bounds(b)[1][2] == ga.LIP_H]
        self.assertEqual(len(lips), 1, "the lip in the doorway")
        self.assertTrue(18 < ga.LIP_H < 45, "jumpable, not steppable")
        self.assertTrue(56 <= ga.START_DOOR_TOP - ga.LIP_H < 88,
                        "room over the lip for the player (56), not for hull 2 (88)")
        safe = [bounds(b[0]) for k, b in parsed if k["classname"] == "psq_safe"]
        self.assertEqual(len(safe), 4, "no monster is placed in the start room or the escape")
        lo, hi = safe[0]
        self.assertTrue(lo[1] <= ga.START[1] <= hi[1] and hi[1] >= ga.START_ROOM[4] + 2 * ga.WALL)
        self.assertEqual([k["classname"] for k, _ in parsed].count("info_null"), 1,
                         "the hall marked occupied for qbsp's fill")

    def test_exit_door(self):
        # Every arena: a door the player shoots open, in front of the exit.
        for name, ents, L in self.each():
            doors = [(k, b) for k, b in ents if k["classname"] == "psq_exit_door"]
            exits = [(k, b) for k, b in ents if k["classname"] == "trigger_changelevel"]
            self.assertEqual(len(doors), 1, name)
            k, b = doors[0]
            self.assertEqual((k["health"], k["wait"]), ("1", "-1"), name)
            dlo, dhi = bounds(b[0])
            tlo, thi = bounds(exits[0][1][0])
            self.assertLessEqual(dhi[1], tlo[1], f"{name}: the door before the teleporter")
            self.assertTrue(dlo[0] <= tlo[0] and dhi[0] >= thi[0], f"{name}: the door spans it")
            self.assertIn("start-room", L["worldkeys"]["_psq_rules"])
            self.assertIn("exit-door", L["worldkeys"]["_psq_rules"])

    def test_escape(self):
        # A hallway from the clear space after the arches out of the room's
        # east side and round, up to a balcony set into the hall's east wall
        # with a window on the hall, far from the doorway; high enough over
        # the floor below that nothing climbs up, low enough that the drop
        # does no damage; low cover at its ends; lit, and in psq_safe zones.
        for name, ents, L in self.each():
            yc, H = L["balcony"]
            world = ents[0][1]
            box_ = [bounds(b) for b in world]
            # The mouth: nothing in the room's east wall there, after the arches.
            ex = ga.START_ROOM[3]
            inside = [(lo, hi) for lo, hi in box_ if lo[0] < ex + ga.WALL and hi[0] > ex
                      and lo[1] < ga.ESC_Y[1] and hi[1] > ga.ESC_Y[0]
                      and lo[2] < ga.ESC_TOP and hi[2] > 0]
            self.assertEqual(inside, [], f"{name}: the mouth is open")
            self.assertTrue(ga.ARCH_Y[1] < ga.ESC_Y[0] and ga.ESC_Y[1] < ga.START_ROOM[4],
                            "after the arches, before the lip")
            # The window in the hall's east wall, over the balcony's floor.
            hx = L["hall"][2]
            east = [(lo, hi) for lo, hi in box_ if lo[0] == hx and hi[0] == hx + ga.WALL]
            yb0, yb1 = yc - ga.BALCONY_HALF, yc + ga.BALCONY_HALF
            for lo, hi in east:
                if lo[1] < yb1 and yb0 < hi[1]:
                    self.assertTrue(hi[2] <= H or lo[2] >= H + ga.BALCONY_HEAD,
                                    f"{name}: east wall {lo} {hi} in the window")
            # Far from the doorway, out of a fiend's leap, no fall damage.
            self.assertGreater(math.hypot(hx - ga.HALL_ENTRY[0], yc - ga.HALL_ENTRY[1]), 600, name)
            below = {"pit": 128, "terraces": 96, "towers": 0}[name]   # under the window
            self.assertTrue(ga.MELEE < H - below <= ga.FALL_SAFE, f"{name}: {H} over {below}")
            cover = [(lo, hi) for lo, hi in box_ if lo[2] == H and hi[2] == H + ga.COVER_H
                     and lo[0] == ga.ESC_X[0]]
            self.assertEqual(len(cover), 2, f"{name}: low cover at the window's ends")
            lights = [tuple(float(v) for v in k["origin"].split()) for k, _ in ents
                      if k["classname"] == "light"]
            self.assertTrue(any(abs(x - (ex - 24)) < 1 and ga.ESC_Y[0] < y < ga.ESC_Y[1]
                                for x, y, z in lights), f"{name}: the mouth is lit")
            self.assertTrue(any(x > ga.ESC_X[0] and yb0 <= y <= yb1 for x, y, z in lights),
                            f"{name}: the balcony is lit")
            safe = [bounds(b[0]) for k, b in ents if k["classname"] == "psq_safe"]
            self.assertTrue(any(lo[0] <= ga.ESC_X[0] + 16 and hi[0] >= ga.ESC_X[1] - 16
                                and lo[1] <= ga.ESC_Y[0] and hi[1] >= yb1 for lo, hi in safe),
                            f"{name}: the hallway and balcony are safe")
            # Nothing of the hall's on it: no grid, no pickup, no resupply spot.
            for k, _ in ents:
                if k["classname"] in ("psq_arena", "psq_grid", "psq_supply") \
                        or k["classname"].startswith(("item_", "weapon_")):
                    x, y, z = (float(v) for v in k["origin"].split())
                    self.assertLess(x, hx, f"{name}: {k['classname']} at {x} {y}")
            self.assertIn("balcony", L["worldkeys"]["_psq_rules"])
            self.assertIn("balcony", [c[0] for c in L["cameras"]])

    def test_railings(self):
        # Each railing: a clip brush over its footprint (solid to the walking
        # hulls, as the block it replaced) with a kick plate, posts at most
        # RAIL_POST apart and two rails inside it, all thinner than it.
        for name, ents, L in self.each():
            world = [bounds(b) + (b[0][3],) for b in ents[0][1]]
            clips = [(lo, hi) for lo, hi, tex in world if tex == ga.TEX_CLIP]
            self.assertGreaterEqual(len(clips), 4, name)
            for lo, hi in clips:
                inside = [(a, b) for a, b, tex in world if tex != ga.TEX_CLIP
                          and all(lo[i] <= a[i] and b[i] <= hi[i] for i in range(3))]
                along = 0 if hi[0] - lo[0] >= hi[1] - lo[1] else 1
                tops = sorted({b[2] for a, b in inside})
                self.assertEqual(tops[-1], hi[2], f"{name}: a top rail at {lo} {hi}")
                self.assertIn(lo[2] + ga.RAIL_KICK, tops, f"{name}: a kick plate at {lo}")
                posts = sorted(a[along] for a, b in inside if b[along] - a[along] == ga.RAIL_POST_W)
                self.assertGreaterEqual(len(posts), 2, f"{name}: posts at {lo} {hi}")
                gaps = [b - a for a, b in zip(posts, posts[1:])]
                self.assertLessEqual(max(gaps), ga.RAIL_POST, f"{name}: posts at {lo} {hi}")
                self.assertIn(hi[2] - lo[2], (32, ga.COVER_H), f"{name}: {lo} {hi}")
        # The Pit's railings round the pit lip stay a jump for the player and
        # a wall for monsters (32: over a step of 18, under a jump of 45).
        _, ents, _ = next(e for e in self.each() if e[0] == "pit")
        clips = [bounds(b) for b in ents[0][1] if b[0][3] == ga.TEX_CLIP]
        self.assertTrue(any(lo[2] == 0 and hi[2] == 32 and lo[1] == 256 for lo, hi in clips))

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
            self.assertEqual(sorted(k["message"] for k, _ in by["psq_skill_arch"]),
                             ["0", "1", "2", "3"], name)
            self.assertNotIn("trigger_setskill", by, f"{name}: every arch says its skill")
            exits = by["trigger_changelevel"]
            self.assertEqual([(k["map"], k["spawnflags"]) for k, _ in exits],
                             [(ga.EXIT_MAP, "1")], name)
            self.assertEqual(by["info_player_start"][0][0]["origin"], "0 -1248 24", name)
            used = {f[3] for _, brushes in ents for b in brushes for f in b}
            self.assertLessEqual(used, (set(ga.TEXTURES) - {"skip"}) | {ga.TEX_CLIP}, name)

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
