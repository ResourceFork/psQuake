#!/usr/bin/env python3
"""Generate psqtesthazards, the hazard test map: its .map and .wad.

A test-only map (never shipped): one tall sealed hall with every hazard the
environment can kill a process monster with, for the headless hazard tests
(src/app/macos/psq_headless_test.cmake, psq_headless_hazards_*). Its name
starts with "psq", so process monsters come to it (psq_map_allows_processes)
without being a psQuake arena: no grid, start room or resupply.

    pool   x centre  what
    lava   -768      *lava1, 248 deep
    slime  -384      *slime1, 248 deep
    water     0      *water0, 248 deep: a monster on the bottom drowns
    pit     384      256 deep, a trigger_hurt (dmg 1000) on its floor
    crush   768      a func_door ("targetname" crusher) that comes down on
                     whatever stands under it (dmg 1000)

Each pool is 256 x 256, centred on y = 0. The floor is at 0 and the ceiling
at 1024, so a monster set 900 up over the floor (y 384) takes a long fall.
The positions are the driver's (tests/psq_headless_driver.c, HZ_*).

    generate_testhazards.py --pak data/librequake/id1/pak0.pak \\
        --map out/psqarena.map --wad out/psqarena.wad

Python 3 standard library only. Output is deterministic.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate_arena as G  # noqa: E402  (box, map_text, the pak reader, wad2)

TEXTURES = {
    "floor_grey_c": ("maps/dev.bsp",
        "d9967b81011af58e0b7f74ef7f9cc0b3fe86a7e65bf4f3b8d7d5a8227c428e31"),
    "trigger": ("maps/dev.bsp",
        "0ecfb8fa2ae69ecd3f0fa9f15f70400d0459f5dceb8c3e560a2c0bba19ba13aa"),
    "*lava1": ("maps/lq_e0m8.bsp",
        "602982a61f9abcb31dc48b2df8a79805d9d14486e0f62262a9058bf6636a46db"),
    "*slime1": ("maps/dev.bsp",
        "22ba6cfc7666772a2d266f60562fe706c5c619b552ed7f02ea73da8a25e4008e"),
    "*water0": ("maps/lq_e0m1.bsp",
        "39c532a01319c03df02ac3ab79f83ff1ee728f25045ff3eadc1b9f0e3e4ee92e"),
}
TX = "floor_grey_c"
HALL = (-1024, -512, 0, 1024, 512, 1024)
POOL_HALF = 128
POOL_DEPTH = 256
POOLS = (("*lava1", -768), ("*slime1", -384), ("*water0", 0), (None, 384))
CRUSH_X = 768


def build():
    x0, y0, z0, x1, y1, z1 = HALL
    t = 16
    w = []
    # The floor round the pools: strips between and beside them.
    edges = [x0 - t]
    for _, cx in POOLS:
        edges += [cx - POOL_HALF, cx + POOL_HALF]
    edges.append(x1 + t)
    for a, b in zip(edges[0::2], edges[1::2]):
        w.append(G.box(a, y0 - t, z0 - t, b, y1 + t, z0, TX))
    for _, cx in POOLS:
        a, b = cx - POOL_HALF, cx + POOL_HALF
        w.append(G.box(a, y0 - t, z0 - t, b, -POOL_HALF, z0, TX))
        w.append(G.box(a, POOL_HALF, z0 - t, b, y1 + t, z0, TX))
        # the pool's walls and floor
        zb = z0 - POOL_DEPTH
        w.append(G.box(a, -POOL_HALF, zb - t, b, POOL_HALF, zb, TX))
        w.append(G.box(a - t, -POOL_HALF, zb, a, POOL_HALF, z0 - t, TX))
        w.append(G.box(b, -POOL_HALF, zb, b + t, POOL_HALF, z0 - t, TX))
        w.append(G.box(a, -POOL_HALF - t, zb, b, -POOL_HALF, z0 - t, TX))
        w.append(G.box(a, POOL_HALF, zb, b, POOL_HALF + t, z0 - t, TX))
    for tex, cx in POOLS:
        if tex:
            w.append(G.box(cx - POOL_HALF, -POOL_HALF, z0 - POOL_DEPTH, cx + POOL_HALF,
                           POOL_HALF, z0 - 8, tex))
    w.append(G.box(x0 - t, y0 - t, z1, x1 + t, y1 + t, z1 + t, TX))
    w.append(G.box(x0 - t, y0 - t, z0, x0, y1 + t, z1, TX))
    w.append(G.box(x1, y0 - t, z0, x1 + t, y1 + t, z1, TX))
    w.append(G.box(x0, y0 - t, z0, x1, y0, z1, TX))
    w.append(G.box(x0, y1, z0, x1, y1 + t, z1, TX))
    pit = POOLS[3][1]
    ents = [
        ({"classname": "info_player_start", "origin": "-900 -400 24", "angle": "0"}, []),
        ({"classname": "trigger_hurt", "dmg": "1000"},
         [G.box(pit - POOL_HALF, -POOL_HALF, z0 - POOL_DEPTH, pit + POOL_HALF, POOL_HALF,
                z0 - POOL_DEPTH + 32, "trigger")]),
        # 480 tall, its foot 96 up; used, it comes down 96 to the floor.
        ({"classname": "func_door", "targetname": "crusher", "angle": "-2", "speed": "400",
          "dmg": "1000", "wait": "-1", "lip": "384"},
         [G.box(CRUSH_X - POOL_HALF, -POOL_HALF, 96, CRUSH_X + POOL_HALF, POOL_HALF, 576, TX)]),
    ]
    for x in (-768, 0, 768):
        for y in (-256, 256):
            ents.append(({"classname": "light", "origin": f"{x} {y} 600", "light": "500"}, []))
    return w, ents


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--pak", required=True)
    ap.add_argument("--map", required=True)
    ap.add_argument("--wad", required=True)
    args = ap.parse_args(argv)
    try:
        found = G.collect_textures(args.pak, sorted(TEXTURES))
        bad = [n for n, (src, data) in found.items()
               if (src, hashlib.sha256(data).hexdigest()) != TEXTURES[n]]
        if bad:
            raise G.ArenaError("texture hash mismatch: " + ", ".join(sorted(bad)))
        world, ents = build()
        text = G.map_text(world, ents, os.path.basename(args.wad), "psQuake hazard test")
        G._write_atomic(args.wad, G.wad2({n: d for n, (_, d) in found.items()}))
        G._write_atomic(args.map, text.encode("ascii"))
    except (G.ArenaError, OSError) as e:
        print(f"generate_testhazards: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
