#!/usr/bin/env python3
"""Generate a themed psQuake arena (arena_maps.py): its .map and .wad.

    generate_map.py --pak data/librequake/id1/pak0.pak --arena psqtemple \\
        --map out/psqarena.map --wad out/psqarena.wad [--plan plan.svg] \\
        [--shots shots.txt] [--exit-map MAP]

The layout is data (arena_maps.py MAPS), built from the pieces of
arena_kit.py by arena_layout.py; the start room, escape and exit door are
generate_arena.py's. Textures are copied from LibreQuake's pak0.pak and each
is checked against a pinned SHA-256: generate_arena.py's own (the start
room's) and the ones listed here. --plan writes a top-down SVG with the
central area's share of the spawn cells; --shots the camera stands for
psq_labels_render --arena-shots.

Python 3 standard library only. Output is deterministic.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import arena_layout  # noqa: E402
import arena_maps  # noqa: E402
import generate_arena as G  # noqa: E402

# Textures the themed arenas add to generate_arena.TEXTURES (BSD-3 LibreQuake).
TEXTURES = {
    "wall_tan_c": ("maps/dev.bsp",
        "9b1033bce04b7e563b2de47cf9a37e72c949e68ee8163765c420bcf906347f27"),
    "wall_grey_c": ("maps/dev.bsp",
        "9d19a0148b6c55ccc8483ceedbb97f974d54156b3b077def25117f743d85678c"),
    "wall_grey_a": ("maps/dev.bsp",
        "3b70c706b967390807e51fa0d86a9ed672ba095bc4a368ab28c770e96015cf1c"),
    "wall_blue_a": ("maps/dev.bsp",
        "dd778d6a1d983fb935754f9f4d2e489d9dbae1bba36af8b45481636110d10bcc"),
    "wall_blue_b": ("maps/dev.bsp",
        "436d39c721a0d4ffb886d425ff19aeb56a5f93ee2df3166d8d82e851ff7a4a25"),
    "wall_orange_a": ("maps/dev.bsp",
        "5bd834e1afa858e5e79147cc5e4f4cdcbfdb0bba48cf634e98def21fe83e2c45"),
    "wall_red_a": ("maps/dev.bsp",
        "34a3c0ca1d81340a2e275f41710144b546b53160d17bdcb57e38013036cc934a"),
    "wall_green_a": ("maps/dev.bsp",
        "7b311e1d9c33724584ae276ca968fc498b62ae7ade7ac886b2f64b712655f546"),
    "wall_pink_a": ("maps/dev.bsp",
        "4c7345a3d1a53dbeb8efe805fcabb3f7b8c1f6f127d93f7d3a7e9a3ee2bdb749"),
    "sky_orng": ("maps/lq_e1m2.bsp",
        "adcaff516fc298a22a033a9068b05d1913a21b140071521d9e84ba5122af458a"),
}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--pak", required=True)
    ap.add_argument("--arena", required=True, choices=sorted(arena_maps.MAPS))
    ap.add_argument("--map", required=True)
    ap.add_argument("--wad", required=True)
    ap.add_argument("--plan")
    ap.add_argument("--shots")
    ap.add_argument("--exit-map", default=None)
    args = ap.parse_args(argv)
    try:
        if os.path.dirname(os.path.abspath(args.map)) != os.path.dirname(os.path.abspath(args.wad)):
            raise G.ArenaError("--map and --wad must be in the same folder")
        pinned = dict(G.TEXTURES)
        pinned.update(TEXTURES)
        names = sorted(pinned)
        found = G.collect_textures(args.pak, names)
        bad = []
        for n in names:
            src, data = found[n]
            if (src, hashlib.sha256(data).hexdigest()) != pinned[n]:
                bad.append(n)
            if n in G.FRAMED and struct.unpack_from("<II", data, 16) != G.FRAMED[n]:
                bad.append(f"{n} (size)")
        if bad:
            raise G.ArenaError("texture hash mismatch: " + ", ".join(bad))
        L = dict(arena_maps.MAPS[args.arena])
        if args.exit_map:
            L["exit_map"] = args.exit_map
        world, ents, D = arena_layout.build(L)
        text = G.map_text(world, ents, os.path.basename(args.wad), D["title"], D["worldkeys"])
        if args.plan:
            G._write_atomic(args.plan, arena_layout.plan_svg(D).encode("utf-8"))
        if args.shots:
            G._write_atomic(args.shots, G.shots_text(D).encode("ascii"))
        G._write_atomic(args.wad, G.wad2({n: found[n][1] for n in names}))
        G._write_atomic(args.map, text.encode("ascii"))
    except (G.ArenaError, arena_layout.LayoutError, OSError) as e:
        print(f"generate_map: {e}", file=sys.stderr)
        return 1
    except Exception as e:   # a kit error names the piece
        print(f"generate_map: {type(e).__name__}: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
