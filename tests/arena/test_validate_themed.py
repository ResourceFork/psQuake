#!/usr/bin/env python3
"""Tests for the themed arenas' checks in tools/validate_bsp.py.

    test_validate_themed.py <tools dir> <compiled psqtemple.bsp>

The Temple passes central, exits, levels, sightline, drops, sky, water and
framed; edited entities (the central area too small, holding too few cells,
moved under a roof; the sightline blocked, too short) and a walk with only
two floors fail the right check.
"""

import contextlib
import io
import os
import sys
import unittest

sys.path.insert(0, os.path.abspath(sys.argv.pop(1)))
TEMPLE = sys.argv.pop(1)
if not os.path.exists(TEMPLE):
    sys.exit(f"test_validate_themed: {TEMPLE} does not exist (build first)")
import validate_bsp as vb  # noqa: E402

with open(TEMPLE, "rb") as f:
    BSP = vb.Bsp(f.read())
ENTS = vb.parse_entities(BSP.lump("entities").decode("latin-1"))
ENTRY = vb.vec3(ENTS[0]["_psq_hall_entry"], "entry")
RULES = ENTS[0]["_psq_rules"].split()


def themed(ents, rules=RULES):
    r = vb.Report()
    with contextlib.redirect_stdout(io.StringIO()):
        vb._check_themed(r, BSP, ents, ENTRY, rules, 96)
    return set(r.failed)


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
