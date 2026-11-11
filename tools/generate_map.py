#!/usr/bin/env python3
"""Generate a themed psQuake arena (arena_maps.py): its .map and .wad.

    generate_map.py --pak data/librequake/id1/pak0.pak --arena psqtemple \\
        --map out/psqarena.map --wad out/psqarena.wad [--plan plan.svg] \\
        [--shots shots.txt] [--exit-map MAP]

The layout is data (arena_maps.py MAPS), built from the pieces of
arena_kit.py by arena_layout.py; the start room, escape and exit door are
generate_arena.py's. Textures are copied from LibreQuake's pak0.pak (or,
pinned as "pak1:...", its pak1.pak beside it) and each is checked against a
pinned SHA-256: generate_arena.py's own (the start
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
    # Temple
    "med_csl_brk10": ("maps/lq_e1m2.bsp",
        "520ff3bf85177bf646c01246a5fffaeecddb2f491a94fc80fcbe6231b621f6ed"),
    "med_csl_brk17": ("maps/lq_e1m3.bsp",
        "6e6dbcad91c6501f1e8daf2b2091cd69093ce9c849b8ab61772501954e6e26c0"),
    "med_csl_brk9_1": ("maps/lq_e1m7.bsp",
        "5dc4afcbf7821ccce09caa5d23ca825ba12c7cc9c1b07c3c23e4d0434a6e17bc"),
    "may_brk1_1": ("maps/lq_e0m5.bsp",
        "dc4a319dd769bccfa8ab422f19a6fc2be99c2cd7e88fbd3b4e593d0917e44cae"),
    "may_bnd1_2": ("maps/lq_e0m5.bsp",
        "a9a10c9e916a2c987922e6bc1ddeed485d7242bccd901a3cc0bd129583251ae7"),
    "may_bnd_skull": ("maps/lq_e0m5.bsp",
        "a099eb91c6276356e5929bdd06e41f97bb93f1a37c441698c9a5a4690e13d502"),
    "med_etrim1": ("maps/lq_e0m4.bsp",
        "6aef9e1d2163e9e0cdde78d1ae986fdba64116039d9bccb06700a6915ac4f0bf"),
    "med_csl_flr1_1": ("maps/lq_e0m2.bsp",
        "a847f169c4b2dface9a2afc12d3576bdf8426e7e6c3adcf39ab6d7714170a369"),
    "med_csl_flr4_1": ("maps/lq_e0m2.bsp",
        "316d071a4c84da7c7fdf7e9b21d3a9107934fe944201bdae290d35cde2bec7f8"),
    "med_csl_flr5_1": ("maps/lq_e1m3.bsp",
        "0529847c77c47859b24db5a05ec232c5fc30a2e4f27c83b527b83124ed7cc104"),
    "grk_flr4_2": ("maps/lq_e0m6.bsp",
        "76a287e214dae12017e8b6a49eef6055947e396332adcddfe56e73af3355893d"),
    "med_csl_stp1": ("maps/lq_e1m1.bsp",
        "67278feff98bc8fc35a3c492a329840f7278199ae482d73d442a7095280549b1"),
    "may_trm1_1": ("maps/lq_e0m5.bsp",
        "5753dc4bde668918be4614592b69d6c7bb7d9f6afeb2ba9f1e09b34e9e482646"),
    "may_deco1_1": ("maps/lq_e0m5.bsp",
        "6c4e524978a2950fa9b10b2ac83a830535133cd6e7ef0bc3a6e16f85a2540a44"),
    "may_plr1_1": ("maps/lq_e0m8.bsp",
        "9f064f92a40b969d83304e25bbb23137da67ff2fecaf790e154e6749a28dd9a4"),
    "met_rune_trim32": ("maps/lq_e1m2.bsp",
        "75657d973b0f9c3afbdd5ef2101d00a098724979cd664c073ceb9fdad5590fdb"),
    "may_tskull": ("maps/lq_e0m5.bsp",
        "c61ddb9e8c187bcf3d09db6191d92e9bb382fe06cf6153d043ff622d6cfd11b8"),
    "med_glass1": ("maps/lq_e1m2.bsp",
        "85b4e08fa1eb1b2f52f4d4ab67a568ae4830fe1e512fa9f061302b195ca4bc28"),
    "med_glass2": ("maps/lq_e0m4.bsp",
        "11151b365e22d85b5bfc6b11f817914456c304330dbd5cad850282519b940efc"),
    "med_glass3": ("maps/lq_e0m4.bsp",
        "67b3de110e1b30cbaa8c50db3f54ac54cfa1891e54792ceb906387e49344c2d8"),
    "med_block_1a": ("maps/lq_e1m4.bsp",
        "8ffb7e4fdb2f1c1c7022cdfb3f186112fd847e5856a23e67cbda1153c042a98d"),
    "may_lite1_1": ("maps/lq_e0m8.bsp",
        "e42936b420b1178235bcc6567391d017de1f3dc51f5e92590accbe35d1888f57"),
    "may_lite2_1": ("maps/lq_e0m8.bsp",
        "6f106774f369d19d6e4e487005cdb29fb116673d1b8e95557323ef147c25c166"),
    "med_brk9_ceil1a": ("maps/lq_e1m7.bsp",
        "71b2eec3c6e27fa20620bf463a627cb3c3167985bfd6c1bca6e9bdab7c2abf89"),
    "grk_arch_trim": ("maps/start.bsp",
        "b6161cd3d96fa512163df6bcc5336c6112161b6816e6194fd3afcbeaced85941"),
    # Ruins
    "grass1": ("maps/lq_e1m7.bsp",
        "03d042e43ee80086f2a3ae91f244943517d6ef9781d3276b9fc287278d063154"),
    "med_cobstn1_1": ("maps/lq_e1m4.bsp",
        "b7565526b026a3315fdb83be33cfb7da597c15d744f740f63d90b2ef69df8ff2"),
    "med_cobstn2_1a": ("maps/lq_e1m4.bsp",
        "9d3c502725f1c904149ab69f98f077684202ff0f0bbb12e93d9fb9d484d7c62e"),
    "med_rock4": ("maps/lq_e0m8.bsp",
        "7450ef14322095679d58c39449d3d565689dd71469f36c728ae5db4f30b99330"),
    "grk_bl_trim1": ("maps/lq_e0m6.bsp",
        "a7fe663124bcb230088ea30f126e7b6881813ae7afb05dce2b89c0cb413ac940"),
    "grk_bl_wll2": ("maps/lq_e0m6.bsp",
        "8e2a48069b922d62f1c358507cc3fc82dd6f4ac56227faa41d5cc9308b3aba66"),
    "grk_bl_flt1": ("maps/lq_e0m6.bsp",
        "fe682b6e510bbd19d0840fb8bd207a836d19858cc287dfd0988cdbedc78ba93a"),
    "grk_leaf1_1": ("maps/lq_e0m6.bsp",
        "a84e83049cafc33883b6cad7d3c6758fcfbff4cee8e30ff67f15c4f03b3a4b67"),
    "vines1": ("maps/lq_e1m1.bsp",
        "a2b56356c4b3cb736fe12181cb1fa4597c8a6131d0f5aea0085994fd19b0417e"),
    "woodbark64": ("maps/lq_e1m7.bsp",
        "422d3186ca6c8f770f76825dea6952785bc03b46409d5a4c88f0c4c756e94539"),
    "sky5_blu": ("maps/lq_e0m1.bsp",
        "404ecb11d9bed52f6cd1b2f04ca2df85171429a2debf7ad22c1b552fc7a86a67"),
    "*water0": ("maps/lq_e0m1.bsp",
        "39c532a01319c03df02ac3ab79f83ff1ee728f25045ff3eadc1b9f0e3e4ee92e"),
    "med_rock9": ("maps/lq_e0m2.bsp",
        "a5e8753d229d35cce45efd82ee14497ae510ff2d4faa163e3c54cafce512f70e"),
    "rocks11d": ("maps/lq_e1m1.bsp",
        "ca22c92904f1d37a387c8b0e27828a45b4e860932cf39f8367eb6a2604254aa8"),
    "gardgrass_1": ("pak1:maps/lq_e3m3.bsp",
        "f658a25e906a4a779a52eccdbf89a5b3ff6d6c93dbc2b0caeb7b50c92df3891c"),
    "med_rock5": ("pak1:maps/lq_e3m2.bsp",
        "da11e738b46968b8d84a2ab1fc5284294cddeacd6de495994049b48eeaa3a89c"),
    "med_rock5_g2": ("pak1:maps/lq_e3m2.bsp",
        "f0b4caa917d7decd14490f5d654a93ba64ad973bf55829ea1d45872d63ab3acc"),
    "rock1_2": ("pak1:maps/lq_e4m3.bsp",
        "77bdb561ccc6fc9612dc33e1c9b2bb4feba78a500e998bfd63e2f44fc76f3643"),
    "grk_bl_brk1a": ("pak1:maps/lq_e3m6.bsp",
        "83a0fcd9c73edb8e1d552d804338f1c7889ad50049322407f6ce5d82e73e5b03"),
    "grk_bl_brk2a": ("pak1:maps/lq_e3m6.bsp",
        "b712bfba1ce8b8d86e3558e8f6e75bf975d7e80192ee41c2bcfe4fa124bb4434"),
    "grk_bl_pil1": ("pak1:maps/lq_e3m6.bsp",
        "e5a1772798dfe50c6c4711a152c732e1852985c68ba6002832ea8904db495355"),
    "darkrock": ("pak1:maps/lq_e3m3.bsp",
        "6c4dd84e0ddc000fd21c66a35f9789d2a56347da640d8d86dfb00548d2425ca7"),
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
        # A source "pak1:maps/x.bsp" is in pak1.pak, beside --pak.
        in1 = [n for n in names if pinned[n][0].startswith("pak1:")]
        found = G.collect_textures(args.pak, [n for n in names if n not in in1])
        if in1:
            pak1 = os.path.join(os.path.dirname(os.path.abspath(args.pak)), "pak1.pak")
            for n, (m, data) in G.collect_textures(pak1, in1).items():
                found[n] = ("pak1:" + m, data)
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
