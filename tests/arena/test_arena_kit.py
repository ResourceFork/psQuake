#!/usr/bin/env python3
"""Tests for the themed arenas' kit, layout and maps (tools/arena_kit.py,
arena_layout.py, arena_maps.py), and that the banked arenas are unchanged.

    test_arena_kit.py <tools dir>

Python only (no compilers): every brush the kit makes is convex with its
faces facing out; stairs keep to 16:32 or gentler and ramps to 1:2; the
carved space emits sealed solid; the Temple builds the same twice, its
grids keep apart in plan, its central area is at least 1024 square with 60%
of the cells; and generate_arena.py still writes the banked arenas' maps
byte for byte as they were (their SHA-256 pinned below).
"""

import hashlib
import math
import os
import sys
import unittest

sys.path.insert(0, os.path.abspath(sys.argv.pop(1)))
import arena_kit as K  # noqa: E402
import arena_layout  # noqa: E402
import arena_maps  # noqa: E402
import generate_arena as G  # noqa: E402

# generate_arena.py's .map text for each banked arena, as built before the
# themed arenas existed (commit 256766b), but for the skill arches and pads
# (gone: no map sets the skill), their pillars (octagonal) and the start
# room's baffle.
BANKED = {
    "psqarena": "9955b76ed7c5cbc34c7bf5c8ce267ea710526c7ddaafd47864cae6c6995394e7",
    "psqterraces": "ed378031e8dfe67d19146fc2d78e84885fd9c4ae4a79f13b2db81f435a547f23",
    "psqtowers": "68d02259ccaea4715d3f0c2112137aa30c6ff52d0a35426b27182b28715c2bca",
    "psqtesthall": "0fced81ea4f78ea909769b916d6e56b3072cdbef7949a8f0c44d57dde03b0835",
}


def outward(faces):
    """Every brush corner is on the inside of every face's plane."""
    pts = {p for f in faces for p in f[:3]}
    for p0, p1, p2, _ in faces:
        n = K._cross(K._sub(p0, p1), K._sub(p2, p1))
        d = K._dot(n, p1)
        if any(K._dot(n, q) - d > 1e-6 for q in pts):
            return False
    return True


class Kit(unittest.TestCase):
    def test_pieces_are_convex_and_outward(self):
        brushes = (K.column(0, 0, 24, 0, 256, "t")
                   + K.arch("x", 0, -64, 64, 0, 96, 160, 32, "t")
                   + K.arch("y", 0, -64, 64, 0, 96, 192, 32, "t", kind="pointed")
                   + K.stairs(0, 0, 256, 128, 0, 128, "+x", "t")
                   + K.ramp(0, 0, 64, 256, 0, 128, "-y", "t")
                   + K.beam((0, 0, 64), (300, 100, 200), 16, 16, "t")
                   + K.buttress(0, 0, "+x", 32, 0, 256, 96, "t")
                   + K.rocks(7, 0, 0, 0, 128, 6, 64, "t")
                   + K.frame("x", -32, 32, 0, 64, 160, 16, 8, "t", glass="g")
                   + K.alcove_torch("y", 0, -512, "+", 48, "t")[0])
        self.assertTrue(brushes)
        for b in brushes:
            self.assertGreaterEqual(len(b), 4)
            self.assertTrue(outward(b), b)

    def test_rocks_are_seeded(self):
        self.assertEqual(K.rocks(3, 0, 0, 0, 96, 4, 48, "r"), K.rocks(3, 0, 0, 0, 96, 4, 48, "r"))
        self.assertNotEqual(K.rocks(3, 0, 0, 0, 96, 4, 48, "r"),
                            K.rocks(4, 0, 0, 0, 96, 4, 48, "r"))

    def test_chutes_refused(self):
        f = dict(x0=-144, y0=-224, x1=144, y1=192, ztop=0, zbot=-96, inset=48, tex="t")
        for chutes in ([("z9", -96, -64, 16)], [("y1", -128, -96, 16)],
                       [("y1", -96, -64, 16), ("y1", -80, -48, 16)], [("y1", -96, -64, 200)]):
            with self.assertRaises(K.KitError, msg=chutes):
                K.pit_funnel(chutes=chutes, **f)
        self.assertTrue(K.pit_funnel(chutes=[("y1", -96, -64, 16)], **f))

    def test_steep_refused(self):
        with self.assertRaises(K.KitError):
            K.ramp(0, 0, 100, 64, 0, 64, "+x", "t")   # steeper than 1:2
        with self.assertRaises(K.KitError):
            K.stairs(0, 0, 100, 64, 0, 40, "+x", "t")  # not whole 16 steps
        with self.assertRaises(arena_layout.LayoutError):
            arena_layout._stair_segments({"rect": (0, 0, 128, 64), "low": 0, "high": 128,
                                          "rise": "+x"})   # 16 deep: steeper than 16:32

    def test_space_is_sealed(self):
        s = K.Space((-256, -256, -64, 256, 256, 256), 16)
        s.carve(-128, -128, 0, 128, 128, 128, "a")
        styles = {"a": {"floor": "f", "ceil": "c", "wall": "w"}, "d": {"floor": "F",
                                                                       "ceil": "C", "wall": "W"}}
        boxes = s.emit(styles, "d")
        # every point of the bounds is open (carved) or inside a box, never both

        def inside(b, p):
            xs = [q[0] for f in b for q in f[:3]]
            ys = [q[1] for f in b for q in f[:3]]
            zs = [q[2] for f in b for q in f[:3]]
            return min(xs) < p[0] < max(xs) and min(ys) < p[1] < max(ys) and \
                min(zs) < p[2] < max(zs)
        for x in range(-248, 256, 32):
            for y in range(-248, 256, 32):
                for z in range(-56, 256, 32):
                    solid = sum(inside(b, (x, y, z)) for b in boxes)
                    self.assertEqual(solid, 0 if s.open_at(x, y, z) else 1, (x, y, z))
        with self.assertRaises(K.KitError):
            s.carve(-250, 0, 0, 0, 16, 16, "a")   # off the grid


