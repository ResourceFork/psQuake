#!/usr/bin/env python3
"""Tests for the themed arenas' checks in tools/validate_bsp.py.

    test_validate_themed.py <tools dir> <compiled psqtemple.bsp> <psqtesthazards.bsp>

The Temple passes central, exits, levels, sightline, drops, sky, water and
framed; edited entities (the central area too small, holding too few cells,
moved under a roof; the sightline blocked, too short) and a walk with only
two floors fail the right check. On the hazard test map (a 256 pit, lava with no warm light, pools walled
in): drops, lavalight and exits fail, and water passes (its water reaches
within 8 of the floor all round). A framed texture shifted half a picture fails framed; a
sky face turned upright fails skywall. Crafted
values (a central area outside the map, a non-finite texinfo) fail and
never raise.
"""

import contextlib
import struct
import io
import os
import sys
import unittest

sys.path.insert(0, os.path.abspath(sys.argv.pop(1)))
TEMPLE = sys.argv.pop(1)
HAZARDS = sys.argv.pop(1)
for _p in (TEMPLE, HAZARDS):
    if not os.path.exists(_p):
        sys.exit(f"test_validate_themed: {_p} does not exist (build first)")
import validate_bsp as vb  # noqa: E402

with open(TEMPLE, "rb") as f:
    DATA = f.read()
BSP = vb.Bsp(DATA)
with open(HAZARDS, "rb") as f:
    HZ = vb.Bsp(f.read())
HZ_ENTS = vb.parse_entities(HZ.lump("entities").decode("latin-1"))
ENTS = vb.parse_entities(BSP.lump("entities").decode("latin-1"))
ENTRY = vb.vec3(ENTS[0]["_psq_hall_entry"], "entry")
RULES = ENTS[0]["_psq_rules"].split()


def themed(ents, rules=RULES, bsp=BSP, entry=ENTRY):
    r = vb.Report()
    with contextlib.redirect_stdout(io.StringIO()):
        try:
            vb._check_themed(r, bsp, ents, entry, rules, 96)
        except vb.BspError as e:   # validate() reports it as a format FAIL
            r.check("format", False, str(e))
    return set(r.failed)


def framed_shifted():
    """The Temple with every telepad1 texinfo's s offset moved 32."""
    data = bytearray(DATA)
    mip = BSP.miptex()
    o, _ = BSP.lumps["texinfo"]
    for i, t in enumerate(BSP.texinfo):
        if 0 <= t[8] < len(mip) and mip[t[8]][0] == "telepad1":
            off = o + 40 * i + 12
            data[off:off + 4] = struct.pack("<f", t[3] + 32)
    return vb.Bsp(bytes(data))


def edited(**keys):
    ents = [dict(e) for e in ENTS]
    ents[0].update(keys)
    return ents


