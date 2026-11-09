#!/usr/bin/env python3
"""Tests for tools/validate_bsp.py.

    test_validate_bsp.py <tools dir> [<compiled psqarena.bsp>]

Without a compiled arena: malformed and hostile inputs are FAILs, never a
traceback, and the entity parser rejects malformed lumps. With one: the
arena passes; edited copies (wrong exit, two psq_arena, a missing skill pad,
an extra changelevel, items or the start on the grid, a leak file) fail the
right check; and a few hundred random byte corruptions never crash it.
"""

import contextlib
import io
import os
import random
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.abspath(sys.argv.pop(1)))
ARENA = sys.argv.pop(1) if len(sys.argv) > 1 else ""
if ARENA and not os.path.exists(ARENA):
    # A build that made the arena must test it: never skip it silently.
    sys.exit(f"test_validate_bsp: the compiled arena {ARENA} does not exist (build first)")
import validate_bsp as vb  # noqa: E402


def run(data, map_path=None, exit_map="lq_e1m1"):
    """Validate data; return (failed check ids, output)."""
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "psqarena.bsp")
        with open(p, "wb") as f:
            f.write(data)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            r = vb.validate(p, map_path, exit_map, 77, 96)
    return set(r.failed), out.getvalue()


def build(lumps):
    """A BSP29 from {lump name: bytes}."""
    header = struct.pack("<i", 29)
    body = b""
    ofs = 4 + 15 * 8
    for name in vb.LUMP_NAMES:
        data = lumps.get(name, b"")
        header += struct.pack("<ii", ofs + len(body), len(data))
        body += data
    return header + body


def with_entities(data, text):
    """data with its entity lump replaced (appended at the end)."""
    out = bytearray(data)
    struct.pack_into("<ii", out, 4, len(data), len(text) + 1)
    return bytes(out) + text.encode("latin-1") + b"\0"