class Temple(unittest.TestCase):
    def setUp(self):
        self.L = arena_maps.MAPS["psqtemple"]

    def test_every_themed_map(self):
        chain = {"psqtemple": "psqruins", "psqruins": "psqforge", "psqforge": "psqarena"}
        self.assertEqual(sorted(arena_maps.MAPS), sorted(chain))
        for name, L in arena_maps.MAPS.items():
            self.assertEqual(L["name"], name)
            self.assertEqual(L["exit_map"], chain[name], name)
            _, _, D = arena_layout.build(dict(L))
            c = next(a for a in L["areas"] if a["id"] == L["central"])["box"]
            self.assertGreaterEqual(min(c[3] - c[0], c[4] - c[1]), 1024, name)
            total = sum(r["cols"] * r["rows"] for r in D["regions"])
            central = sum(r["cols"] * r["rows"] for r in D["regions"]
                          if r["area"] == L["central"])
            self.assertGreaterEqual(central / total, 0.6, name)
            self.assertGreaterEqual(len({a["box"][2] for a in L["areas"]}), 3, name)
            roles = {a.get("role") for a in L["areas"]}
            self.assertTrue({"central", "close", "mid", "far", "high"} <= roles, (name, roles))

    def test_deterministic(self):
        a = arena_layout.build(dict(self.L))
        b = arena_layout.build(dict(self.L))
        self.assertEqual(G.map_text(a[0], a[1], "x.wad", "t", a[2]["worldkeys"]),
                         G.map_text(b[0], b[1], "x.wad", "t", b[2]["worldkeys"]))

    def test_central_area_and_grids(self):
        _, _, D = arena_layout.build(dict(self.L))
        c = next(a for a in self.L["areas"] if a["id"] == self.L["central"])["box"]
        self.assertGreaterEqual(c[3] - c[0], 1024)
        self.assertGreaterEqual(c[4] - c[1], 1024)
        total = sum(r["cols"] * r["rows"] for r in D["regions"])
        central = sum(r["cols"] * r["rows"] for r in D["regions"] if r["area"] == "court")
        self.assertGreaterEqual(central / total, 0.6)
        self.assertTrue(all(any(r["area"] == a for r in D["regions"])
                            for a in ("aisle_w", "aisle_e", "gallery_w", "nave", "chapel_w1")))
        rects = [r["rect"] for r in D["regions"]]
        for i, a in enumerate(rects):
            for b in rects[i + 1:]:
                self.assertFalse(a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3],
                                 (a, b))
        self.assertEqual(self.L["exit_map"], "psqruins")
        self.assertEqual(self.L["name"], "psqtemple")