class Themed(unittest.TestCase):
    def test_temple_passes(self):
        self.assertEqual(themed(ENTS), set())
        for rule in ("central", "levels", "sightline", "drops", "sky", "water", "framed"):
            self.assertIn(rule, RULES)

    def test_central_too_small(self):
        x0, y0, x1, y1, z = vb._central_rect(ENTS[0])
        self.assertIn("central", themed(edited(_psq_central=f"{x0} {y0} {x0 + 900} {y1} {z}"),
                                        ["central"]))

    def test_central_too_few_cells(self):
        ents = [dict(e) for e in ENTS]
        for k in range(4):   # 4 x 32 x 2 cells elsewhere: under 60% left in the middle
            ents.append({"classname": "psq_grid", "origin": f"0 {4000 + 300 * k} 0",
                         "psq_cols": "32", "psq_rows": "2", "psq_cell": "96"})
        self.assertIn("central", themed(ents, ["central"]))

    def test_sky_under_a_roof(self):
        # the nave (z 0, roofed) as if it were the central area
        self.assertIn("sky", themed(edited(_psq_central="-224 1376 224 1888 0"), ["sky"]))

    def test_sightline_blocked(self):
        self.assertIn("sightline", themed(edited(_psq_sightline="0 -48 -82 2000 -48 -82"),
                                          ["sightline"]))

    def test_sightline_too_short(self):
        self.assertIn("sightline", themed(edited(_psq_sightline="0 100 -82 0 600 -82"),
                                          ["sightline"]))

    def test_hazard_map_fails(self):
        start = (-900, -400, 24)
        ents = [dict(e) for e in HZ_ENTS]
        ents[0]["_psq_central"] = "-896 -128 -640 128 -256"   # the lava pool's floor
        got = themed(ents, ["water", "drops", "central"], HZ, start)
        # its pools are walled in below the floor; their water tops out 8
        # under it, so the player swims up and steps out on every side
        self.assertTrue({"drops", "exits"} <= got and "water" not in got, got)
        r = vb.Report()
        with contextlib.redirect_stdout(io.StringIO()):
            vb._check_lavalight(r, HZ, HZ_ENTS)
        self.assertEqual(r.failed, ["lavalight"])

    def test_framed_shifted(self):
        r = vb.Report()
        with contextlib.redirect_stdout(io.StringIO()):
            vb._check_framed(r, framed_shifted())
        self.assertEqual(r.failed, ["framed"])

    def test_skywall_upright(self):
        """The Temple's sky faces are all ceilings; one stood upright (its
        plane turned to face along x) fails skywall."""
        r = vb.Report()
        with contextlib.redirect_stdout(io.StringIO()):
            vb._check_skywall(r, BSP)
        self.assertEqual(r.failed, [])
        mip = BSP.miptex()
        sky = next(f for f in BSP.faces
                   if 0 <= f[4] < len(BSP.texinfo)
                   and mip[BSP.texinfo[f[4]][8]][0].startswith("sky"))
        data = bytearray(DATA)
        o, _ = BSP.lumps["planes"]
        nx, ny, nz, d, kind = BSP.planes[sky[0]]
        data[o + 20 * sky[0]:o + 20 * sky[0] + 20] = struct.pack("<4fi", 1.0, 0.0, 0.0, d, 0)
        r = vb.Report()
        with contextlib.redirect_stdout(io.StringIO()):
            vb._check_skywall(r, vb.Bsp(bytes(data)))
        self.assertEqual(r.failed, ["skywall"])

    def test_skywall_finds_sky_by_contents(self):
        """A sky brush's face is found by the sky behind it, not only by its
        texture: with the sky textures renamed, every sky face still counts."""
        def count(bsp):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                vb._check_skywall(vb.Report(), bsp)
            return out.getvalue()
        names = {m[0] for m in BSP.miptex() if m[0] and m[0].startswith("sky")}
        self.assertTrue(names)
        data = bytes(DATA)
        for n in names:
            data = data.replace(n.encode() + b"\0", b"x" + n[1:].encode() + b"\0")
        renamed = vb.Bsp(data)
        self.assertFalse(any(m[0].startswith("sky") for m in renamed.miptex() if m[0]))
        self.assertEqual(count(renamed), count(BSP))

    def test_crafted_values_fail(self):
        self.assertIn("format", themed(edited(_psq_central="-1e30 -1e30 1e30 1e30 0"),
                                       ["sky"]))
        self.assertIn("sightline", themed(edited(_psq_sightline="0 0 1e30 0 0 -1e30"),
                                          ["sightline"]))
        data = bytearray(DATA)
        o, _ = BSP.lumps["texinfo"]
        for i in range(len(BSP.texinfo)):
            data[o + 40 * i:o + 40 * i + 4] = struct.pack("<f", float("inf"))
        r = vb.Report()
        with contextlib.redirect_stdout(io.StringIO()):
            vb._check_framed(r, vb.Bsp(bytes(data)))
        self.assertEqual(r.failed, ["framed"])

    def test_hostile_bounds_and_leaves(self):
        # the world model's bounds made huge: a FAIL, not a hang or a traceback
        data = bytearray(DATA)
        o, _ = BSP.lumps["models"]
        data[o:o + 24] = struct.pack("<6f", -1e30, -1e30, -1e30, 1e30, 1e30, 1e30)
        self.assertIn("format", themed(ENTS, ["levels"], vb.Bsp(bytes(data))))
        # thousands of touching water leaves merge quickly, and too many fail
        class Many:
            def __init__(self, n):
                self.leafs = [(-3, 0, i * 8, 0, 0, i * 8 + 8, 8, 8) for i in range(n)]
        import time
        t = time.time()
        groups = vb._liquid_groups(Many(4000))
        self.assertEqual(len(groups), 1)
        self.assertLess(time.time() - t, 5)
        with self.assertRaises(vb.BspError):
            vb._liquid_groups(Many(vb.MAX_LIQUID_LEAVES + 1))

    def test_hostile_floor_face(self):
        # a floor face's vertices pushed far outside the world: routelight
        # skips it quickly, never loops over its huge extent
        data = bytearray(DATA)
        o, n = BSP.lumps["vertexes"]
        for i in range(0, n, 12):
            x, y, z = struct.unpack_from("<3f", data, o + i)
            if z < -100:
                struct.pack_into("<3f", data, o + i, x * 1e6, y * 1e6, z)
        import time
        t = time.time()
        themed(ENTS, ["routelight"], vb.Bsp(bytes(data)))
        self.assertLess(time.time() - t, 120)

    def test_levels_two(self):
        r = vb.Report()
        seen = {(x * 32, y * 32, h) for x in range(20) for y in range(10) for h in (24, 216)}
        with contextlib.redirect_stdout(io.StringIO()):
            vb._check_levels(r, seen)
        self.assertEqual(r.failed, ["levels"])
        r = vb.Report()
        with contextlib.redirect_stdout(io.StringIO()):
            vb._check_levels(r, seen | {(x * 32, y * 32, -104) for x in range(20)
                                         for y in range(10)})
        self.assertEqual(r.failed, [])


if __name__ == "__main__":
    unittest.main()