class Malformed(unittest.TestCase):
    def test_short_and_wrong_version(self):
        for data in (b"", b"\x1d\0\0\0", bytes(50)):
            failed, out = run(data)
            self.assertEqual(failed, {"format"}, out)
        failed, out = run(struct.pack("<i", 30) + bytes(120))
        self.assertIn("version 30", out)

    def test_lump_outside_file(self):
        data = bytearray(build({"planes": bytes(20)}))
        struct.pack_into("<ii", data, 4 + 8, 100, 10 ** 6)
        failed, out = run(bytes(data))
        self.assertEqual(failed, {"format"}, out)
        struct.pack_into("<ii", data, 4 + 8, -5, 20)
        self.assertEqual(run(bytes(data))[0], {"format"})

    def test_partial_records(self):
        failed, out = run(build({"planes": bytes(21)}))
        self.assertEqual(failed, {"format"}, out)
        self.assertIn("not whole records", out)

    def test_empty_bsp_fails_without_crash(self):
        failed, out = run(build({}))
        self.assertIn("geometry", failed)

    def test_node_cycle(self):
        plane = struct.pack("<4fi", 1, 0, 0, 0, 0)
        node = struct.pack("<i2h6h2H", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        model = struct.pack("<9f4i3i", *([0.0] * 9), 0, 0, 0, 0, 0, 0, 0)
        failed, out = run(build({"planes": plane, "nodes": node, "models": model,
                                 "entities": b"{\n}\n\0"}))
        self.assertIn("format", failed)
        self.assertIn("cycle", out)

    def test_bad_texture_lump(self):
        lump = struct.pack("<i", 1000)
        failed, out = run(build({"textures": lump, "faces": bytes(20),
                                 "texinfo": bytes(40)}))
        self.assertIn("format", failed)

    def test_huge_texture_count(self):
        lump = struct.pack("<i", 5000) + bytes(4 * 5000)
        failed, out = run(build({"textures": lump, "faces": bytes(20),
                                 "texinfo": bytes(40)}))
        self.assertIn("format", failed)
        self.assertIn("at most", out)

    def test_entity_lump_stops_at_first_error(self):
        # Megabytes of '{': parsing must stop at the second one, not
        # tokenize the whole lump first.
        with self.assertRaisesRegex(vb.BspError, "nested"):
            vb.parse_entities("{" * (3 * 1024 * 1024))
        # Over the cap, the lump is refused before parsing.
        bsp = vb.Bsp(build({"entities": b"{" * (5 * 1024 * 1024)}))
        r = vb.Report()
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertIsNone(vb._check_entities(r, bsp, "lq_e1m1", 77, 96))
        self.assertEqual(r.failed, ["entities"])
        self.assertIn("over", out.getvalue())

    def test_entity_parser(self):
        self.assertEqual(vb.parse_entities('{\n"a" "b"\n}\n{\n}\n'), [{"a": "b"}, {}])
        for bad in ('{ {', '}', '{ "a" }', '"a" "b"', '{ "a" "b"', '{ x }'):
            with self.assertRaises(vb.BspError, msg=bad):
                vb.parse_entities(bad)


@unittest.skipUnless(ARENA, "no arena was built (configure warned why)")
class CompiledArena(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(ARENA, "rb") as f:
            cls.data = f.read()
        bsp = vb.Bsp(cls.data)
        cls.ents_text = bsp.lump("entities").split(b"\0", 1)[0].decode("latin-1")

    def edited(self, old, new, count=1):
        self.assertEqual(self.ents_text.count(old), count, old)
        return with_entities(self.data, self.ents_text.replace(old, new))

    def test_arena_passes(self):
        failed, out = run(self.data)
        self.assertEqual(failed, set(), out)

    def test_wrong_exit_map(self):
        failed, out = run(self.data, exit_map="e1m1")
        self.assertEqual(failed, {"entities"}, out)

    def test_exit_with_intermission(self):
        data = self.edited('"map" "lq_e1m1"\n"spawnflags" "1"', '"map" "lq_e1m1"\n"spawnflags" "0"')
        self.assertEqual(run(data)[0], {"entities"})

    def test_two_arenas(self):
        data = with_entities(self.data, self.ents_text +
                             '{\n"classname" "psq_arena"\n"origin" "0 0 0"\n}\n')
        self.assertEqual(run(data)[0], {"entities"})

    def test_wrong_grid(self):
        data = self.edited('"psq_cols" "11"', '"psq_cols" "10"')
        self.assertEqual(run(data)[0], {"entities"})
        data = self.edited('"psq_cell" "96"', '"psq_cell" "64"')
        self.assertEqual(run(data)[0], {"entities"})
        data = self.edited('"psq_rows" "7"', '"psq_rows" "seven"')
        self.assertEqual(run(data)[0], {"entities"})

    def test_missing_skill(self):
        data = self.edited('"message" "3"', '"message" "2"')
        self.assertEqual(run(data)[0], {"entities"})

    def test_extra_changelevel(self):
        data = with_entities(self.data, self.ents_text +
                             '{\n"classname" "trigger_changelevel"\n"map" "start"\n'
                             '"spawnflags" "1"\n"model" "*1"\n}\n')
        self.assertEqual(run(data)[0], {"entities"})

    def test_bad_model_reference(self):
        data = self.edited('"model" "*1"', '"model" "*99"')
        self.assertEqual(run(data)[0], {"entities"})

    def test_item_on_grid(self):
        data = self.edited('"origin" "656 0 16"', '"origin" "96 0 16"')
        self.assertEqual(run(data)[0], {"pickups"})

    def test_item_box_reaches_grid(self):
        # The origin is off the grid, but a 32-unit pickup box reaches it.
        data = self.edited('"origin" "-656 0 16"', '"origin" "-550 0 16"')
        self.assertEqual(run(data)[0], {"pickups"})

    def test_start_on_grid(self):
        data = self.edited('"origin" "0 -896 24"', '"origin" "0 -96 24"')
        self.assertEqual(run(data)[0], {"start"})

    def test_start_in_wall(self):
        data = self.edited('"origin" "0 -896 24"', '"origin" "0 -1000 24"')
        self.assertEqual(run(data)[0], {"start"})

    def test_leak_file_beside_map(self):
        with tempfile.TemporaryDirectory() as d:
            m = os.path.join(d, "psqarena.map")
            open(m, "w").close()
            self.assertEqual(run(self.data, map_path=m)[0], set())
            open(os.path.join(d, "psqarena.pts"), "w").close()
            self.assertEqual(run(self.data, map_path=m)[0], {"leak"})

    def test_truncated(self):
        self.assertEqual(run(self.data[: len(self.data) // 2])[0], {"format"})

    def test_random_corruption_never_crashes(self):
        rng = random.Random(1996)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "psqarena.bsp")
            for _ in range(300):
                data = bytearray(self.data)
                for _ in range(rng.randint(1, 8)):
                    i = rng.randrange(len(data))
                    data[i] = rng.randrange(256)
                if rng.random() < 0.2:
                    # Hit the header and lump table, where offsets live.
                    i = rng.randrange(4, 4 + 15 * 8)
                    data[i] = rng.randrange(256)
                with open(p, "wb") as f:
                    f.write(data)
                with contextlib.redirect_stdout(io.StringIO()):
                    vb.validate(p, None, "lq_e1m1", 77, 96)  # must not raise


if __name__ == "__main__":
    unittest.main(verbosity=2)
