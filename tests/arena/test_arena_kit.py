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
# themed arenas existed (commit 256766b).
BANKED = {
    "psqarena": "61dd33e1df32eb038b4d4487abcc341831ccfa4524c1c73b4132f14765dd44eb",
    "psqterraces": "d472fb763146e2b5e3eb7e7d007761c81f8cc4ea4787a87521aabf4e9ae08db0",
    "psqtowers": "b73fa9001a74e211aee7616de874cd507b51ff8c4ab3472469dc4309050f11d1",
    "psqtesthall": "d25c3e970e971c0d2c63883302672f0ac0d345b68377bd93e592279c5281cbbe",
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
        self.brushes, self.liquids = [], []
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

    @staticmethod
    def _inside(planes, p):
        return all(K._dot(n, p) - d < -0.01 for n, d in planes)

    def solid(self, p):
        if not self.sp.open_at(*p):
            return True
        return any(self._inside(pl, p) for pl, _ in self.brushes)

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