class Solid:
    """What is solid in a built layout: uncarved space and every brush
    (world and detail), but not liquids or triggers; and the liquid brushes."""

    def __init__(self, world, ents, sp, near=None):
        """near=(x, y, r): keep only the brushes reaching within r of (x, y)."""
        self.sp = sp
        self.brushes, self.liquids, self.raw = [], [], []
        for b in list(world) + [b for keys, bs in ents for b in bs
                                if not keys.get("classname", "").startswith("trigger")
                                and keys.get("classname") != "psq_safe"]:
            planes = []
            for p0, p1, p2, tex in b:
                n = K._cross(K._sub(p0, p1), K._sub(p2, p1))
                planes.append((n, K._dot(n, p1)))
            texs = {str(f[3]) for f in b}
            pts = [q for f in b for q in f[:3]]
            box = (tuple(min(q[i] for q in pts) for i in range(3)),
                   tuple(max(q[i] for q in pts) for i in range(3)))
            if near and (box[0][0] > near[0] + near[2] or box[1][0] < near[0] - near[2]
                         or box[0][1] > near[1] + near[2] or box[1][1] < near[1] - near[2]):
                continue
            if all(t.startswith("*") for t in texs):
                self.liquids.append((planes, box))
            elif not texs <= {"trigger", "clip"}:
                self.brushes.append((planes, box))
                self.raw.append(b)

    @staticmethod
    def _inside(planes, p):
        return all(K._dot(n, p) - d < -0.01 for n, d in planes)

    def solid(self, p):
        if self.sp is not None and not self.sp.open_at(*p):
            return True
        return any(bx[0][0] - 1 <= p[0] <= bx[1][0] + 1 and bx[0][1] - 1 <= p[1] <= bx[1][1] + 1
                   and bx[0][2] - 1 <= p[2] <= bx[1][2] + 1 and self._inside(pl, p)
                   for pl, bx in self.brushes)

    def water_top(self, p):
        for pl, box in self.liquids:
            if self._inside(pl, p):
                return box[1][2]
        return None


