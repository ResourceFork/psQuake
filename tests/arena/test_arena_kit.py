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


class Stairs(unittest.TestCase):
    """Every stair run of every themed map is entered from open floor at its
    low end and left onto open floor with headroom at its high end, in
    steps within the player's step height; one rising out of water starts
    where the water is shallow enough to walk in (under the waist)."""
    HEAD = 72       # the player is 56 tall: room to walk off the top
    STEP = 18
    WAIST = 28      # SV_CheckWater: over this deep the player swims

    def test_stairs_enter_and_leave(self):
        bad = []
        for name, L in arena_maps.MAPS.items():
            _, _, D = arena_layout.build(dict(L))
            sp = D["space"]
            solids = [b for kind, b in L.get("world_pieces", []) if kind == "box"]

            def floor_at(x, y, z):
                # solid just under z: uncarved, or a world box
                if not sp.open_at(x, y, z - 4):
                    return True
                return any(b["lo"][0] <= x <= b["hi"][0] and b["lo"][1] <= y <= b["hi"][1]
                           and b["lo"][2] <= z - 4 <= b["hi"][2] for b in solids)
            for s in L.get("stairs", []):
                n = s["steps"] if s.get("kind") == "spiral" else (s["high"] - s["low"]) // 16
                if n < 1 or (s["high"] - s["low"]) % n or (s["high"] - s["low"]) // n > self.STEP:
                    bad.append(f"{name} {s.get('rect', s.get('centre'))}: steps over {self.STEP}")
                def walkable(x, y, z):
                    return all(sp.open_at(x, y, z + dz) for dz in range(1, self.HEAD, 8)) \
                        and floor_at(x, y, z + 1)
                low, high = arena_layout.stair_ends(s)
                if not walkable(*low):
                    bad.append(f"{name} {s.get('rect', s.get('centre'))}: its low end at "
                               f"({low[0]:g} {low[1]:g} {low[2]:g}) meets a wall or no floor")
                if not any(walkable(*p) for p in [high] + arena_layout.stair_top_sides(s)):
                    bad.append(f"{name} {s.get('rect', s.get('centre'))}: its top at "
                               f"({high[0]:g} {high[1]:g} {high[2]:g}) leads into a wall or "
                               "onto no floor, ahead or to either side")
                if s.get("from_water"):
                    depth = s["from_water"] - s["low"]
                    if depth > self.WAIST - 4:
                        bad.append(f"{name}: the water at {s.get('rect', s.get('centre'))} is "
                                   f"{depth} deep, the player swims there")
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