class Stairs(unittest.TestCase):
    """Every stair run of every themed map is entered from open floor at its
    low end and left onto open floor with headroom at its high end (ahead or
    to a side), every piece and brush counted; one rising out of water starts
    where the water (measured from its brush) is shallow enough to walk in."""
    HEAD = 72       # the player is 56 tall: room to walk off the top
    WAIST = 28      # SV_CheckWater: over this deep the player swims

    def test_stairs_enter_and_leave(self):
        bad = []
        for name, L in arena_maps.MAPS.items():
            world, ents, D = arena_layout.build(dict(L))
            so = Solid(world, ents, D["space"])

            def walkable(x, y, z):
                return all(not so.solid((x, y, z + dz)) for dz in range(2, self.HEAD, 8)) \
                    and so.solid((x, y, z - 4))
            for s in L.get("stairs", []):
                where = s.get("rect", s.get("centre"))
                low, high = arena_layout.stair_ends(s)
                if not walkable(*low):
                    bad.append(f"{name} {where}: its low end at ({low[0]:g} {low[1]:g} "
                               f"{low[2]:g}) meets a wall or no floor")
                if not any(walkable(*p) for p in [high] + arena_layout.stair_top_sides(s)):
                    bad.append(f"{name} {where}: its top at ({high[0]:g} {high[1]:g} "
                               f"{high[2]:g}) leads into a wall or onto no floor, ahead or to "
                               "either side")
                top = so.water_top((low[0], low[1], low[2] + 2))
                if top is not None and top - low[2] > self.WAIST - 4:
                    bad.append(f"{name} {where}: its foot stands in water {top - low[2]:g} "
                               "deep: the player swims there")
        self.assertEqual(bad, [], "\n" + "\n".join(bad))

    def test_spirals_face_the_open_water(self):
        """A spiral rising out of water is entered from the side toward the
        middle of the water it stands in, and leaves no spot to stand behind
        it: every place to stand at its foot's level near it sees the middle
        of that water past the stair (no dark pocket between it and the walls)."""
        bad = []
        for name, L in arena_maps.MAPS.items():
            world, ents, D = arena_layout.build(dict(L))
            for s in L.get("stairs", []):
                if s.get("kind") != "spiral" or "from_water" not in s:
                    continue
                low, _ = arena_layout.stair_ends(s)
                area = next(a for a in L["areas"]
                            if a["box"][0] <= low[0] <= a["box"][3]
                            and a["box"][1] <= low[1] <= a["box"][4]
                            and a["box"][2] <= low[2] < a["box"][5])
                x0, y0, _, x1, y1, _ = area["box"]
                mid = ((x0 + x1) / 2, (y0 + y1) / 2)
                cx, cy = s["centre"]
                rm, t = (s["r0"] + s["r1"]) / 2, math.radians(s["a0"])
                foot = (cx + rm * math.cos(t), cy + rm * math.sin(t))
                if math.dist(low[:2], mid) >= math.dist(foot, mid):
                    bad.append(f"{name} {s['centre']}: its foot is entered from "
                               f"({low[0]:g} {low[1]:g}), the side away from the middle of "
                               f"{area['id']}")
                reach = s["r1"] + 96
                so = Solid(world, ents, D["space"], near=(cx, cy, reach + 128))
                z = low[2]

                def stand(x, y):
                    return so.solid((x, y, z - 4)) and not any(
                        so.solid((x + dx, y + dy, z + dz)) for dx in (-15, 15)
                        for dy in (-15, 15) for dz in range(2, 56, 18))
                hidden = []
                for x in range(int(cx - reach), int(cx + reach) + 1, 16):
                    for y in range(int(cy - reach), int(cy + reach) + 1, 16):
                        if math.dist((x, y), (cx, cy)) > reach or not stand(x, y):
                            continue
                        d = math.dist((x, y), mid)
                        ux, uy = (mid[0] - x) / d, (mid[1] - y) / d
                        k = 8
                        while math.dist((x + ux * k, y + uy * k), (cx, cy)) <= reach + 64:
                            if so.solid((x + ux * k, y + uy * k, z + 40)):
                                hidden.append((x, y))
                                break
                            k += 8
                if hidden:
                    bad.append(f"{name} {s['centre']}: {len(hidden)} spots to stand behind it, "
                               f"hidden from the middle of {area['id']}, e.g. {hidden[:3]}")
        self.assertEqual(bad, [], "\n" + "\n".join(bad))

    PLAYER_TOP = 56     # the player's hull stands 56 tall over its floor
    CLEAR = 8           # and a wall fixture clears it by this much

    def test_fixtures_clear_the_player(self):
        """No sconce or alcove torch sticking out of a wall hangs low enough
        to catch the player walking along that wall: its lowest part that
        stands out of the wall is at least PLAYER_TOP + CLEAR over the
        highest floor under any part of a player's hull that meets it (16
        past it along the wall and out from it), on every map."""
        bad = []
        for name, L in arena_maps.MAPS.items():
            world, ents, D = arena_layout.build(dict(L))
            so = Solid(world, ents, D["space"])
            for kind, a in L.get("pieces", []):
                if kind not in ("sconce", "alcove_torch"):
                    continue
                brushes, _ = arena_layout._piece(kind, a)
                i = 1 if a["axis"] == "x" else 0
                s = 1 if a["face"] == "+" else -1
                pts = [q for b in brushes for f in b for q in f[:3]]
                out = [q for q in pts if s * (q[i] - a["at"]) > 1]
                if not out:
                    continue    # flush or recessed: nothing to catch on
                stick = max(s * (q[i] - a["at"]) for q in out)
                low = min(q[2] for q in out)
                j = 0 if a["axis"] == "x" else 1
                along = [q[j] - a["u"] for q in out]
                w = K._on_wall(a["axis"], a["u"], a["at"], a["face"])
                # the highest floor under any part of a player's hull that
                # meets the fixture: 16 past it along the wall and out
                floor = None
                for da in range(int(min(along)) - 16, int(max(along)) + 17, 8):
                    for do in range(2, int(stick) + 17, 8):
                        x, y, _ = w(da, do, 0)
                        z = low - 1
                        if so.solid((x, y, z)):
                            continue
                        while z > low - 4096 and not so.solid((x, y, z)):
                            z -= 1
                        floor = z + 1 if floor is None else max(floor, z + 1)
                if floor is None:
                    continue
                if low - floor < self.PLAYER_TOP + self.CLEAR:
                    bad.append(f"{name} {a.get('design', kind)} at {a['axis']} {a['u']} on "
                               f"{a['at']}: {stick:g} out of the wall, {low - floor:g} over the "
                               f"floor at {floor:g} (needs {self.PLAYER_TOP + self.CLEAR})")
        self.assertEqual(bad, [], "\n" + "\n".join(bad))

    def test_sconces_on_walls(self):
        """Every sconce is flush on a wall: solid just behind its plate, open
        in front of it, its flame in the open."""
        bad = []
        for name, L in arena_maps.MAPS.items():
            world, ents, D = arena_layout.build(dict(L))
            so = Solid(world, ents, D["space"])
            for kind, a in L.get("pieces", []):
                if kind != "sconce":
                    continue
                w = K._on_wall(a["axis"], a["u"], a["at"], a["face"])
                behind = w(0, -4, a["z"] + 16)
                front = w(0, 40, a["z"] + 16)
                if not so.solid(behind) or so.solid(front):
                    bad.append(f"{name} {a['design']} at {a['axis']} {a['u']} on {a['at']}: "
                               "no wall behind it or none open in front")
            for keys, _ in ents:
                if keys.get("classname", "").startswith("light"):
                    p = tuple(float(v) for v in keys["origin"].split())
                    if so.solid(p):
                        bad.append(f"{name} {keys['classname']} at {p} is inside a brush")
        self.assertEqual(bad, [], "\n" + "\n".join(bad))


def _prism(b):
    """(vertical side count, lo, hi) of an upright prism brush, else None."""
    up, flat = 0, 0
    for p0, p1, p2, _ in b:
        n = K._cross(K._sub(p0, p1), K._sub(p2, p1))
        m = math.sqrt(sum(v * v for v in n)) or 1
        if abs(n[2]) / m < 1e-6:
            up += 1
        elif abs(abs(n[2]) / m - 1) < 1e-6:
            flat += 1
        else:
            return None
    if flat != 2:
        return None
    pts = [q for f in b for q in f[:3]]
    return (up, [min(q[i] for q in pts) for i in range(3)],
            [max(q[i] for q in pts) for i in range(3)])


def _every_map():
    """(name, world, ents, space or None) of every arena, banked and themed."""
    for name, layout, exit_map in G.ARENAS:
        G._PLAN = []
        try:
            world, ents = G.build(None, layout, exit_map)
        finally:
            G._PLAN = None
        yield name, world, ents, None
    for name, L in arena_maps.MAPS.items():
        world, ents, D = arena_layout.build(dict(L))
        yield name, world, ents, D["space"]


class Round3(unittest.TestCase):
    """The third play-test: round columns with their own caps, low thin perch
    rails, Forge's bridged and fed pits."""

    def test_columns_are_round(self):
        """Every free-standing column (an upright prism, its footprint near
        square and at most 72 across, at least 2.5 times as tall, open on
        all round at some height) of every map has at least eight sides."""
        bad = []
        for name, world, ents, sp in _every_map():
            so = Solid(world, ents, sp) if sp is not None else Solid(world, ents, None)
            for b in so.raw:
                r = _prism(b)
                if not r:
                    continue
                n, lo, hi = r
                dx, dy, h = hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]
                w = max(dx, dy)
                if not (12 <= min(dx, dy) and min(dx, dy) / w >= 0.85 and w <= 72
                        and h >= max(80, 2.5 * w)) or n >= 8:
                    continue
                mx, my = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
                # free-standing at some height: open all round there
                free = False
                for k in range(1, 8):
                    mz = lo[2] + h * k / 8
                    sides = sum(not so.solid(q) for q in ((lo[0] - 8, my, mz), (hi[0] + 8, my, mz),
                                                          (mx, lo[1] - 8, mz), (mx, hi[1] + 8, mz)))
                    free = free or sides == 4
                if not free:
                    continue
                bad.append(f"{name}: a {n}-sided column at ({mx:g} {my:g}) "
                           f"{sorted({str(f[3]) for f in b})[0]}")
        self.assertEqual(bad, [], "\n" + "\n".join(bad))

    def test_column_caps_have_their_own_texture(self):
        """Every capped column on a themed map takes a cap texture that is no
        wall's (no style's wall, base or frieze) and not its shaft's."""
        bad = []
        for name, L in arena_maps.MAPS.items():
            walls = set()
            for st in L["styles"].values():
                walls.add(st["wall"])
                for band in ("base", "frieze"):
                    if band in st:
                        walls.add(st[band][0])
            for kind, a in L.get("pieces", []):
                if kind not in ("column", "colonnade"):
                    continue
                cap = a.get("cap_tex")
                if cap is None or cap in walls or cap == a["tex"]:
                    bad.append(f"{name} {kind} at ({a.get('cx', a.get('a'))} "
                               f"{a.get('cy', a.get('at'))}): cap {cap}")
        self.assertEqual(bad, [], "\n" + "\n".join(bad))

    EYE = 46        # the player's eye over its floor
    RAIL_DEPTH = 8  # a rail is this thin, seen across
    # perches that must be railed along every edge that drops away, but for
    # the gaps (along the edge) left open to drop from
    RAILED = {("psqruins", "tower", "x1"): [], ("psqruins", "ledge", "x0"): [(1088, 1216)]}

    def test_perch_rails_low_and_thin(self):
        """Where a rail runs along an edge of a high perch that drops away, it
        stops a player walking off (solid over a step's height, 18), its top
        is under the eye (46) and it is at most 8 thick, so the player looks
        over it at what is below. A drop with no rail is left open."""
        bad = []
        for name, L in arena_maps.MAPS.items():
            world, ents, D = arena_layout.build(dict(L))
            for a in L["areas"]:
                if a.get("role") != "high":
                    continue
                x0, y0, z0, x1, y1, z1 = a["box"]
                so = Solid(world, ents, D["space"], near=((x0 + x1) / 2, (y0 + y1) / 2,
                                                          max(x1 - x0, y1 - y0)))
                edges = [((x, y0), (0, 1), "y0") for x in range(x0 + 2, x1 - 1, 4)] + \
                        [((x, y1), (0, -1), "y1") for x in range(x0 + 2, x1 - 1, 4)] + \
                        [((x0, y), (1, 0), "x0") for y in range(y0 + 2, y1 - 1, 4)] + \
                        [((x1, y), (-1, 0), "x1") for y in range(y0 + 2, y1 - 1, 4)]
                worst = {}
                for (ex, ey), (ix, iy), side in edges:
                    gaps = self.RAILED.get((name, a["id"], side))
                    out = (ex - 8 * ix, ey - 8 * iy)
                    # an edge that drops away (more than a step: not a stair's
                    # top) with a rail along it; an open drop has none
                    if so.solid((out[0], out[1], z0 + 80)) or so.solid((out[0], out[1], z0 - 24)):
                        continue
                    band = [(ex + d * ix, ey + d * iy) for d in range(1, 25)]
                    along = ex if iy else ey
                    if not any(so.solid((x, y, z)) for x, y in band for z in range(z0 + 2, z0 + 81, 2)):
                        if gaps is not None and not any(g0 <= along <= g1 for g0, g1 in gaps):
                            worst.setdefault((a["id"], "no rail"), (ex, ey))
                        continue
                    stops = any(so.solid((x, y, z)) for x, y in band for z in range(20, 41, 4)
                                for z in [z0 + z])
                    top = max((z for x, y in band for z in range(z0 + 1, z0 + 81)
                               if so.solid((x, y, z - 0.5))), default=None)
                    deep = sum(1 for x, y in band if so.solid((x, y, z0 + 24)))
                    why = []
                    if not stops:
                        why.append("nothing stops a walker")
                    if top is not None and top >= z0 + self.EYE:
                        why.append(f"top {top - z0} over the floor")
                    if deep > self.RAIL_DEPTH:
                        why.append(f"{deep} thick")
                    if why:
                        worst.setdefault((a["id"], "; ".join(why)), (ex, ey))
                for (aid, why), at in worst.items():
                    bad.append(f"{name} {aid} edge near {at}: {why}")
        self.assertEqual(bad, [], "\n" + "\n".join(bad))

    def _pits(self, L):
        """Forge's central pits: lava boxes in the central area, below its floor."""
        c = next(a for a in L["areas"] if a["id"] == L["central"])["box"]
        out = []
        for kind, a in L.get("world_pieces", []):
            if kind == "box" and a["tex"] == arena_maps.LAVA and a["hi"][2] < c[2] - 32 \
                    and c[0] < a["lo"][0] and a["hi"][0] < c[3]:
                out.append(a)
        return out

    def test_forge_pits_bridged(self):
        """Every central lava pit in Forge has a bridge across it: a line over
        its whole length that stands at the hall's floor with headroom."""
        L = arena_maps.MAPS["psqforge"]
        world, ents, D = arena_layout.build(dict(L))
        so = Solid(world, ents, D["space"])
        pits = self._pits(L)
        self.assertGreaterEqual(len(pits), 2)
        bad = []
        for p in pits:
            (px0, py0, _), (px1, py1, _) = p["lo"], p["hi"]
            ok = any(all(so.solid((x, y, -2)) and not so.solid((x, y, 8))
                         for y in range(py0 - 4, py1 + 5, 8))
                     for x in range(px0 + 4, px1 - 3, 8))
            if not ok:
                bad.append(f"the pit {p['lo']}..{p['hi']} has no bridge")
        self.assertEqual(bad, [], "\n" + "\n".join(bad))

    def test_forge_lava_at_one_level(self):
        """Every lava surface in Forge but the central pits' (the pours land
        there, down their funnels) sits at one height: the side channels,
        the forecourt pool, the floor channels and any slope between them."""
        L = arena_maps.MAPS["psqforge"]
        pits = [(p["lo"], p["hi"]) for p in self._pits(L)]
        tops = {}
        for kind, a in L["world_pieces"]:
            if kind == "box" and a["tex"] == arena_maps.LAVA and (a["lo"], a["hi"]) not in pits:
                tops.setdefault(a["hi"][2], []).append(f"pool {a['lo']}..{a['hi']}")
            if kind == "lava_channel":
                tops.setdefault(a["z"] - a["surface"], []).append(f"channel from {a['path'][0]}")
                if a.get("inflow") is not None:
                    tops.setdefault(a["inflow"], []).append(f"its slope into {a['path'][0]}")
        self.assertEqual(len(tops), 1, "\n" + "\n".join(f"z {z}: {', '.join(v)}"
                                                          for z, v in sorted(tops.items())))

    SMALLEST_HULL = 32
    POUR_LIFT = 0      # a pour's lava lies at or under the wall's slope

    def test_forge_nothing_over_the_lava(self):
        """Where a channel meets its pit (within 64 of the pit's edge, the
        mouth and the whole pour), nothing solid draws over its lava: just
        over every point of the lava's top is open (no funnel wall, rim or
        trim across it)."""
        L = arena_maps.MAPS["psqforge"]
        world, ents, D = arena_layout.build(dict(L))
        so = Solid(world, ents, D["space"])
        bad = []
        for kind, a in L["world_pieces"]:
            if kind != "lava_channel":
                continue
            brushes, _ = arena_layout._piece(kind, a)
            for b in brushes:
                if not all(str(face[3]).startswith("*") for face in b):
                    continue
                planes = []
                for p0, p1, p2, _ in b:
                    n = K._cross(K._sub(p0, p1), K._sub(p2, p1))
                    planes.append((n, K._dot(n, p1)))
                pts = [q for face in b for q in face[:3]]
                lo = [min(q[i] for q in pts) for i in range(3)]
                hi = [max(q[i] for q in pts) for i in range(3)]
                ex, ey = a["path"][-1]      # the pit's edge: judge round the meeting
                for x in range(int(lo[0]) + 1, int(hi[0]), 2):
                    for y in range(int(lo[1]) + 1, int(hi[1]), 2):
                        if math.dist((x, y), (ex, ey)) > 64:
                            continue
                        z = hi[2]
                        while z > lo[2] and not Solid._inside(planes, (x, y, z)):
                            z -= 0.5
                        if z <= lo[2]:
                            continue
                        if so.solid((x, y, z + 1.5)):
                            bad.append(f"solid over the lava at ({x} {y} {z + 1.5:g})")
        self.assertEqual(bad[:8], [], f"\n{len(bad)} points, e.g.\n" + "\n".join(bad[:8]))

    def test_forge_pours_match_their_chutes(self):
        """Each pour's slope is its funnel's fall and its channel's depth its
        chute's: the lava then lies on the chute's bed, 4 under the wall."""
        L = arena_maps.MAPS["psqforge"]
        funnels = [a for kind, a in L["world_pieces"] if kind == "pit_funnel"]
        bad = []
        for kind, a in L["world_pieces"]:
            if kind != "lava_channel" or "sections" not in a["pour"]:
                continue
            ex, ey = a["path"][-1]
            f = next(f for f in funnels if f["x0"] <= ex <= f["x1"] and f["y0"] <= ey <= f["y1"])
            fall = (f["ztop"] - f["zbot"]) / f["inset"]
            if a["pour"]["slope"] != fall:
                bad.append(f"pour at ({ex} {ey}): slope {a['pour']['slope']}, funnel falls {fall:g}")
            here = [c for c in f.get("chutes", ())
                    if c[1] <= (ex if c[0][0] == "y" else ey) <= c[2]]
            if len(here) != 1:
                bad.append(f"pour at ({ex} {ey}): {len(here)} chutes under it")
            elif here[0][3] != a["depth"]:
                bad.append(f"pour at ({ex} {ey}): channel {a['depth']} deep, chute {here[0][3]}")
            elif here[0][2] - here[0][1] < 2 * max(w for _, w in a["pour"]["sections"]):
                bad.append(f"pour at ({ex} {ey}): wider than its chute")
        self.assertEqual(bad, [])

    def test_forge_pours_lie_on_the_wall(self):
        """Each pour's lava (its stream and the channel's lava turning into it)
        lies on the pit's funnel wall: no point of it over the wall stands
        more than POUR_LIFT above the wall's slope there."""
        L = arena_maps.MAPS["psqforge"]
        funnels = [a for kind, a in L["world_pieces"] if kind == "pit_funnel"]
        bad = []
        for kind, a in L["world_pieces"]:
            if kind != "lava_channel":
                continue
            brushes, _ = arena_layout._piece(kind, a)
            ex, ey = a["path"][-1]
            dx, dy = {"+y": (0, 1), "-y": (0, -1), "+x": (1, 0), "-x": (-1, 0)}[a["pour"]["face"]]
            f = next(f for f in funnels if f["x0"] <= ex <= f["x1"] and f["y0"] <= ey <= f["y1"])
            fall = (f["ztop"] - f["zbot"]) / f["inset"]
            for b in brushes:
                if not all(str(face[3]).startswith("*") for face in b):
                    continue
                for p in {q for face in b for q in face[:3]}:
                    d = (p[0] - ex) * dx + (p[1] - ey) * dy
                    if not 0 < d < f["inset"]:
                        continue
                    lift = p[2] - (f["ztop"] - fall * d)
                    if lift > self.POUR_LIFT:
                        bad.append(f"pour at ({ex} {ey}): lava {lift:g} over the wall, {d:g} in")
        self.assertEqual(sorted(set(bad))[:8], [], "\n" + "\n".join(sorted(set(bad))[:8]))

    def test_forge_pits_fed_by_narrow_channels(self):
        """Every central pit in Forge is fed by a lava channel from a side
        channel, narrower than the smallest hull (32), that pours over the
        pit's edge."""
        L = arena_maps.MAPS["psqforge"]
        pits = self._pits(L)
        chans = [a for kind, a in L.get("pieces", []) + L.get("world_pieces", [])
                 if kind == "lava_channel"]
        bad = []
        world, ents, D = arena_layout.build(dict(L))
        so = Solid(world, ents, D["space"])
        for a in chans:
            # measured across each leg's middle, just under the floor: the
            # opening the built rims and floor leave (lava is not solid)
            for (x0, y0), (x1, y1) in zip(a["path"], a["path"][1:]):
                mx, my = (x0 + x1) / 2, (y0 + y1) / 2
                across = [(mx + d, my) if x0 == x1 else (mx, my + d) for d in range(-40, 41)]
                gap = sum(1 for p in across if not so.solid((p[0], p[1], a["z"] - 2)))
                if gap >= self.SMALLEST_HULL:
                    bad.append(f"a channel {gap} open across at ({mx:g} {my:g})")
        for p in pits:
            (px0, py0, _), (px1, py1, _) = p["lo"], p["hi"]
            fed = [a for a in chans
                   if px0 <= a["path"][-1][0] <= px1 and py0 - 1 <= a["path"][-1][1] <= py1 + 1]
            if not fed:
                bad.append(f"no channel pours into the pit {p['lo']}..{p['hi']}")
        self.assertEqual(bad, [], "\n" + "\n".join(bad))


class Banked(unittest.TestCase):
    def test_banked_arenas_unchanged(self):
        for name, layout, exit_map in G.ARENAS:
            G._PLAN = []
            try:
                world, ents = G.build(None, layout, exit_map)
            finally:
                G._PLAN = None
            L = G.hall_description() if layout == "hall" else G.build_layout(layout, exit_map)[2]
            text = G.map_text(world, ents, "psqarena.wad", G.ARENA_TITLES[layout],
                              L.get("worldkeys"))
            self.assertEqual(hashlib.sha256(text.encode("ascii")).hexdigest(), BANKED[name],
                             name)


if __name__ == "__main__":
    unittest.main()
