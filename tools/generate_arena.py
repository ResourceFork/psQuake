#!/usr/bin/env python3
"""Generate a psQuake arena: its .map and the WAD2 of its textures.

The app ships four arenas (ARENAS). New Game opens psqarena, the Pit; its
north exit leads to psqterraces, Terraces, whose exit leads to psqtowers,
Towers, whose exit leads to the first campaign map. psqtesthall, the original
one-floor hall, is for the tests: `map psqtesthall` loads it, no exit leads
there. The three shipped arenas share a south start room: the player starts
there and jumps a lip into the hall, or takes the escape hallway round to a
balcony over the hall to drop or shoot from; their exit is behind a door the
player shoots open. No map sets the skill: New Game plays on Normal.
The test hall keeps the original antechamber and an open exit alcove.

Process monsters appear on floor grids of 96-unit cells (room for Quake's
largest walking hull, 64 x 64). The psq_arena point entity carries the first
grid for the QuakeC controller; each further grid, one per floor region of
the multi-level arenas, is a psq_grid point entity with the same keys:

    origin    grid centre, on the floor
    psq_cols  cells along x       psq_rows  cells along y
    psq_cell  cell size in units

No two grids overlap in plan. Pickups, cover and decoration stay off them.
The test hall has one 11 x 7 grid.

Textures are copied, as Quake miptex, out of LibreQuake's own maps in its
pak0.pak (BSD-3, see LibreQuake's docs/COPYING), so no texture is downloaded
and none is id Software's. Each copied texture is checked against a pinned
SHA-256. Each multi-level arena has its own theme (THEMES): banded walls,
ceiling, a floor and spawn-area inlay per level, trims; plus shared hazard
edges, tread plate, lamps and light panels, and LibreQuake's wall torches and
flames.

    generate_arena.py --pak data/librequake/id1/pak0.pak --arena psqarena \\
        --map build/arena/psqarena.map --wad build/arena/psqarena.wad \\
        [--plan plan.svg] [--shots shots.txt]

--plan writes a top-down SVG of the layout, coloured by height; --shots the
camera stands for psq_labels_render --arena-shots. Without --arena it writes
the test hall.

--break KIND writes a deliberately broken test hall, for the validator's
tests: missing-texture, leak, no-exit, cell-in-brush, cell-corner, pocket.

Python 3 standard library only (and arena_kit.py beside it, for its octagon).
Output is deterministic.
"""

from __future__ import annotations

import argparse
import hashlib
import math
import os
import struct
import sys

# --- Layout (Quake units; z is up, the floor is z = 0) ----------------------

GRID_COLS = 11
GRID_ROWS = 7
GRID_CELL = 96
GRID_HALF_X = GRID_COLS * GRID_CELL // 2  # 528
GRID_HALF_Y = GRID_ROWS * GRID_CELL // 2  # 336

WALL = 16
HALL = (-768, -576, 0, 768, 576, 256)          # interior x0 y0 z0 x1 y1 z1
ANTE = (-320, -992, 0, 320, -608, 192)         # south start room
EXIT = (-128, 608, 0, 128, 864, 160)           # north exit alcove
DOOR_HALF = 96                                 # doorway half-width
DOOR_TOP = 128

EXIT_MAP = "lq_e1m1"     # LibreQuake's first campaign map (start.bsp's episode 1 portal)

# Texture name -> (the first LibreQuake map in pak0, by name, that embeds it;
# SHA-256 of its miptex). maps/dev.bsp textures are LibreQuake's lq_dev (CC0).
TEXTURES = {
    "tek_flr3": ("maps/lq_e0m1.bsp",
        "9984aa1ba53122a010109f058a7957636b1fccff29724fe896d85b383418255a"),
    "met_brn_flat": ("maps/lq_e0m1.bsp",
        "807d7b2e40f2223c26c31bd1fc53ccb26e9ac4dcc515646529c0bd4d452ea5fb"),
    "met_blu_rect": ("maps/lq_e0m7.bsp",
        "8fa68bba3a279025b66607d036207c4c85d0ebd87d22dfce051696f4f11a2a46"),
    "met_blu_trim32": ("maps/lq_e0m7.bsp",
        "5face3b0781c101e09fd0beb391b4eeb483ef66830049667dfbac1b899dd4c01"),
    "met_gry_slat": ("maps/lq_e0m1.bsp",
        "595055c628e395831b8d9244ea18d8a7ea4e216592bec92bf5951b4d3f4c05ed"),
    "telepad1": ("maps/lq_e1m1.bsp",
        "9f4ea7893306b770448db5e281bfb7defc61cd617b83afdb6d8dce5111a9a4b0"),
    "tele_frame1": ("maps/lq_e1m7.bsp",
        "ee27ac0c99300a80ab83e7a80d3a4aad00b3c9c9d4776434d8fb48736934bce4"),
    "trigger": ("maps/dev.bsp",
        "0ecfb8fa2ae69ecd3f0fa9f15f70400d0459f5dceb8c3e560a2c0bba19ba13aa"),
    # Arena themes (generate_arena.py THEMES), copied the same way.
    "aqmetl07": ("maps/lq_e1m1.bsp",
        "8e654286c9d80fa37247e72165bef3e5af33398c15126060d00df7c54f2dca83"),
    "aqmetl28": ("maps/lq_e0m1.bsp",
        "556483089afe55713e8f436d4581d1a12429c7dde864b061ac6d78cddc7ce3fc"),
    "aqsupp01": ("maps/lq_e1m1.bsp",
        "3a85a7edd13803a9fc0827dadaeac8275bdf893407e362bcb943c3025f2909da"),
    "aqsupp03": ("maps/lq_e0m1.bsp",
        "dbb3eb39f68c8b3a6b66f9e9a73e0243b2f1c712ef2871c34cc4db9c28f38ec7"),
    "aqsupp06": ("maps/lq_e1m1.bsp",
        "49dc40ad43fc03d26b42ca982a71a29835c5b44ce98c5707b764454be53af65b"),
    "aqtrim01": ("maps/lq_e1m1.bsp",
        "681b7bf962291c45e3142b3518258c3c45a5a2fe3e1fb6e01fe28ae2fcc0b956"),
    "crate1_side": ("maps/lq_e1m1.bsp",
        "1fd9b3732dfe46c91161cc81b1e250a0bd38416578a737d29396da56290dd62d"),
    "crate1_top": ("maps/lq_e1m1.bsp",
        "de6d125c9ae6e8f6b17952ed0667452a24434575b4b093e72fed63b1e8e94029"),
    "met_blu_flat": ("maps/lq_e0m7.bsp",
        "80e3dfb3e663fad70a43be086bc8fedae7ce6f6c227317d17c40fdfcd8b888dd"),
    "met_blu_rivs": ("maps/start.bsp",
        "801f913faae83d05bf2231a25d2d58c46d88fb32fc01ccade77a432e28db3b52"),
    "met_blu_slat": ("maps/lq_e0m6.bsp",
        "c4a2781131d0b5610b6b542ff95747f6adb2b7f74aeb529575d70a6da8d262c4"),
    "met_blu_sqr": ("maps/lq_e0m7.bsp",
        "b00768900dad5ed7d817e9944f9c99ccbdacfd670bb873374909fc086350cc11"),
    "met_blu_stile": ("maps/start.bsp",
        "68b9248dae20b6cfe0685c6029c4b241bf669ac0d45884d0b2b3711a593b52bd"),
    "met_blu_trim16": ("maps/lq_e0m7.bsp",
        "9acc85f55c72fb326028d743e1e20743e2f06551f143c4c52949dc4dbee48932"),
    "met_blu_trim32s": ("maps/lq_e0m7.bsp",
        "3beef1b9f59dc9a9e1aaa884bed52ff99964125a44f5442963a15eccba1450fe"),
    "met_blu_trim64": ("maps/lq_e0m7.bsp",
        "ac062a05876e1bb4e1d1c6a7d294a575c9ad9e285642d1b3590b983e6c941ad7"),
    "met_brn_rect": ("maps/lq_e0m7.bsp",
        "ba4967bc5bfa7b9a0167223b7c07fa08e4194847d54fc67df7da6503d42b4fef"),
    "met_brn_stile": ("maps/lq_e1m2.bsp",
        "99a6448df956982f1de9fff4f0fef87e59a97bcc392ff9d68aeec6af74e70f8a"),
    "met_brn_tile2": ("maps/lq_e0m7.bsp",
        "aeb0e3b8c33acffe901e26f895db3517ff2e781e5f013d8a64cb627fb75b3bc5"),
    "met_brn_trim16": ("maps/lq_e0m7.bsp",
        "7c13cd4fede96a47d308f3c072ce88503317e090eb2ad57e7c010f1dae004101"),
    "met_brn_trim32": ("maps/lq_e1m2.bsp",
        "436885a8e16d211d08134087bb8ad586a140dba7bba45f769b9dd04f50276a4a"),
    "met_brn_trim64": ("maps/lq_e0m7.bsp",
        "c9f32ba338389c2934be49e4f87f4e7546e5cffcff7c9662a7387ca6569c5c48"),
    "met_dbrn_flat": ("maps/start.bsp",
        "ac0c5a55f15dfc38ffe8b2b0ef77db2f28c80e1a7659bd46de050b706d1876f4"),
    "met_grn_flat": ("maps/lq_e1m3.bsp",
        "cba63bfef100f79c6e4f743e0bedd0301e82ae5a7944451433769b2f968f17f6"),
    "met_grn_stile": ("maps/start.bsp",
        "e81558b555c7f1adb545418dad2c172aec8fccdf685730bd338f8d709c8026f7"),
    "met_lbrn_slat": ("maps/lq_e0m1.bsp",
        "a5b9e1a8c28d5e4d1ade87f87974b76394b612e4d21377b7685b2149ce9f3acf"),
    "met_mix_diam": ("maps/lq_e0m7.bsp",
        "e3b4e1bf9f7ab3cb82bae8e91aae0329969d90554bea1c570671f41ce640d55f"),
    "met_teal_flat": ("maps/lq_e1m2.bsp",
        "47d536efdeef944a7824094e475d577268996fed9928d34de9364c52a6d82d6d"),
    "met_teal_trim32": ("maps/lq_e1m2.bsp",
        "e3d59ad23c3b36a551af983715fc1265e5c04eff878d39c4c088ce804869c91d"),
    "met_teal_trim64": ("maps/lq_e1m2.bsp",
        "9720357ba5817d8582f01ccbb5f40ec52a3c28f10c3b09607e7128644207bfd5"),
    "metalstrip_1": ("maps/lq_e0m1.bsp",
        "7d60b45d478c3475bcefbdddf18160b72b524c5fdfdc6a41dcc740312f37b336"),
    "t_blok03": ("maps/lq_e1m1.bsp",
        "6c7719161079e4ae3298f35a6f995abdb1ebd19997db0caa6b1a8fb8d0a02757"),
    "t_blok04": ("maps/lq_e1m1.bsp",
        "479d0dcec7b0425de32a5c09c5a2490d6fc61495415a9deb623c181b1b514bc7"),
    "t_flor1a": ("maps/lq_e1m1.bsp",
        "dfcfb02e7ec2a96766d79492f32b70b9b05a42c1033cf32313ab7089fc8eb2ca"),
    "t_flor1b": ("maps/lq_e1m1.bsp",
        "96eed00bb0e03a2e8ea0299b4f84156c2f47d0c8031cad3d332e24fd43f7c607"),
    "t_flor2a": ("maps/lq_e1m1.bsp",
        "1bb0c9f95d17cb10d878ae2401dcd5ab2668c303a95b9890e065571937500653"),
    "t_lit07": ("maps/lq_e1m1.bsp",
        "c503c2746893457eee0cd1843a7d28372579a7fd98df386db065f472a6c97f28"),
    "t_metalsheetc": ("maps/lq_e1m1.bsp",
        "d50cb432de1da35a3d1c53f956d6d7f36614085fead5d32e10cc12459339caf8"),
    "t_trim1c": ("maps/lq_e1m1.bsp",
        "2ac88fdf04ef6d174f905921627cd60edf781737c19e6681fcab5c0149d3ebbf"),
    "t_trim1ca": ("maps/lq_e0m1.bsp",
        "dec09be3d9076a7952fd2109730a95f3f66660c326b54e2b780e6c8e5f501097"),
    "t_trim2aa": ("maps/lq_e0m1.bsp",
        "b2b781e23d73e7bff07bd34df8e597eb7c28a1cfe9d0f603c661345cb9fe6b28"),
    "t_wall1b": ("maps/lq_e1m1.bsp",
        "eff77f66edee06d3c950ceafe71e96435f88b9de20150ce8478bd55f0d249928"),
    "t_wall2b": ("maps/lq_e0m1.bsp",
        "0e04b714f120774304c0760e656526d47c0330e2e90fd8a6bf86d9fa8ede59ba"),
    "t_wall3a": ("maps/lq_e0m1.bsp",
        "1ea9b38fb2eb152a44aa25858609d5ca889ac8de744f53dd1826bd9ad9c1e645"),
    "tek_grate": ("maps/lq_e1m1.bsp",
        "3cca32b1d188c20a4f7e1de3b24dae4a5eee8000da9bbe0ee06b34d78ed4a133"),
    "tek_lit1": ("maps/lq_e0m1.bsp",
        "831111eb1a618720f74bfba3810bcd6f39aec149e213a162475ecad54d821d3e"),
    "treadplatemetal": ("maps/lq_e0m1.bsp",
        "a47239fce5283477891c50f7e6ae4e5bc00f1647c066e29d747ccc34426e8627"),
    # The exit door.
    "tek_door1": ("maps/lq_e1m1.bsp",
        "17a52787942ad5543f9fa8784aabe2657a732148abbffba465f04147fd155400"),
    # Not used on any face: ericw-tools 2.x reserves miptex slot 0 for it.
    "skip": ("maps/dev.bsp",
        "90776c8a801008cff4aceea7eaa79d1376fcc61a472357f31048fb477c28cc23"),
}
TEX_GRID = "tek_flr3"
TEX_FLOOR = "met_brn_flat"
TEX_WALL = "met_blu_rect"
TEX_TRIM = "met_blu_trim32"
TEX_CEIL = "met_gry_slat"
TEX_EXIT_FLOOR = "telepad1"
TEX_EXIT_WALL = "tele_frame1"
TEX_TRIGGER = "trigger"
TEX_CLIP = "clip"        # qbsp: solid to the walking hulls (1 and 2), not drawn, and
                         # not in hull 0, so shots and sight pass; no texture needed
TEX_MISSING = "psq_no_such_texture"

BREAK_KINDS = ("missing-texture", "leak", "no-exit", "cell-in-brush", "cell-corner", "pocket")

PAK_MAX_ENTRIES = 2048           # QuakeSpasm's MAX_FILES_IN_PACK
BSP_MAX_BYTES = 64 * 1024 * 1024
MIPTEX_HEADER = 40


class ArenaError(Exception):
    pass


# --- Brushes -----------------------------------------------------------------

# For an outward normal n, two axis vectors a, b with a x b = n: qbsp builds
# a face plane from points p0, p1, p2 with normal cross(p0 - p1, p2 - p1).
_FACE_AXES = {
    (0, 0, 1): ((1, 0, 0), (0, 1, 0)),
    (0, 0, -1): ((0, 1, 0), (1, 0, 0)),
    (1, 0, 0): ((0, 1, 0), (0, 0, 1)),
    (-1, 0, 0): ((0, 0, 1), (0, 1, 0)),
    (0, 1, 0): ((0, 0, 1), (1, 0, 0)),
    (0, -1, 0): ((1, 0, 0), (0, 0, 1)),
}


# --plan: every brush box() and ramp() make is noted here, with the kind
# _tag() last set, while a plan is being recorded (None otherwise).
_PLAN = None
_KIND = "block"
_FIT = False   # build_layout: fit framed textures to their faces (FRAMED)

# Framed textures: each picture one object (a light panel, a lamp, a door, a
# crate, the teleporter's pad and frame), so a face must show it whole, not
# wherever the world grid cuts it. In the shipped arenas each such face gets
# offsets that start the picture at its corner, and a scale that fits whole
# copies to it (a face under half the picture, a panel's edge, keeps scale
# 1; one from half the picture to its whole size is fitted to one copy). A
# room's floor and ceiling fit to the room, not the slab under its walls.
# Sizes as pinned (TEXTURES); main() checks them against the textures.
FRAMED = {"tek_lit1": (32, 64), "t_lit07": (64, 64), "tele_frame1": (64, 128),
          "tek_door1": (128, 128), "crate1_side": (64, 64), "crate1_top": (64, 64),
          "telepad1": (64, 64)}


class Tex(str):
    """A face's texture with its alignment: offsets (s, t) and scales (s, t),
    as the .map writes them after the name."""
    def __new__(cls, name, off=(0, 0), scale=(1, 1)):
        t = super().__new__(cls, name)
        t.off, t.scale = off, scale
        return t


def _fit(tex, normal, lo, hi):
    """tex fitted to the face of the box lo..hi facing normal, as qbsp maps
    it (Quake's base axes: a floor or ceiling s = x, t = -y; an x wall
    s = y, t = -z; a y wall s = x, t = -z; texel = coordinate / scale +
    offset)."""
    W, H = FRAMED[tex]
    if normal[2]:
        s_rng, t_rng = (lo[0], hi[0]), (-hi[1], -lo[1])
    elif normal[0]:
        s_rng, t_rng = (lo[1], hi[1]), (-hi[2], -lo[2])
    else:
        s_rng, t_rng = (lo[0], hi[0]), (-hi[2], -lo[2])
    off, scale = [], []
    for (a, b), size in ((s_rng, W), (t_rng, H)):
        n = round((b - a) / size)
        sc = (b - a) / (n * size) if n >= 1 else 1
        scale.append(sc)
        off.append((-a / sc) % size)
    return Tex(tex, tuple(off), tuple(scale))


def _tag(kind):
    global _KIND
    _KIND = kind


def box(x0, y0, z0, x1, y1, z1, tex, top=None, fit=None):
    """An axis-aligned box brush; top overrides the texture of its +z face.
    fit, (lo, hi), is the part a framed texture fits to when the box runs
    under walls (a room's floor slab: its visible floor)."""
    if not (x0 < x1 and y0 < y1 and z0 < z1):
        raise ArenaError(f"empty box {x0, y0, z0, x1, y1, z1}")
    if _PLAN is not None:
        _PLAN.append({"kind": _KIND, "rect": (x0, y0, x1, y1), "z0": z0, "z1": z1})
    faces = []
    for normal, (a, b) in _FACE_AXES.items():
        base = tuple(
            (hi if n > 0 else lo) if n else lo
            for n, lo, hi in zip(normal, (x0, y0, z0), (x1, y1, z1))
        )
        p1 = base
        p0 = tuple(p + 64 * d for p, d in zip(base, a))
        p2 = tuple(p + 64 * d for p, d in zip(base, b))
        t = top if (top is not None and normal == (0, 0, 1)) else tex
        if _FIT and t in FRAMED:
            t = _fit(t, normal, *(fit or ((x0, y0, z0), (x1, y1, z1))))
        faces.append((p0, p1, p2, t))
    return faces


def pillar_brush(cx, cy, half, z0, z1, tex):
    """An octagonal pillar shaft round (cx, cy), half-width half, z0 to z1:
    arena_kit's octagon, its corners cut by 0.4 of half."""
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:      # run as a script under -I
        sys.path.insert(0, here)
    import arena_kit as K
    if _PLAN is not None:
        _PLAN.append({"kind": _KIND, "rect": (cx - half, cy - half, cx + half, cy + half),
                      "z0": z0, "z1": z1})
    return K.prism(K.octagon(cx, cy, half), z0, z1, tex)


def wall_with_door(x0, y0, z0, x1, y1, z1, axis, lo, hi, top, tex):
    """A wall slab with a doorway [lo, hi] along axis ('x' or 'y'), floor to top."""
    out = []
    if axis == "x":
        out.append(box(x0, y0, z0, lo, y1, z1, tex))
        out.append(box(hi, y0, z0, x1, y1, z1, tex))
        out.append(box(lo, y0, top, hi, y1, z1, tex))
    else:
        out.append(box(x0, y0, z0, x1, lo, z1, tex))
        out.append(box(x0, hi, z0, x1, y1, z1, tex))
        out.append(box(x0, lo, top, x1, hi, z1, tex))
    return out


def room(interior, doors_south=None, doors_north=None, omit=(), tex_wall=TEX_WALL,
         tex_floor=TEX_FLOOR, tex_ceil=TEX_CEIL):
    """Floor, ceiling and four walls around an interior box. A door is
    (lo, hi, top) along x in the south or north wall."""
    x0, y0, z0, x1, y1, z1 = interior
    t = WALL
    _tag("floor")
    brushes = [box(x0 - t, y0 - t, z0 - t, x1 + t, y1 + t, z0, tex_floor,
                   fit=((x0, y0, z0 - t), (x1, y1, z0)))]
    _tag("ceiling")
    brushes.append(box(x0 - t, y0 - t, z1, x1 + t, y1 + t, z1 + t, tex_ceil,
                       fit=((x0, y0, z1), (x1, y1, z1 + t))))
    _tag("wall")
    walls = {
        "west": (x0 - t, y0 - t, z0, x0, y1 + t, z1),
        "east": (x1, y0 - t, z0, x1 + t, y1 + t, z1),
        "south": (x0, y0 - t, z0, x1, y0, z1),
        "north": (x0, y1, z0, x1, y1 + t, z1),
    }
    for side, w in walls.items():
        if side in omit:
            continue
        door = doors_south if side == "south" else doors_north if side == "north" else None
        if door:
            lo, hi, top = door
            brushes.extend(wall_with_door(*w, "x", lo, hi, top, tex_wall))
        else:
            brushes.append(box(*w, tex_wall))
    return brushes


def floor_with_grid(interior):
    """The hall floor as five slabs: the grid inlay and four margins."""
    x0, y0, z0, x1, y1, _ = interior
    t = WALL
    gx, gy = GRID_HALF_X, GRID_HALF_Y
    zb = z0 - t
    return [
        box(-gx, -gy, zb, gx, gy, z0, TEX_GRID),
        box(x0 - t, y0 - t, zb, x1 + t, -gy, z0, TEX_FLOOR),
        box(x0 - t, gy, zb, x1 + t, y1 + t, z0, TEX_FLOOR),
        box(x0 - t, -gy, zb, -gx, gy, z0, TEX_FLOOR),
        box(gx, -gy, zb, x1 + t, gy, z0, TEX_FLOOR),
    ]


def cell_centre(col, row):
    return (
        (col - (GRID_COLS - 1) / 2) * GRID_CELL,
        (row - (GRID_ROWS - 1) / 2) * GRID_CELL,
    )


# --- Entities ----------------------------------------------------------------


def build(break_kind=None, layout="hall", exit_map=EXIT_MAP):
    """Return (worldspawn brushes, [(entity keys, brushes)]) for the arena."""
    if layout != "hall":
        if break_kind is not None:
            raise ArenaError("--break needs the hall layout")
        world, ents, _ = build_layout(layout, exit_map)
        return world, ents
    if break_kind is not None and break_kind not in BREAK_KINDS:
        raise ArenaError(f"unknown --break kind: {break_kind}")
    world = []

    # Hall: floor inlay, ceiling, walls with a south and a north doorway.
    hall = room(HALL, doors_south=(-DOOR_HALF, DOOR_HALF, DOOR_TOP),
                doors_north=(-DOOR_HALF, DOOR_HALF, DOOR_TOP),
                omit=("east",) if break_kind == "leak" else ())
    _tag("floor")
    world.extend(floor_with_grid(HALL))
    world.extend(hall[1:])  # drop room()'s plain floor; the inlay replaces it

    # Trim band along the hall walls at the top of the doorways.
    hx0, hy0, _, hx1, hy1, _ = HALL
    tz0, tz1 = DOOR_TOP, DOOR_TOP + 16
    _tag("wall")
    world.append(box(hx0, hy0, tz0, hx0 + 8, hy1, tz1, TEX_TRIM))
    world.append(box(hx1 - 8, hy0, tz0, hx1, hy1, tz1, TEX_TRIM))

    # Corner pillars in the margins, outside the grid.
    _tag("pillar")
    for sx in (-1, 1):
        for sy in (-1, 1):
            cx, cy = sx * 656, sy * 464
            world.append(pillar_brush(cx, cy, 32, 0, HALL[5], TEX_TRIM))

    # Antechamber and exit alcove. Each sits 2 * WALL from the hall, so its
    # doorway wall slab lies flush against the hall's and the two doorways
    # form one sealed passage.
    world.extend(room(ANTE, doors_north=(-DOOR_HALF, DOOR_HALF, DOOR_TOP)))
    world.extend(room(EXIT, doors_south=(-DOOR_HALF, DOOR_HALF, DOOR_TOP),
                      tex_floor=TEX_EXIT_FLOOR))
    # The exit alcove's back wall face: a teleporter frame.
    ex0, _, _, ex1, ey1, _ = EXIT
    world.append(box(-64, ey1 - 8, 0, 64, ey1, 128, TEX_EXIT_WALL))

    if break_kind == "missing-texture":
        # A floor plate in the antechamber with a texture the WAD lacks.
        _tag("pad")
        world.append(box(-312, -768, 0, -168, -672, 8, TEX_FLOOR, top=TEX_MISSING))

    if break_kind == "cell-in-brush":
        cx, cy = cell_centre(5, 3)
        world.append(box(cx - 16, cy - 16, 0, cx + 16, cy + 16, 96, TEX_TRIM))
    if break_kind == "cell-corner":
        # A thin post on the corner four cells share, clear of their centres.
        cx, cy = cell_centre(2, 2)
        cx, cy = cx + GRID_CELL // 2, cy + GRID_CELL // 2
        world.append(box(cx - 8, cy - 8, 0, cx + 8, cy + 8, 96, TEX_TRIM))
    if break_kind == "pocket":
        # A platform in the south margin, too high to step onto, under a
        # second grid: every cell clear, none walkable from the start.
        world.append(box(-400, -528, 0, -208, -400, 64, TEX_TRIM, top=TEX_FLOOR))

    _tag("trigger")
    ents = []
    ents.append(({"classname": "info_player_start", "origin": "0 -896 24",
                  "angle": "90"}, []))
    ents.append(({"classname": "psq_arena", "origin": "0 0 0",
                  "psq_cols": str(GRID_COLS), "psq_rows": str(GRID_ROWS),
                  "psq_cell": str(GRID_CELL)}, []))
    if break_kind == "pocket":
        ents.append(({"classname": "psq_grid", "origin": "-304 -464 64", "psq_cols": "2",
                      "psq_rows": "1", "psq_cell": str(GRID_CELL)}, []))
    if break_kind != "no-exit":
        ents.append(({"classname": "trigger_changelevel", "map": EXIT_MAP,
                      "spawnflags": "1"},
                     [box(ex0 + 16, ey1 - 64, 0, ex1 - 16, ey1 - 16, 144, TEX_TRIGGER)]))

    # Pickups in the east and west margins (the grid spans x -528..528).
    for x, y, cls in (
        (-656, 0, "weapon_supershotgun"), (656, 0, "weapon_nailgun"),
        (-656, -192, "item_shells"), (656, -192, "item_spikes"),
        (-656, 192, "item_health"), (656, 192, "item_armor1"),
    ):
        ents.append(({"classname": cls, "origin": f"{x} {y} 16"}, []))

    # Lights: a 5 x 3 grid under the hall ceiling, two in the antechamber,
    # one in the exit alcove. All white (the test hall is as it shipped; its
    # psqarena.lit, which every arena's light run writes, is white too).
    for x in (-576, -288, 0, 288, 576):
        for y in (-384, 0, 384):
            ents.append(({"classname": "light", "origin": f"{x} {y} 208",
                          "light": "450"}, []))
    for x in (-160, 160):
        ents.append(({"classname": "light", "origin": f"{x} -800 152",
                      "light": "350"}, []))
    ents.append(({"classname": "light", "origin": "0 736 120", "light": "300"}, []))
    return world, ents


# --- The multi-level layouts (--layout, --arena) ------------------------------
#
# The Pit, Terraces and Towers, which ship as psqarena, psqterraces and
# psqtowers (ARENAS); "hall" above is the test hall, psqtesthall. Each opens
# the hall's south wall on its start room (start_room, below) at z = 0, and
# puts its exit alcove (exit_alcove) on the north wall at the level the
# layout chooses.
#
# Spawning: several grids instead of one. The psq_arena entity carries the
# first; each further one is a psq_grid point entity with the same keys
# (origin on that level's floor, psq_cols, psq_rows, psq_cell). The QuakeC
# controller picks a grid by its share of the cells, then a free cell in it
# (the engine's psq_grid_cell takes any entity with those keys). No two grids
# overlap in plan, so a cell's occupancy slab never meets another level's.

LAYOUTS = ("hall", "terraces", "pit", "towers")
CELL = GRID_CELL

# The arenas the app ships: map name, layout, where its north exit leads.
# New Game opens the first; each exit leads to the next, the last to the
# campaign. psqtesthall is the original one-grid hall, kept for the tests and
# loadable with `map psqtesthall`; no exit leads to it.
ARENAS = (
    ("psqarena", "pit", "psqterraces"),
    ("psqterraces", "terraces", "psqtowers"),
    ("psqtowers", "towers", EXIT_MAP),
    ("psqtesthall", "hall", EXIT_MAP),
)
ARENA_TITLES = {"pit": "psQuake Arena: The Pit", "terraces": "psQuake Arena: Terraces",
                "towers": "psQuake Arena: Towers", "hall": "psQuake Arena"}

# Shared accents. Hazard stripes mark the edges of raised floors; tread plate
# tops stairs and ramps; lamps and light panels are fullbright.
TX_HAZARD = "metalstrip_1"
TX_TREAD = "treadplatemetal"
TX_GRATE = "tek_grate"
TX_LAMP = "t_lit07"
TX_PANEL = "tek_lit1"
TX_CRATE = "crate1_side"
TX_CRATE_TOP = "crate1_top"

# Each arena's own textures: walls in three bands (a base, the field, a
# frieze), its ceiling, floors per level and the trim of its parts.
THEMES = {
    "pit": {"wall": "t_wall1b", "base": "t_trim2aa", "frieze": "t_trim1c",
            "ceil": "t_metalsheetc", "floor": "t_flor1a", "cover": "t_trim1ca",
            "pillar": "aqsupp01", "cap": "met_brn_trim64", "rail": "met_brn_trim16",
            "inlay": {-128: "t_flor1b", 0: "met_brn_stile", 128: "t_blok04"}},
    "terraces": {"wall": "met_brn_rect", "base": "met_brn_trim32", "frieze": "met_brn_trim16",
                 "ceil": "met_gry_slat", "floor": "met_dbrn_flat", "cover": "aqtrim01",
                 "pillar": "aqmetl07", "cap": "met_brn_trim64", "rail": "met_brn_trim16",
                 "inlay": {0: "met_brn_stile", 96: "met_grn_flat", 192: "met_lbrn_slat"},
                 # Its fires sit among lamp-lit walls: both their lights run
                 # this much stronger (_Builder._fire).
                 "fire": 1.35},
    "towers": {"wall": "met_blu_rect", "base": "met_blu_trim32", "frieze": "met_blu_trim16",
               "ceil": "met_blu_slat", "floor": "t_flor2a", "cover": "met_teal_trim32",
               "pillar": "met_blu_sqr", "cap": "met_blu_trim64", "rail": "met_blu_trim16",
               "inlay": {0: "met_blu_flat", 96: "t_flor1b", 192: "met_blu_sqr"}},
}
H_MEGA = 2       # item_health spawnflags: megahealth (+100)
BIG = 1          # item_shells / _spikes / _rockets / _cells spawnflags: the big box
COVER_H = 40     # low cover: the player's eye (46 up) sees over it

# Lighting: bright fighting areas, shadowy recesses and corners. A low
# minimum light and ericw's dirt mapping (ambient occlusion) let corners,
# recesses, the pit and the spaces behind cover fall dark; a few lamps over
# each arena's fighting areas (L["lamps"]) make the pools of light.
LIGHT_MIN = 2          # worldspawn "light": the darkest any face gets (with
                       # "_minlight_dirt" 0, so dirt mapping cannot take a face
                       # under it to no lightmap, which renders black)
LIGHT_DIRT = "1"       # worldspawn "_dirt": ambient occlusion on
LIGHT_DIRT_SCALE = "2"    # worldspawn "_dirtscale": how dark the occlusion gets
LIGHT_POOL = 340       # what a lamp gives the floor right under it
LIGHT_WAIT = 1.3       # a lamp's falloff (ericw "wait"; over 1, a smaller pool, sharper edge)
# Fire light, as real fire throws it: each torch and flame is two lights.
# Its own entity is a bright, short-throw yellow core (a high value that
# falls off fast: ericw "wait" over 1), so what is near the fire glows; a
# "light" entity at the same spot is a dimmer, medium-throw red-orange glow
# (wait under 1), so the falloff farther out is dimmer and redder. Their
# "_color": light -lit writes it to <arena>.lit, which the app bundles; every
# other light is white. A theme's "fire" scales both lights of its fires.
LIGHT_TORCH = 340      # a wall torch's core
LIGHT_TORCH_WAIT = 2.6     # ... falls away within ~130
LIGHT_TORCH_GLOW = 170     # its glow
LIGHT_FLAME = 460      # a large flame's core (it stands on the floor)
LIGHT_FLAME_WAIT = 2.2
LIGHT_FLAME_GLOW = 230
LIGHT_GLOW_WAIT = 0.55     # a torch's glow reaches ~310, a flame's ~420 (light / wait)
LIGHT_FIRE_COLOR = "1 0.78 0.4"    # the core: yellow
LIGHT_GLOW_COLOR = "1 0.42 0.12"   # the glow: red-orange
LIGHT_PANEL = 40       # the glow in front of a wall light panel
CRATE_H = 64     # a crate hides a standing player
RAIL_KICK = 8    # a railing's kick plate
RAIL_POST = 64   # the most between a railing's posts
RAIL_POST_W = 8  # a post's width along the railing
STAIR_HALF = 64  # the Pit's balcony stairways: 128 wide, two of hull 2


def _sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def ramp(x0, y0, x1, y1, zb, zlo, zhi, rise, tex, top=None, thick=0):
    """A ramp over the footprint, its top rising toward rise ("+x", "-x",
    "+y" or "-y") from zlo at the low edge to zhi at the high edge. Solid
    down to zb, or with thick a slab that thick under its top (a bridge)."""
    if not (x0 < x1 and y0 < y1 and zlo < zhi and zb <= zlo) or rise not in ("+x", "-x", "+y", "-y"):
        raise ArenaError(f"bad ramp {x0, y0, x1, y1, zb, zlo, zhi, rise}")
    sign, axis = rise
    if axis == "y":
        lo, hi = (y0, y1) if sign == "+" else (y1, y0)
        a0, a1 = x0, x1

        def P(a, r, z):
            return (a, r, z)
    else:
        lo, hi = (x0, x1) if sign == "+" else (x1, x0)
        a0, a1 = y0, y1

        def P(a, r, z):
            return (r, a, z)
    t = top if top is not None else tex
    if thick:
        under = [P(a0, lo, zlo - thick), P(a1, lo, zlo - thick), P(a0, hi, zhi - thick)]
        verts = [P(a, r, z - d) for a in (a0, a1) for r, z in ((lo, zlo), (hi, zhi))
                 for d in (0, thick)]
    else:
        under = [P(a0, lo, zb), P(a1, lo, zb), P(a0, hi, zb)]
        verts = [P(a, r, z) for a in (a0, a1) for r in (lo, hi) for z in (zb,)] + \
                [P(a, lo, zlo) for a in (a0, a1)] + [P(a, hi, zhi) for a in (a0, a1)]
    zs = zb if not thick else zlo - thick
    faces = [
        ([P(a0, lo, zlo), P(a1, lo, zlo), P(a0, hi, zhi)], t),
        (under, tex),
        ([P(a0, lo, zs), P(a0, hi, zs), P(a0, lo, zs + 64)], tex),
        ([P(a1, lo, zs), P(a1, hi, zs), P(a1, lo, zs + 64)], tex),
        ([P(a0, hi, zs), P(a1, hi, zs), P(a0, hi, zs + 64)], tex),
    ]
    if thick or zlo > zb:
        faces.append(([P(a0, lo, zs), P(a1, lo, zs), P(a0, lo, zs + 64)], tex))
    centre = tuple(sum(v[i] for v in verts) / len(verts) for i in range(3))
    out = []
    for (p0, p1, p2), ft in faces:
        if _dot(_cross(_sub(p0, p1), _sub(p2, p1)), _sub(centre, p1)) > 0:
            p0, p2 = p2, p0
        out.append((p0, p1, p2, ft))
    if _PLAN is not None:
        _PLAN.append({"kind": _KIND, "rect": (x0, y0, x1, y1), "z0": zs,
                      "z1": zhi, "zlo": zlo, "rise": rise})
    return out


def stairs(x0, y0, x1, y1, z0, z1, rise, tex=None, top=None):
    """Steps over the footprint rising toward rise from the floor at z0 in
    16-unit risers; the last riser climbs onto what stands at the high edge,
    whose top is z1. The footprint's length must split into equal treads.
    Risers are tex (hazard stripes), treads top (tread plate)."""
    n = (z1 - z0) // 16 - 1
    sign, axis = rise
    length = (y1 - y0) if axis == "y" else (x1 - x0)
    if n < 1 or (z1 - z0) % 16 or length % n:
        raise ArenaError(f"stairs {x0, y0, x1, y1} cannot rise {z0}..{z1} in 16s")
    tread = length // n
    out = []
    for i in range(n):
        ztop = z0 + 16 * (i + 1)
        if axis == "y":
            a, b = (y0 + i * tread, y0 + (i + 1) * tread) if sign == "+" else \
                   (y1 - (i + 1) * tread, y1 - i * tread)
            out.append(box(x0, a, z0, x1, b, ztop, tex or TX_HAZARD, top=top or TX_TREAD))
        else:
            a, b = (x0 + i * tread, x0 + (i + 1) * tread) if sign == "+" else \
                   (x1 - (i + 1) * tread, x1 - i * tread)
            out.append(box(a, y0, z0, b, y1, ztop, tex or TX_HAZARD, top=top or TX_TREAD))
    if _PLAN is not None:
        _PLAN.append({"kind": "stairs-arrow", "rect": (x0, y0, x1, y1), "z0": z0,
                      "z1": z1, "zlo": z0, "rise": rise})
    return out


def banded(x0, y0, z0, x1, y1, z1, layers):
    """A box cut across z into bands: layers is [(top, texture)] rising, the
    last reaching z1; a band outside [z0, z1] is dropped. No geometry
    changes, only where one texture gives way to the next."""
    out = []
    lo = z0
    for top, tex in layers:
        hi = min(top, z1)
        if hi > lo:
            out.append(box(x0, y0, lo, x1, y1, hi, tex))
            lo = hi
    if lo < z1:
        out.append(box(x0, y0, lo, x1, y1, z1, layers[-1][1]))
    return out


def wall_doors(x0, y0, z0, x1, y1, z1, lo, hi, zb, zt, layers):
    """A wall slab along x with a doorway x [lo, hi], z [zb, zt], banded."""
    out = banded(x0, y0, z0, lo, y1, z1, layers) + banded(hi, y0, z0, x1, y1, z1, layers) + \
        banded(lo, y0, zt, hi, y1, z1, layers)
    if zb > z0:
        out += banded(lo, y0, z0, hi, y1, zb, layers)
    return out


def region(cx, cy, z, cols, rows):
    """A spawn grid: centre, floor height, cells along x and y."""
    return {"origin": (cx, cy, z), "cols": cols, "rows": rows,
            "rect": (cx - cols * CELL // 2, cy - rows * CELL // 2,
                     cx + cols * CELL // 2, cy + rows * CELL // 2)}


def _pieces(x0, y0, x1, y1, inlays):
    """The footprint cut along every inlay edge inside it: [(x0, y0, x1, y1,
    inlay texture or None)], runs of equal texture merged along x."""
    xs = sorted({x0, x1} | {v for r, _ in inlays for v in (r[0], r[2]) if x0 < v < x1})
    ys = sorted({y0, y1} | {v for r, _ in inlays for v in (r[1], r[3]) if y0 < v < y1})
    out = []
    for ya, yb in zip(ys, ys[1:]):
        row = []
        for xa, xb in zip(xs, xs[1:]):
            cx, cy = (xa + xb) / 2, (ya + yb) / 2
            tex = next((t for r, t in inlays if r[0] < cx < r[2] and r[1] < cy < r[3]), None)
            if row and row[-1][2] == xa and row[-1][4] == tex:
                row[-1][2] = xb
            else:
                row.append([xa, ya, xb, yb, tex])
        out.extend(tuple(p) for p in row)
    return out


class _Builder:
    """A layout's brushes and decorations. Raised blocks are kept until the
    spawn grids are known, so each grid's floor gets its inlay."""

    def __init__(self, theme):
        self.T = theme
        self.world = []
        self.blocks = []
        self.ents = []

    def add(self, brush_or_list):
        if brush_or_list and isinstance(brush_or_list[0], list):
            self.world.extend(brush_or_list)
        else:
            self.world.append(brush_or_list)

    def block(self, x0, y0, z0, x1, y1, z1, side, top, band=TX_HAZARD):
        """A raised floor: side faces, top, and a 16-unit band along its edge."""
        self.blocks.append((_KIND, (x0, y0, z0, x1, y1, z1), side, top, band))

    def cover(self, x0, y0, x1, y1, z, h=COVER_H, tex=None):
        _tag("cover")
        self.world.append(box(x0, y0, z, x1, y1, z + h, tex or self.T["cover"]))

    def rail(self, x0, y0, x1, y1, z, h=COVER_H):
        """A railing over a thin footprint (its long side along x or y): a
        kick plate, slim posts at most RAIL_POST apart, a mid rail and a top
        rail; and a clip brush filling the footprint to h, so walking and
        leaping hulls meet the solid block it stands for (a 32 one the
        player jumps and no monster steps), while shots and sight pass
        between the rails."""
        _tag("cover")
        T = self.T
        along_x = x1 - x0 >= y1 - y0
        a0, a1 = (x0, x1) if along_x else (y0, y1)
        c = (y0 + y1) // 2 if along_x else (x0 + x1) // 2
        half = ((y1 - y0) if along_x else (x1 - x0)) // 2

        def bar(a, b, t, za, zb, tex):
            if along_x:
                return box(a, c - t, za, b, c + t, zb, tex)
            return box(c - t, a, za, c + t, b, zb, tex)

        w = self.world
        w.append(bar(a0, a1, half, z, z + RAIL_KICK, T["cover"]))
        n = max(1, -(-(a1 - a0 - RAIL_POST_W) // RAIL_POST))
        for i in range(n + 1):
            p = a0 + (a1 - a0 - RAIL_POST_W) * i // n
            w.append(bar(p, p + RAIL_POST_W, 3, z + RAIL_KICK, z + h - 4, T["rail"]))
        m = z + RAIL_KICK + (h - RAIL_KICK - 4) // 2
        w.append(bar(a0, a1, 2, m - 2, m + 2, T["rail"]))
        w.append(bar(a0, a1, 4, z + h - 4, z + h, T["rail"]))
        _tag("clip")
        w.append(box(x0, y0, z, x1, y1, z + h, TEX_CLIP))
        _tag("cover")

    def crate(self, x0, y0, x1, y1, z, h=CRATE_H):
        _tag("cover")
        self.world.append(box(x0, y0, z, x1, y1, z + h, TX_CRATE, top=TX_CRATE_TOP))

    def pillar(self, cx, cy, half, z0, z1):
        """An octagonal pillar with a square base and cap 8 units wider all
        round."""
        _tag("pillar")
        T = self.T
        self.world.append(pillar_brush(cx, cy, half, z0, z1, T["pillar"]))
        self.world.append(box(cx - half - 8, cy - half - 8, z0, cx + half + 8, cy + half + 8,
                              z0 + 16, T["cap"]))
        self.world.append(box(cx - half - 8, cy - half - 8, z1 - 16, cx + half + 8,
                              cy + half + 8, z1, T["cap"]))

    def _fire(self, cls, x, y, z, core, wait, glow):
        """A fire entity (its model, and its yellow core light) and its
        red-orange glow at the same spot (LIGHT_* above)."""
        k = self.T.get("fire", 1)   # a theme's fire-light strength
        core, glow = round(core * k), round(glow * k)
        self.ents.append(({"classname": cls, "origin": f"{x} {y} {z}", "light": str(core),
                           "wait": f"{wait:g}", "_color": LIGHT_FIRE_COLOR}, []))
        self.ents.append(({"classname": "light", "origin": f"{x} {y} {z}", "light": str(glow),
                           "wait": f"{LIGHT_GLOW_WAIT:g}", "_color": LIGHT_GLOW_COLOR}, []))

    def torch(self, x, y, z):
        self._fire("light_torch_small_walltorch", x, y, z, LIGHT_TORCH, LIGHT_TORCH_WAIT,
                   LIGHT_TORCH_GLOW)

    def flame(self, x, y, z):
        self._fire("light_flame_large_yellow", x, y, z, LIGHT_FLAME, LIGHT_FLAME_WAIT,
                   LIGHT_FLAME_GLOW)

    def emit_blocks(self, regions, inlay_tex):
        for kind, (x0, y0, z0, x1, y1, z1), side, top, band in self.blocks:
            _tag(kind)
            inlays = []
            for r in regions:
                a, b, c, d = r["rect"]
                if r["origin"][2] == z1 and a < x1 and x0 < c and b < y1 and y0 < d:
                    inlays.append(((max(a, x0), max(b, y0), min(c, x1), min(d, y1)),
                                   inlay_tex.get(z1, top)))
            for xa, ya, xb, yb, tex in _pieces(x0, y0, x1, y1, inlays):
                if band and z1 - z0 > 16:
                    self.world.append(box(xa, ya, z0, xb, yb, z1 - 16, side, top=side))
                    self.world.append(box(xa, ya, z1 - 16, xb, yb, z1, band, top=tex or top))
                else:
                    self.world.append(box(xa, ya, z0, xb, yb, z1, side, top=tex or top))


def _pit(B):
    """A ring floor at 0 round a pit at -128 (two ramps down, a bridge over),
    balconies at 128 along the west and east walls, each reached by a
    stairway up its middle from the ring and by ramps along the end walls
    to corner landings. A spawn grid on each balcony half."""
    L = {"hall": (-768, -576, 768, 576), "zlo": -128, "zhi": 352, "exit_z": 0}
    _tag("block")
    for b in ((-768, -576, 768, -256), (-768, 256, 768, 576),
              (-768, -256, -384, 256), (384, -256, 768, 256)):
        B.block(b[0], b[1], -128, b[2], b[3], 0, "t_wall2b", "met_brn_tile2")
    # Each balcony is cut in the middle by a stairway straight up from the
    # ring: monsters chase in a straight line, so the corner ramps alone
    # left a player on the balcony out of reach of ground monsters.
    for x0, x1, xs0, xs1 in ((-768, -528, -768, -696), (528, 768, 696, 768)):
        for y0, y1 in ((-320, -STAIR_HALF), (STAIR_HALF, 320)):
            B.block(x0, y0, 0, x1, y1, 128, "t_wall3a", "t_blok03", band="met_brn_trim16")
        B.block(xs0, -STAIR_HALF, 0, xs1, STAIR_HALF, 128, "t_wall3a", "t_blok03",
                band="met_brn_trim16")
    for x0, x1 in ((-768, -640), (640, 768)):
        for y0, y1 in ((-576, -320), (320, 576)):
            B.block(x0, y0, 0, x1, y1, 128, "t_wall3a", "t_blok03", band="met_brn_trim16")
    _tag("ramp")
    B.add(ramp(-384, -128, -256, 256, -128, -128, 0, "+y", "aqsupp03", top=TX_TREAD))
    B.add(ramp(256, -256, 384, 128, -128, -128, 0, "-y", "aqsupp03", top=TX_TREAD))
    for y0, y1 in ((-576, -448), (448, 576)):
        B.add(ramp(-640, y0, -384, y1, 0, 0, 128, "-x", "aqsupp03", top=TX_TREAD))
        B.add(ramp(384, y0, 640, y1, 0, 0, 128, "+x", "aqsupp03", top=TX_TREAD))
    _tag("stairs")
    B.add(stairs(-696, -STAIR_HALF, -528, STAIR_HALF, 0, 128, "-x", tex=B.T["cap"]))
    B.add(stairs(528, -STAIR_HALF, 696, STAIR_HALF, 0, 128, "+x", tex=B.T["cap"]))
    _tag("bridge")
    B.add(box(-64, -256, -16, 64, 256, 0, "met_brn_trim16", top=TX_GRATE))
    # Railings round the pit lip (32: you can jump them, monsters cannot)
    # and the balcony parapets.
    for x0, x1 in ((-256, -64), (64, 384)):
        B.rail(x0, 256, x1, 272, 0, h=32)
    for x0, x1 in ((-384, -64), (64, 256)):
        B.rail(x0, -272, x1, -256, 0, h=32)
    B.rail(-400, -256, -384, 128, 0, h=32)
    B.rail(384, -128, 400, 256, 0, h=32)
    for y0, y1 in ((-320, -224), (-96, -STAIR_HALF), (STAIR_HALF, 96), (224, 320)):
        B.rail(-544, y0, -528, y1, 128, h=32)
        B.rail(528, y0, 544, y1, 128, h=32)
    # Low walls by the entrance.
    B.cover(-320, -448, -160, -416, 0)
    B.cover(160, -448, 320, -416, 0)
    for x in (-448, 448):
        B.pillar(x, -352, 32, 0, 352)
    L["regions"] = [region(0, 0, -128, 5, 5),
                    region(-240, 424, 0, 3, 3), region(240, 424, 0, 3, 3),
                    region(-456, 0, 0, 1, 5), region(456, 0, 0, 1, 5),
                    region(-656, -160, 128, 2, 2), region(-656, 160, 128, 2, 2),
                    region(656, -160, 128, 2, 2), region(656, 160, 128, 2, 2)]
    L["items"] = [("weapon_supershotgun", -352, -528, 16), ("weapon_nailgun", -352, -240, -112),
                  ("item_shells", 320, -528, 16), ("item_spikes", 288, 192, -112),
                  ("item_health", -736, -288, 144), ("item_armor1", 688, 272, 144),
                  ("item_health", -192, -528, 16), ("item_health", 160, -528, 16),
                  ("item_health", -736, 272, 144), ("item_health", 688, -288, 144),
                  ("item_health", 0, 520, 16, H_MEGA),
                  ("item_shells", -288, -320, 16, BIG), ("item_spikes", 256, -320, 16, BIG)]
    # Resupply spots (psq_supply): the ring, the pit, the balconies' back
    # strips and the corner landings.
    L["supply"] = [(-96, -400, 16), (96, -400, 16), (0, 360, 16),
                   (-304, -176, -112), (336, 144, -112),
                   (-736, 0, 144), (720, 0, 144), (-704, -480, 144), (704, 480, 144)]
    # The pit lit from below and by torches on its walls; the ring by the
    # ceiling lamps, a little dimmer than the other arenas.
    # Lamps over the ring's north and south and each balcony half; the pit
    # gets one dim light in its middle, its corners and the ring's ends dark.
    L["lamps"] = [(-240, 424, 0), (240, 424, 0), (0, -448, 0),
                  (-656, -192, 128), (-656, 192, 128), (656, -192, 128), (656, 192, 128)]
    L["lights"] = [(0, 0, -48, 200)]
    for x in (-160, 160):
        B.torch(x, -248, -56)
        B.torch(x, 248, -56)
        B.torch(x, -568, 96)
        B.torch(x, 568, 96)
    for y in (-200, 200):
        B.torch(-760, y, 208)
        B.torch(760, y, 208)
    for x in (-736, 736):
        for y in (-544, 544):
            B.flame(x, y, 136)
    L["panels"] = [("w", -440, 200), ("w", 440, 200), ("e", -440, 200), ("e", 440, 200),
                   ("n", -400, 200), ("n", 400, 200), ("s", -400, 200), ("s", 400, 200)]
    L["cameras"] = [("start", 0, -528, 0, 14, 90), ("perch", -576, -160, 128, 26, 15),
                    ("cover", -240, -480, 0, 4, 90),
                    ("room", 0, -1240, 0, -6, 90), ("escape", 64, -784, 0, 0, 0),
                    ("balcony", 816, -24, 256, 20, 205),
                    ("torch", 120, -140, -128, -12, 290)]
    L["views"] = [("overview", 0, -560, 320, 38, 90)]
    # The escape's balcony over the east balcony's stair head (128): 128 up,
    # out of a bite's reach (100) from below.
    L["balcony"] = (-24, 256)
    L["pins"] = [(-144, -48, -128), (144, 144, -128), (-456, 96, 0), (240, 424, 0),
                 (656, 96, 128), (-240, 520, 0)]
    L["blurb"] = ("The Pit: a ring round a sunken pit, crossed by a bridge, with balconies "
                  "on both side walls and a stair up each. Monsters climb out of the pit and "
                  "up to the balconies; you fight from above.")
    return L


def _terraces(B):
    """Three terraces rising south to north (0, 96, 192); the exit is on the
    top one, whose open front is the perch. Stairways between tiers, low
    walls and crates for cover, parapets on the tier edges' outer thirds."""
    L = {"hall": (-768, -576, 768, 832), "zlo": 0, "zhi": 448, "exit_z": 192}
    _tag("block")
    B.block(-768, -64, 0, 768, 384, 96, "aqmetl28", "met_grn_stile")
    B.block(-768, 384, 0, 768, 832, 192, "aqsupp06", "met_mix_diam")
    _tag("stairs")
    for x0, x1 in ((-96, 96), (-768, -608), (608, 768)):
        B.add(stairs(x0, -224, x1, -64, 0, 96, "+y", tex=B.T["cap"]))
    for x0, x1 in ((-416, -256), (256, 416)):
        B.add(stairs(x0, 224, x1, 384, 96, 192, "+y", tex=B.T["cap"]))
    # Railings on the tier edges (32: a jump for the player, a wall for monsters).
    for x0, x1 in ((-512, -256), (256, 512)):
        B.rail(x0, -64, x1, -48, 96, h=32)
    for x0, x1 in ((-736, -480), (480, 736)):
        B.rail(x0, 384, x1, 400, 192, h=32)
    # Bottom tier: low walls flanking the entry lane, by the door, and in
    # front of the middle tier's face.
    for x0, x1 in ((-160, -128), (128, 160)):
        B.cover(x0, -432, x1, -272, 0)
    for x0, x1 in ((-416, -256), (256, 416)):
        B.cover(x0, -560, x1, -528, 0)
    for x0, x1 in ((-576, -448), (448, 576)):
        B.cover(x0, -176, x1, -160, 0)
    # Middle tier: crates in the middle, low walls beside the grids.
    for x0, x1 in ((-128, -64), (64, 128)):
        B.crate(x0, 128, x1, 192, 96)
    for x0, x1 in ((-176, -160), (160, 176)):
        B.cover(x0, 0, x1, 128, 96)
    # Top tier: low walls flanking the perch.
    for x0, x1 in ((-256, -224), (224, 256)):
        B.cover(x0, 448, x1, 576, 192)
    for x, y in ((-672, -400), (672, -400), (-288, 752), (288, 752)):
        B.pillar(x, y, 32, 0 if y < 0 else 192, 448)
    L["regions"] = [region(-384, -336, 0, 4, 3), region(384, -336, 0, 4, 3),
                    region(-432, 80, 96, 5, 2), region(432, 80, 96, 5, 2),
                    region(-528, 592, 192, 4, 3), region(528, 592, 192, 4, 3)]
    L["items"] = [("weapon_supershotgun", -720, -528, 16), ("weapon_nailgun", 688, -528, 16),
                  ("item_shells", -720, -288, 16), ("item_spikes", 688, -288, 16),
                  ("item_health", -720, 240, 112), ("item_armor1", 704, 792, 208),
                  ("item_health", 688, 240, 112), ("item_health", -64, 280, 112),
                  ("item_health", -720, 792, 208), ("item_health", 48, -448, 16),
                  ("item_health", 0, 784, 208, H_MEGA),
                  ("item_shells", -224, -528, 16, BIG), ("item_spikes", 224, -528, 16, BIG)]
    # Resupply spots: three on each tier.
    L["supply"] = [(-32, -320, 16), (-480, -528, 16), (448, -528, 16),
                   (0, 40, 112), (-720, 0, 112), (688, 0, 112),
                   (-200, 700, 208), (200, 700, 208), (0, 500, 208)]
    # Lamps over each tier's middle; the tier faces, the side stairs and the
    # corners stay dark.
    L["lamps"] = [(-288, -352, 0), (288, -352, 0), (-400, 80, 96), (400, 80, 96),
                  (-528, 592, 192), (528, 592, 192), (0, 592, 192)]
    L["lights"] = []
    for x in (-448, 448):
        B.torch(x, -72, 56)
    for x in (-560, 560):
        B.torch(x, 376, 152)
    for x in (-160, 160):
        B.torch(x, 824, 280)
    for x in (-760, 760):
        B.torch(x, -320, 80)
    for x in (-736, 736):
        B.flame(x, 800, 200)
    L["panels"] = [("w", -320, 300), ("w", 320, 340), ("e", -320, 300), ("e", 320, 340),
                   ("n", -480, 330), ("n", 480, 330), ("s", -400, 300), ("s", 400, 300)]
    L["cameras"] = [("start", 0, -528, 0, -10, 90), ("perch", 0, 432, 192, 28, 270),
                    ("cover", -100, -352, 0, 4, 180),
                    ("room", 0, -1240, 0, -6, 90), ("escape", 64, -784, 0, 0, 0),
                    ("balcony", 816, 56, 288, 20, 205)]
    L["views"] = [("overview", 0, -560, 400, 34, 90)]
    # The escape's balcony over the middle tier (96), 64 over the top tier's
    # parapets (224), which a fiend can jump onto.
    L["balcony"] = (56, 288)
    L["pins"] = [(-432, -336, 0), (336, -240, 0), (-336, 128, 96), (432, 32, 96),
                 (-480, 592, 192), (576, 688, 192)]
    L["blurb"] = ("Terraces: three tiers rising to the exit. You start at the bottom; "
                  "monsters hold the high ground and come down five stairways.")
    return L


def _towers(B):
    """An open court: towers at 192 in the north corners joined by a catwalk,
    a plinth at 96 in the middle with a sloped bridge up to the catwalk,
    long ramps up the side walls, low walls scattered on the ground."""
    L = {"hall": (-768, -576, 768, 832), "zlo": 0, "zhi": 448, "exit_z": 0}
    _tag("block")
    for x0, x1 in ((-768, -512), (512, 768)):
        B.block(x0, 384, 0, x1, 832, 192, "met_blu_rivs", "met_teal_flat", band="met_teal_trim32")
    B.block(-192, -64, 0, 192, 256, 96, "met_teal_trim64", "met_blu_stile")
    _tag("bridge")
    B.add(box(-512, 704, 176, 512, 832, 192, "met_blu_trim32s", top=TX_GRATE))
    B.add(ramp(-64, 256, 64, 704, 0, 96, 192, "+y", "met_blu_trim32s", top=TX_GRATE, thick=16))
    _tag("ramp")
    B.add(ramp(-768, 0, -640, 384, 0, 0, 192, "+y", "met_blu_trim64", top=TX_TREAD))
    B.add(ramp(640, 0, 768, 384, 0, 0, 192, "+y", "met_blu_trim64", top=TX_TREAD))
    _tag("stairs")
    B.add(stairs(-96, -224, 96, -64, 0, 96, "+y", tex=B.T["cap"]))
    B.add(stairs(-352, 32, -192, 160, 0, 96, "+x", tex=B.T["cap"]))
    B.add(stairs(192, 32, 352, 160, 0, 96, "-x", tex=B.T["cap"]))
    # Ground: low walls round the grids and by the door.
    B.cover(-400, -320, -368, -160, 0)
    B.cover(368, -320, 400, -160, 0)
    B.cover(-96, -416, 96, -384, 0)
    B.cover(-320, -480, -288, -352, 0)
    B.cover(288, -480, 320, -352, 0)
    B.cover(-456, 400, -424, 560, 0)
    B.cover(424, 400, 456, 560, 0)
    # Plinth: railings on its north and south edges, the bridge and stairs open.
    for x0, x1 in ((-192, -80), (80, 192)):
        B.rail(x0, 224, x1, 240, 96)
    for x0, x1 in ((-192, -112), (112, 192)):
        B.rail(x0, -64, x1, -48, 96)
    # Towers: railings on their inner edges and on the catwalk by them.
    B.rail(-528, 400, -512, 688, 192)
    B.rail(512, 400, 528, 688, 192)
    for x0, x1 in ((-512, -384), (384, 512)):
        B.rail(x0, 704, x1, 720, 192, h=32)
    for x in (-288, 288):
        B.pillar(x, 768, 16, 0, 176)
    L["regions"] = [region(0, 528, 0, 8, 3), region(-576, -336, 0, 3, 3),
                    region(576, -336, 0, 3, 3), region(-480, 144, 0, 2, 3),
                    region(480, 144, 0, 2, 3), region(0, 96, 96, 3, 2),
                    region(-640, 624, 192, 2, 3), region(640, 624, 192, 2, 3)]
    L["items"] = [("weapon_supershotgun", -720, -528, 16), ("weapon_nailgun", 200, 752, 208),
                  ("item_shells", 688, -528, 16), ("item_spikes", -232, 752, 208),
                  ("item_health", 0, -40, 112), ("item_armor1", -720, 400, 208),
                  ("item_health", -240, -528, 16), ("item_health", 208, -528, 16),
                  ("item_health", 720, 400, 208), ("item_health", -608, -96, 16),
                  ("item_health", 0, 752, 16, H_MEGA),
                  ("item_shells", 256, -112, 16, BIG), ("item_spikes", -256, -112, 16, BIG)]
    # Resupply spots: the court, under the catwalk and the tower tops.
    L["supply"] = [(0, -480, 16), (-160, -240, 16), (144, -240, 16),
                   (-160, 768, 16), (160, 768, 16), (-592, 432, 208), (592, 432, 208)]
    # Lamps over the court's south and north, the plinth and the tower tops;
    # under the catwalk and the court's corners stay dark.
    L["lamps"] = [(-384, -336, 0), (384, -336, 0), (0, 528, 0), (0, 96, 96),
                  (-640, 624, 192), (640, 624, 192)]
    L["lights"] = [(x, 768, 112, 120) for x in (-160, 160)]
    for s in (-1, 1):
        B.torch(s * 504, 560, 120)
        B.torch(s * 200, 224, 64)
        B.torch(s * 144, 824, 96)
        B.flame(s * 720, 784, 200)
    L["panels"] = [("w", -320, 320), ("e", -320, 320), ("s", -400, 320), ("s", 400, 320),
                   ("n", -400, 300), ("n", 400, 300)]
    L["cameras"] = [("start", 0, -528, 0, -6, 90), ("perch", 640, 432, 192, 16, 228),
                    ("cover", -336, -240, 0, 4, 180), ("catwalk", 0, 768, 192, 22, 270),
                    ("room", 0, -1240, 0, -6, 90), ("escape", 64, -784, 0, 0, 0),
                    ("balcony", 816, -304, 176, 20, 200)]
    L["views"] = [("overview", 0, -560, 420, 32, 90)]
    # The escape's balcony over the court (0), south of the east ramp.
    L["balcony"] = (-304, 176)
    L["pins"] = [(-576, -336, 0), (0, 144, 96), (-240, 528, 0), (688, 624, 192),
                 (432, 144, 0), (-592, 528, 192)]
    L["blurb"] = ("Towers: an open court with two corner towers, a catwalk over the exit "
                  "and a plinth in the middle, joined by ramps and a sloped bridge.")
    return L


_LAYOUT_FNS = {"terraces": _terraces, "pit": _pit, "towers": _towers}


def hall_description():
    """The test hall in the same terms as a layout (for --plan)."""
    gx, gy = GRID_HALF_X, GRID_HALF_Y
    return {"hall": (HALL[0], HALL[1], HALL[3], HALL[4]), "zlo": 0, "zhi": HALL[5],
            "exit_z": 0,
            "regions": [{"origin": (0, 0, 0), "cols": GRID_COLS, "rows": GRID_ROWS,
                         "rect": (-gx, -gy, gx, gy)}],
            "items": [("weapon_supershotgun", -656, 0, 16), ("weapon_nailgun", 656, 0, 16),
                      ("item_shells", -656, -192, 16), ("item_spikes", 656, -192, 16),
                      ("item_health", -656, 192, 16), ("item_armor1", 656, 192, 16)],
            "cameras": [("start", 0, -536, 0, 8, 90)], "pins": [],
            "blurb": "The test hall: one floor, one grid."}



# --- The start room and the exit (the shipped arenas) ------------------------
#
# A start room monsters cannot walk into: the spawn at its south end, the
# escape's mouth in its east wall, and a lip in the doorway to the hall,
# LIP_H high: the player jumps it (Quake's jump clears about 45),
# a walking monster cannot step it (18). The exit: a deeper alcove behind a
# door the player shoots open (psq_exit_door), so leaving is deliberate.

START_ROOM = (-416, -1312, 0, 416, -608, 256)   # interior
START = (0, -1248, 24)                          # the player start
LIP_H = 32
START_DOOR_TOP = 112     # the doorway over the lip: 80 clear above it, room for the
                         # player (56) but for no hull-2 monster (88), a leaping
                         # fiend included
HALL_ENTRY = (0, -528, 24)                      # just inside the hall's door
EXIT_DOOR_IN = 64                               # how far into the alcove the door is
EXIT_DEPTH = 320                                # the alcove, south to north
TX_EXIT_DOOR = "tek_door1"
# The escape: a hallway from the room's north end, east
# through the room's east wall, then north outside the hall's east wall and
# up a stair to a balcony set into that wall, far from the doorway the
# monsters crowd. A window opens it on the hall: the player drops from it or
# shoots from behind its low cover. Each layout's "balcony" (y centre,
# floor) puts it over the hall high enough that no monster climbs or leaps
# up (validate_bsp startroom) and low enough that the drop does no damage.
ESC_Y = (-848, -720)        # the hallway's east leg (and its mouth), south to north
ESC_X = (784, 896)          # its north leg and the balcony, west to east (the hall's
                            # east wall, x 768..784, is its west side)
ESC_TOP = 128               # the east leg's ceiling, and the mouth's
BALCONY_HALF = 96           # half the balcony's length along the wall
BALCONY_HEAD = 96           # from the balcony floor to the window's top and the ceiling
FALL_SAFE = 216             # Quake hurts a fall from over ~264 (client.qc jump_flag -650);
                            # jumping off adds ~46
MELEE = 100                 # the longest melee reach (fiend, ogre, dog, shambler)
# worldspawn keys for validate_bsp (a leading "_": the engine skips them)
RULES = "start-room exit-door balcony"


def start_room(world, T):
    """The start room's brushes into world; returns its lights and psq_safe
    zone."""
    x0, y0, z0, x1, y1, z1 = START_ROOM
    world.extend(room(START_ROOM, doors_north=(-DOOR_HALF, DOOR_HALF, START_DOOR_TOP),
                      omit=("east",),
                      tex_wall=T["wall"], tex_floor=T["floor"], tex_ceil=T["ceil"]))
    # The east wall, with the escape hallway's mouth.
    _tag("wall")
    world.extend(wall_with_door(x1, y0 - WALL, z0, x1 + WALL, y1 + WALL, z1, "y",
                                ESC_Y[0], ESC_Y[1], ESC_TOP, T["wall"]))
    ents = []
    # A baffle across the room's middle, so a monster crowding the doorway
    # has no line to the spawn; the player walks round either end of it.
    _tag("cover")
    world.append(box(-96, -960, z0, 96, -928, 112, T["pillar"], top=T["cap"]))
    # The lip, in the doorway through both wall slabs into the hall.
    _tag("cover")
    world.append(box(-DOOR_HALF, y1, z0, DOOR_HALF, y1 + 2 * WALL, LIP_H, T["cover"],
                     top=TX_HAZARD))
    # Well lit from the spawn to the doorway.
    for x, y, lv in ((0, -1200, 400), (-280, -1100, 360), (280, -1100, 360), (0, -760, 400),
                     (-280, -760, 300), (280, -760, 300)):
        ents.append(({"classname": "light", "origin": f"{x} {y} 200", "light": str(lv)}, []))
    # The escape's mouth reads from the room: framed in hazard stripes, a
    # light panel over it, lit in front and inside.
    _tag("panel")
    my = (ESC_Y[0] + ESC_Y[1]) // 2
    ey0, ey1 = ESC_Y
    world.append(box(x1 - 4, ey0 - 16, z0, x1, ey0, ESC_TOP + 16, TX_HAZARD))
    world.append(box(x1 - 4, ey1, z0, x1, ey1 + 16, ESC_TOP + 16, TX_HAZARD))
    world.append(box(x1 - 4, ey0, ESC_TOP, x1, ey1, ESC_TOP + 16, TX_HAZARD))
    world.append(box(x1 - 4, my - 16, ESC_TOP + 32, x1, my + 16, ESC_TOP + 96, TX_PANEL))
    ents.append(({"classname": "light", "origin": f"{x1 - 24} {my} {ESC_TOP - 16}",
                  "light": "320"}, []))
    ents.append(({"classname": "light", "origin": f"{x1 + 24} {my} {ESC_TOP - 32}",
                  "light": "360"}, []))
    # No monster is placed in the room (psq.qc psq_in_safe), the doorway
    # and its lip included.
    _tag("trigger")
    ents.append(({"classname": "psq_safe"},
                 [box(x0, y0, z0, x1, y1 + 2 * WALL, z1, TEX_TRIGGER)]))
    return ents


def escape(world, T, hall, balcony, zhi):
    """The escape hallway and its balcony (ESC_*) into world, for a hall
    (x0, y0, x1, y1) with its ceiling at zhi and a balcony (y centre,
    floor); returns its entities and the window (y0, y1, z0, z1) to cut in
    the hall's east wall."""
    hx1, hy0 = hall[2], hall[1]
    if hx1 + WALL != ESC_X[0] or hy0 != HALL[1]:
        raise ArenaError("the escape runs along an east wall at x 768, from y -576")
    yc, H = balcony
    if H + BALCONY_HEAD > zhi or yc + BALCONY_HALF + WALL > hall[3]:
        raise ArenaError(f"the balcony {balcony} does not fit the hall's east wall")
    t = WALL
    rx = START_ROOM[3] + t                       # the room's east wall's outer face
    ex0, ex1 = ESC_X
    ey0, ey1 = ESC_Y
    yb0, yb1 = yc - BALCONY_HALF, yc + BALCONY_HALF
    zc = H + BALCONY_HEAD
    _tag("floor")
    world.append(box(rx, ey0 - t, -t, ex1 + t, ey1 + t, 0, T["floor"]))
    world.append(box(ex0 - t, ey1, -t, ex1 + t, yb1 + t, 0, T["floor"]))
    _tag("ceiling")
    world.append(box(rx, ey0 - t, ESC_TOP, ex0, ey1 + t, ESC_TOP + t, T["ceil"]))
    world.append(box(ex0 - t, ey0 - t, zc, ex1 + t, yb1 + t, zc + t, T["ceil"]))
    _tag("wall")
    world.append(box(rx, ey0 - t, 0, ex0 - t, ey0, ESC_TOP, T["wall"]))     # east leg, south
    world.append(box(rx, ey1, 0, ex0 - t, ey1 + t, ESC_TOP, T["wall"]))     # east leg, north
    world.append(box(ex0 - t, ey0 - t, 0, ex1 + t, ey0, zc, T["wall"]))     # the corner, south
    world.append(box(ex0 - t, ey0, ESC_TOP, ex0, ey1, zc, T["wall"]))       # over the leg's end
    world.append(box(ex0 - t, ey1, 0, ex0, hy0 - t, zc, T["wall"]))         # west, to the hall
    world.append(box(ex1, ey0, 0, ex1 + t, yb1 + t, zc, T["wall"]))         # east
    world.append(box(ex0, yb1, 0, ex1, yb1 + t, zc, T["wall"]))             # north
    # Up from the corner to the balcony, a solid floor at H.
    _tag("stairs")
    world.extend(stairs(ex0, ey1, ex1, yb0, 0, H, "+y", tex=T["cap"]))
    _tag("block")
    world.append(box(ex0, yb0, 0, ex1, yb1, H, T["base"], top=TX_TREAD))
    # Low cover at the window's ends; the middle open to drop from.
    _tag("cover")
    for a, b in ((yb0, yb0 + 48), (yb1 - 48, yb1)):
        world.append(box(ex0, a, H, ex0 + 32, b, H + COVER_H, T["cover"]))
    ents = []
    my = (ey0 + ey1) // 2
    for x, y, z, lv in ((512, my, 96, 300), (672, my, 96, 300), (840, my, 104, 300),
                        (840, (ey1 + yb0) // 2, H // 2 + 72, 280), (856, yc, H + 64, 200)):
        ents.append(({"classname": "light", "origin": f"{x} {y} {z}", "light": str(lv)}, []))
    # No monster is placed in the hallway or on the balcony (two zones: one
    # box round both would take in the hall).
    _tag("trigger")
    ents.append(({"classname": "psq_safe"},
                 [box(rx - t, ey0, 0, ex0, ey1, ESC_TOP, TEX_TRIGGER)]))
    ents.append(({"classname": "psq_safe"},
                 [box(ex0, ey0, 0, ex1, yb1, zc, TEX_TRIGGER)]))
    # ... and the window, where the player stands on its sill's edge, from
    # the balcony's floor up (the hall's own floor is below it).
    ents.append(({"classname": "psq_safe"},
                 [box(ex0 - 3 * t, yb0, H, ex0, yb1, zc, TEX_TRIGGER)]))
    return ents, (yb0, yb1, H, zc)


def exit_alcove(world, y1, ez, exit_map):
    """The exit alcove behind the hall's north wall at y1 (floor ez), its
    door and trigger; returns (alcove interior, entities)."""
    t = WALL
    alc = (-128, y1 + 2 * t, ez, 128, y1 + 2 * t + EXIT_DEPTH, ez + 160)
    world.extend(room(alc, doors_south=(-DOOR_HALF, DOOR_HALF, ez + DOOR_TOP),
                      tex_floor=TEX_EXIT_FLOOR))
    ey1 = alc[4]
    world.append(box(-64, ey1 - 8, ez, 64, ey1, ez + 128, TEX_EXIT_WALL))
    _tag("trigger")
    dy = alc[1] + EXIT_DOOR_IN
    ents = [
        # The door fills the alcove; shot (by the player only), it rises
        # into the ceiling and stays open.
        ({"classname": "psq_exit_door", "angle": "-1", "health": "1", "wait": "-1",
          "speed": "120", "lip": "8", "sounds": "2"},
         [box(alc[0], dy, ez, alc[3], dy + 16, alc[5], TX_EXIT_DOOR)]),
        ({"classname": "trigger_changelevel", "map": exit_map, "spawnflags": "1"},
         [box(alc[0] + 16, ey1 - 64, ez, alc[3] - 16, ey1 - 16, ez + 144, TEX_TRIGGER)]),
        ({"classname": "light", "origin": f"0 {alc[1] + 32} {ez + 120}", "light": "160"}, []),
        ({"classname": "light", "origin": f"0 {ey1 - 96} {ez + 120}", "light": "300"}, []),
    ]
    return alc, ents


def build_layout(name, exit_map=EXIT_MAP):
    """(world, ents, description) for a multi-level layout."""
    global _FIT
    _FIT = True
    try:
        return _build_layout(name, exit_map)
    finally:
        _FIT = False


def _build_layout(name, exit_map):
    if name not in _LAYOUT_FNS:
        raise ArenaError(f"unknown layout: {name}")
    T = THEMES[name]
    B = _Builder(T)
    L = _LAYOUT_FNS[name](B)
    x0, y0, x1, y1 = L["hall"]
    zlo, zhi, ez = L["zlo"], L["zhi"], L["exit_z"]
    t = WALL
    if y0 != HALL[1]:
        raise ArenaError("a layout's hall must meet the start room at y = -576")
    B.emit_blocks(L["regions"], T["inlay"])
    world = B.world
    # Shell: the lowest floor (with its grids' inlays), the ceiling, and the
    # walls in bands: a base at the main floor, the field, a frieze, the
    # field again. The south door at 0, the north at the exit's level.
    _tag("floor")
    inlays = [(r["rect"], T["inlay"].get(zlo, T["floor"])) for r in L["regions"]
              if r["origin"][2] == zlo]
    for xa, ya, xb, yb, tex in _pieces(x0 - t, y0 - t, x1 + t, y1 + t, inlays):
        world.append(box(xa, ya, zlo - t, xb, yb, zlo, T["floor"], top=tex or T["floor"]))
    _tag("ceiling")
    world.append(box(x0 - t, y0 - t, zhi, x1 + t, y1 + t, zhi + t, T["ceil"]))
    _tag("wall")
    fz = zhi - 112
    layers = [(32, T["base"]), (fz, T["wall"]), (fz + 16, T["frieze"]), (zhi, T["wall"])]
    world.extend(banded(x0 - t, y0 - t, zlo, x0, y1 + t, zhi, layers))
    # The east wall, with the balcony's window.
    esc_ents, (wy0, wy1, wz0, wz1) = escape(world, T, L["hall"], L["balcony"], zhi)
    _tag("wall")
    world.extend(banded(x1, y0 - t, zlo, x1 + t, wy0, zhi, layers))
    world.extend(banded(x1, wy1, zlo, x1 + t, y1 + t, zhi, layers))
    world.extend(banded(x1, wy0, zlo, x1 + t, wy1, wz0, layers))
    world.extend(banded(x1, wy0, wz1, x1 + t, wy1, zhi, layers))
    world.extend(wall_doors(x0, y0 - t, zlo, x1, y0, zhi, -DOOR_HALF, DOOR_HALF,
                            0, START_DOOR_TOP, layers))
    world.extend(wall_doors(x0, y1, zlo, x1, y1 + t, zhi, -DOOR_HALF, DOOR_HALF,
                            ez, ez + DOOR_TOP, layers))
    # Light panels, 4 deep and above every floor's headroom, on the walls.
    _tag("panel")
    for side, a, z in L.get("panels", ()):
        if side == "w":
            world.append(box(x0, a - 16, z, x0 + 4, a + 16, z + 64, TX_PANEL))
        elif side == "e":
            world.append(box(x1 - 4, a - 16, z, x1, a + 16, z + 64, TX_PANEL))
        elif side == "s":
            world.append(box(a - 16, y0, z, a + 16, y0 + 4, z + 64, TX_PANEL))
        else:
            world.append(box(a - 16, y1 - 4, z, a + 16, y1, z + 64, TX_PANEL))
    # The start room (arches, the lip) and the exit alcove behind its door.
    start_ents = start_room(world, T)
    exit_room, exit_ents = exit_alcove(world, y1, ez, exit_map)
    # Ceiling lamps over the fighting areas only: a fixture, and a light just
    # under it giving LIGHT_POOL to the floor below (each lamp names that
    # floor's height); between and beyond them the light falls away.
    _tag("lamp")
    zl = zhi - 40
    lamps = L["lamps"]
    for x, y, _ in lamps:
        world.append(box(x - 32, y - 32, zhi - 12, x + 32, y + 32, zhi, TX_LAMP))
    _tag("trigger")
    ents = [({"classname": "info_player_start",
              "origin": " ".join(str(v) for v in START), "angle": "90"}, [])]
    for i, r in enumerate(L["regions"]):
        ox, oy, oz = r["origin"]
        ents.append(({"classname": "psq_arena" if i == 0 else "psq_grid",
                      "origin": f"{ox} {oy} {oz}", "psq_cols": str(r["cols"]),
                      "psq_rows": str(r["rows"]), "psq_cell": str(CELL)}, []))
    ents.extend(start_ents)
    ents.extend(esc_ents)
    ents.extend(exit_ents)
    # An origin in open air in the hall, for qbsp: it fills every hull's
    # leaves that hold no entity, and with the doorway too low for hull 2 the
    # player start no longer reaches the hall in that hull (info_null is
    # removed at spawn; the game never sees it).
    ents.append(({"classname": "info_null",
                  "origin": f"{HALL_ENTRY[0]} {HALL_ENTRY[1] + 80} {HALL_ENTRY[2] + 40}"}, []))
    for x, y, z in L.get("supply", ()):
        ents.append(({"classname": "psq_supply", "origin": f"{x} {y} {z}"}, []))
    for item in L["items"]:
        cls, x, y, z = item[:4]
        keys = {"classname": cls, "origin": f"{x} {y} {z}"}
        if len(item) > 4:
            keys["spawnflags"] = str(item[4])   # MEGA (health) or BIG (ammo)
        ents.append((keys, []))
    lights = [(x, y, zl, round((zl - fz) * LIGHT_WAIT) + LIGHT_POOL, LIGHT_WAIT)
              for x, y, fz in lamps] + [l + (None,) for l in L["lights"]]
    for side, a, z in L.get("panels", ()):
        px, py = {"w": (x0 + 24, a), "e": (x1 - 24, a), "s": (a, y0 + 24), "n": (a, y1 - 24)}[side]
        lights.append((px, py, z + 32, LIGHT_PANEL, None))
    for x, y, z, lv, wait in lights:
        keys = {"classname": "light", "origin": f"{x} {y} {z}", "light": str(lv)}
        if wait is not None:
            keys["wait"] = f"{wait:g}"
        ents.append((keys, []))
    ents.extend(B.ents)
    L["exit_room"] = exit_room
    L["exit_map"] = exit_map
    L["title"] = ARENA_TITLES[name]
    L["worldkeys"] = {"light": str(LIGHT_MIN), "_minlight_dirt": "0", "_dirt": LIGHT_DIRT,
                      "_dirtscale": LIGHT_DIRT_SCALE, "_psq_rules": RULES,
                      "_psq_hall_entry": " ".join(str(v) for v in HALL_ENTRY)}
    return world, ents, L


def shots_text(L):
    """The render driver's --arena-shots script: camera stands and monster pins."""
    out = []
    for name, x, y, z, pitch, yaw in L["cameras"]:
        out.append(f"shot {name} {x} {y} {z} {pitch} {yaw}\n")
    for name, x, y, z, pitch, yaw in L.get("views", ()):
        out.append(f"view {name} {x} {y} {z} {pitch} {yaw}\n")
    for x, y, z in L["pins"]:
        out.append(f"pin {x} {y} {z}\n")
    return "".join(out)


# --- Plan drawing (--plan) ---------------------------------------------------

_HEIGHT_STOPS = ((-128, (36, 71, 122)), (-64, (61, 116, 168)), (0, (150, 158, 168)),
                 (64, (216, 190, 112)), (128, (232, 150, 74)), (192, (200, 85, 61)))


def _height_colour(z):
    stops = _HEIGHT_STOPS
    if z <= stops[0][0]:
        c = stops[0][1]
    elif z >= stops[-1][0]:
        c = stops[-1][1]
    else:
        for (za, ca), (zb, cb) in zip(stops, stops[1:]):
            if za <= z <= zb:
                f = (z - za) / (zb - za)
                c = tuple(round(a + (b - a) * f) for a, b in zip(ca, cb))
                break
    return "#%02x%02x%02x" % c


def plan_svg(name, plan, L):
    """A top-down plan: brushes coloured by the height of their top, spawn
    grids, start, pads, exit, pickups and camera stands. North is up."""
    x0, y0, x1, y1 = L["hall"]
    zhi = L["zhi"]
    vx0, vx1 = x0 - 40, x1 + (ESC_X[1] - x1 + 56 if "balcony" in L else 40)
    vy0, vy1 = -1010, y1 + 330
    W = vx1 - vx0
    H = vy1 - vy0

    def X(x):
        return x - vx0

    def Y(y):
        return vy1 - y

    s = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H + 120}" '
         f'width="{W // 2}" height="{(H + 120) // 2}" font-family="Helvetica, Arial, sans-serif">',
         f'<rect width="{W}" height="{H + 120}" fill="#15171a"/>',
         '<defs><pattern id="hatch" width="16" height="16" patternUnits="userSpaceOnUse" '
         'patternTransform="rotate(45)"><line x1="0" y1="0" x2="0" y2="16" '
         'stroke="#000" stroke-opacity="0.35" stroke-width="6"/></pattern>'
         '<marker id="arr" viewBox="0 0 10 10" refX="5" refY="5" markerWidth="5" '
         'markerHeight="5" orient="auto-start-reverse"><path d="M0 0L10 5L0 10z" '
         'fill="#fff"/></marker></defs>']

    def rect(r, fill, extra=""):
        a, b, c, d = r
        return (f'<rect x="{X(a)}" y="{Y(d)}" width="{c - a}" height="{d - b}" '
                f'fill="{fill}" {extra}/>')

    items = [p for p in plan if p["kind"] not in ("ceiling", "trigger", "clip")]
    # Floors first, then everything else by the height of its top; walls last.
    order = {"floor": 0, "wall": 3}
    items.sort(key=lambda p: (order.get(p["kind"], 1), p["z1"] if p["kind"] != "pillar" else 9999))
    for p in items:
        k = p["kind"]
        if k == "stairs-arrow":
            continue
        if k == "wall" or (k == "pillar" and p["z1"] >= zhi - 1):
            s.append(rect(p["rect"], "#3a3f47", 'stroke="#000" stroke-width="2"'))
            continue
        if k == "ramp" or "rise" in p:
            a, b, c, d = p["rect"]
            gid = f"g{len(s)}"
            horiz = p["rise"][1] == "x"
            up = p["rise"][0] == "+"
            lo_c, hi_c = _height_colour(p["zlo"]), _height_colour(p["z1"])
            x_1, y_1, x_2, y_2 = (0, 0, 1, 0) if horiz else (0, 1, 0, 0)
            if not up:
                x_1, y_1, x_2, y_2 = x_2, y_2, x_1, y_1
            s.append(f'<linearGradient id="{gid}" x1="{x_1}" y1="{y_1}" x2="{x_2}" y2="{y_2}">'
                     f'<stop offset="0" stop-color="{lo_c}"/><stop offset="1" stop-color="{hi_c}"/>'
                     f'</linearGradient>')
            s.append(rect(p["rect"], f"url(#{gid})", 'stroke="#fff" stroke-opacity="0.5" '
                          'stroke-width="2" stroke-dasharray="6 4"' if k == "bridge" else
                          'stroke="#000" stroke-width="1"'))
            continue
        fill = _height_colour(p["z1"])
        extra = 'stroke="#000" stroke-width="1"'
        s.append(rect(p["rect"], fill, extra))
        if k == "cover":
            s.append(rect(p["rect"], "url(#hatch)"))
        if k == "bridge":
            s.append(rect(p["rect"], "none", 'stroke="#fff" stroke-opacity="0.6" '
                          'stroke-width="2" stroke-dasharray="6 4"'))
        if k == "pillar":
            s.append(rect(p["rect"], "url(#hatch)"))
    # Up arrows on stairs and ramps.
    for p in plan:
        if "rise" not in p:
            continue
        a, b, c, d = p["rect"]
        cx, cy = (a + c) / 2, (b + d) / 2
        hx, hy = (c - a) * 0.35, (d - b) * 0.35
        dx, dy = {"+x": (hx, 0), "-x": (-hx, 0), "+y": (0, hy), "-y": (0, -hy)}[p["rise"]]
        s.append(f'<line x1="{X(cx - dx)}" y1="{Y(cy - dy)}" x2="{X(cx + dx)}" '
                 f'y2="{Y(cy + dy)}" stroke="#fff" stroke-width="5" marker-end="url(#arr)"/>')
    # Spawn grids.
    total = 0
    for r in L["regions"]:
        a, b, c, d = r["rect"]
        z = r["origin"][2]
        n = r["cols"] * r["rows"]
        total += n
        s.append(rect(r["rect"], "#ffffff", 'fill-opacity="0.10" stroke="#fff" '
                      'stroke-width="3" stroke-dasharray="10 6"'))
        for i in range(r["cols"]):
            for j in range(r["rows"]):
                px, py = a + CELL * i + CELL // 2, b + CELL * j + CELL // 2
                s.append(f'<circle cx="{X(px)}" cy="{Y(py)}" r="7" fill="#fff" fill-opacity="0.8"/>')
        s.append(f'<text x="{X(a) + 6}" y="{Y(d) + 26}" fill="#fff" font-size="22" '
                 f'font-weight="bold" stroke="#000" stroke-width="4" paint-order="stroke">'
                 f'{n} @ z{z:+d}</text>')
    # Start, exit.
    s.append(f'<path d="M{X(0)} {Y(-860)} l-22 40 h44 z" fill="#4cd964" stroke="#000" stroke-width="2"/>')
    s.append(f'<text x="{X(0)}" y="{Y(-930)}" text-anchor="middle" font-size="22" '
             f'fill="#4cd964">start</text>')
    er = L.get("exit_room") or (EXIT[0], EXIT[1], 0, EXIT[3], EXIT[4], 0)
    s.append(rect((er[0], er[1], er[3], er[4]), "#c03ec0", 'fill-opacity="0.75" stroke="#000"'))
    s.append(f'<text x="{X(0)}" y="{Y((er[1] + er[4]) / 2) + 10}" text-anchor="middle" '
             f'font-size="28" font-weight="bold" fill="#fff">EXIT z{L["exit_z"]:+d}</text>')
    for cls, x, y, z, *_ in L["items"]:
        s.append(f'<rect x="{X(x) - 10}" y="{Y(y) - 10}" width="20" height="20" '
                 f'transform="rotate(45 {X(x)} {Y(y)})" fill="#ffd60a" stroke="#000" stroke-width="2"/>')
    for x, y, z in L.get("supply", ()):
        s.append(f'<circle cx="{X(x)}" cy="{Y(y)}" r="11" fill="none" stroke="#4cd964" '
                 f'stroke-width="4"/>')
    for cname, x, y, z, pitch, yaw in L["cameras"] + L.get("views", []):
        ex, ey = x + 120 * math.cos(math.radians(yaw)), y + 120 * math.sin(math.radians(yaw))
        s.append(f'<line x1="{X(x)}" y1="{Y(y)}" x2="{X(ex):.0f}" y2="{Y(ey):.0f}" '
                 f'stroke="#5ac8fa" stroke-width="5" marker-end="url(#arr)"/>')
        s.append(f'<circle cx="{X(x)}" cy="{Y(y)}" r="12" fill="#5ac8fa" stroke="#000" stroke-width="2"/>')
        s.append(f'<text x="{X(x) + 16}" y="{Y(y) - 14}" font-size="22" fill="#5ac8fa" '
                 f'stroke="#000" stroke-width="4" paint-order="stroke">{cname}</text>')
    # Legend.
    ly = H + 20
    s.append(f'<text x="20" y="{ly + 28}" font-size="30" font-weight="bold" fill="#fff">'
             f'{name}: {total} spawn cells in {len(L["regions"])} grid(s)</text>')
    lx = 20
    for z in (-128, -64, 0, 64, 128, 192):
        s.append(f'<rect x="{lx}" y="{ly + 50}" width="40" height="30" fill="{_height_colour(z)}" stroke="#000"/>')
        s.append(f'<text x="{lx + 46}" y="{ly + 74}" font-size="22" fill="#ddd">z{z:+d}</text>')
        lx += 130
    s.append(f'<text x="{lx + 10}" y="{ly + 74}" font-size="22" fill="#ddd">hatched: cover / '
             f'pillar · dashed: spawn grid / bridge · arrow: up · blue: camera · yellow: pickup · green ring: resupply</text>')
    s.append("</svg>\n")
    return "\n".join(s)


def _fmt_point(p):
    return "( " + " ".join(str(int(v)) for v in p) + " )"


def map_text(world, ents, wad_name, message="psQuake Arena", worldkeys_extra=None):
    out = ["// psQuake arena, generated by tools/generate_arena.py. Do not edit.\n"]

    def brush_text(faces):
        lines = ["{\n"]
        for p0, p1, p2, tex in faces:
            (xo, yo), (xs, ys) = getattr(tex, "off", (0, 0)), getattr(tex, "scale", (1, 1))
            lines.append(f"{_fmt_point(p0)} {_fmt_point(p1)} {_fmt_point(p2)} {tex} "
                         f"{xo:g} {yo:g} 0 {xs:g} {ys:g}\n")
        lines.append("}\n")
        return "".join(lines)

    worldkeys = {"classname": "worldspawn", "message": message,
                 "worldtype": "2", "sounds": "0", "light": "40",
                 "wad": wad_name}
    worldkeys.update(worldkeys_extra or {})
    out.append("{\n")
    for k, v in worldkeys.items():
        out.append(f'"{k}" "{v}"\n')
    for faces in world:
        out.append(brush_text(faces))
    out.append("}\n")
    for keys, brushes in ents:
        out.append("{\n")
        for k, v in keys.items():
            if '"' in k + v or "\n" in k + v:
                raise ArenaError(f"bad entity key or value: {k!r} {v!r}")
            out.append(f'"{k}" "{v}"\n')
        for faces in brushes:
            out.append(brush_text(faces))
        out.append("}\n")
    return "".join(out)


# --- Textures from LibreQuake's pak0 -----------------------------------------


def read_pak_dir(f, size):
    head = f.read(12)
    if len(head) != 12:
        raise ArenaError("pak: short header")
    magic, dirofs, dirlen = struct.unpack("<4sii", head)
    if magic != b"PACK":
        raise ArenaError("pak: not a PACK file")
    if dirofs < 12 or dirlen < 0 or dirlen % 64 or dirlen // 64 > PAK_MAX_ENTRIES \
            or dirofs + dirlen > size:
        raise ArenaError("pak: bad directory")
    f.seek(dirofs)
    raw = f.read(dirlen)
    entries = {}
    for i in range(0, dirlen, 64):
        name = raw[i:i + 56].split(b"\0", 1)[0].decode("latin-1")
        ofs, ln = struct.unpack("<ii", raw[i + 56:i + 64])
        if ofs < 12 or ln < 0 or ofs + ln > size:
            raise ArenaError(f"pak: entry {name!r} outside the file")
        entries[name] = (ofs, ln)
    return entries


def bsp_miptex(bsp):
    """{name: miptex bytes} for every fully embedded texture in a BSP29."""
    if len(bsp) < 4 + 15 * 8:
        raise ArenaError("bsp: short header")
    version = struct.unpack_from("<i", bsp, 0)[0]
    if version != 29:
        return {}
    ofs, ln = struct.unpack_from("<ii", bsp, 4 + 2 * 8)
    if ofs < 0 or ln < 4 or ofs + ln > len(bsp):
        raise ArenaError("bsp: bad texture lump")
    lump = bsp[ofs:ofs + ln]
    (count,) = struct.unpack_from("<i", lump, 0)
    if count < 0 or 4 + 4 * count > ln:
        raise ArenaError("bsp: bad texture count")
    out = {}
    for i in range(count):
        (mofs,) = struct.unpack_from("<i", lump, 4 + 4 * i)
        if mofs < 0 or mofs + MIPTEX_HEADER > ln:
            continue
        name = lump[mofs:mofs + 16].split(b"\0", 1)[0].decode("latin-1")
        w, h, *offs = struct.unpack_from("<II4I", lump, mofs + 16)
        if not (0 < w <= 1024 and 0 < h <= 1024 and w % 16 == 0 and h % 16 == 0):
            continue
        want = [MIPTEX_HEADER]
        for lv in range(1, 4):
            want.append(want[-1] + (w >> (lv - 1)) * (h >> (lv - 1)))
        total = want[-1] + (w >> 3) * (h >> 3)
        if list(offs) != want or mofs + total > ln:
            continue
        out[name] = lump[mofs:mofs + total]
    return out


def collect_textures(pak_path, names):
    """{name: (map, miptex bytes)} from the first pak0 map (by name) holding it."""
    size = os.path.getsize(pak_path)
    found = {}
    with open(pak_path, "rb") as f:
        entries = read_pak_dir(f, size)
        maps = sorted(n for n in entries
                      if n.startswith("maps/") and n.endswith(".bsp")
                      and not n.startswith("maps/b_"))
        for m in maps:
            ofs, ln = entries[m]
            if ln > BSP_MAX_BYTES:
                continue
            f.seek(ofs)
            tex = bsp_miptex(f.read(ln))
            for n in names:
                if n not in found and n in tex:
                    found[n] = (m, tex[n])
            if len(found) == len(names):
                break
    missing = sorted(set(names) - set(found))
    if missing:
        raise ArenaError(f"textures not found in {pak_path}: {', '.join(missing)}")
    return found


def wad2(textures):
    """A WAD2 holding the given {name: miptex bytes}, sorted by name."""
    body = bytearray()
    dirents = []
    for name in sorted(textures):
        data = textures[name]
        ofs = 12 + len(body)
        body += data
        nm = name.encode("latin-1")
        if len(nm) > 15:
            raise ArenaError(f"texture name too long: {name}")
        dirents.append(struct.pack("<iiibbh16s", ofs, len(data), len(data),
                                   0x44, 0, 0, nm))
    header = struct.pack("<4sii", b"WAD2", len(dirents), 12 + len(body))
    return header + bytes(body) + b"".join(dirents)


def _write_atomic(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--pak", required=True, help="LibreQuake id1/pak0.pak")
    ap.add_argument("--map", required=True, help="output .map")
    ap.add_argument("--wad", required=True, help="output .wad (same folder as --map)")
    ap.add_argument("--break", dest="break_kind", choices=BREAK_KINDS,
                    help="write a deliberately broken map (validator tests)")
    ap.add_argument("--arena", choices=[a[0] for a in ARENAS],
                    help="one of the shipped arenas: its layout and exit map")
    ap.add_argument("--layout", choices=LAYOUTS, default="hall",
                    help="hall (the test hall, the default), pit, terraces or towers")
    ap.add_argument("--exit-map", default=None,
                    help=f"where the exit leads (default {EXIT_MAP}, or the --arena's)")
    ap.add_argument("--plan", help="also write a top-down SVG plan of the layout")
    ap.add_argument("--shots", help="also write the render driver's --arena-shots script")
    ap.add_argument("--print-hashes", action="store_true",
                    help="print each texture's source map and SHA-256")
    args = ap.parse_args(argv)
    exit_map = args.exit_map or EXIT_MAP
    if args.arena:
        _, args.layout, arena_exit = next(a for a in ARENAS if a[0] == args.arena)
        exit_map = args.exit_map or arena_exit

    try:
        if args.layout == "hall" and exit_map != EXIT_MAP:
            raise ArenaError(f"the hall's exit is {EXIT_MAP}")
        if os.path.dirname(os.path.abspath(args.map)) != os.path.dirname(os.path.abspath(args.wad)):
            raise ArenaError("--map and --wad must be in the same folder")
        names = sorted(TEXTURES)
        found = collect_textures(args.pak, names)
        bad = []
        for n in names:
            src, data = found[n]
            digest = hashlib.sha256(data).hexdigest()
            if args.print_hashes:
                print(f"{n} {src} {digest}")
            if (src, digest) != TEXTURES[n]:
                bad.append(f"{n} from {src} {digest}, expected {' '.join(TEXTURES[n])}")
            if n in FRAMED and struct.unpack_from("<II", data, 16) != FRAMED[n]:
                bad.append(f"{n} is {struct.unpack_from('<II', data, 16)}, FRAMED says "
                           f"{FRAMED[n]}")
        if bad:
            raise ArenaError("texture hash mismatch:\n  " + "\n  ".join(bad))
        global _PLAN
        _PLAN = []
        world, ents = build(args.break_kind, args.layout, exit_map)
        plan, _PLAN = _PLAN, None
        L = hall_description() if args.layout == "hall" else \
            build_layout(args.layout, exit_map)[2]
        text = map_text(world, ents, os.path.basename(args.wad), ARENA_TITLES[args.layout],
                        L.get("worldkeys"))
        if args.plan:
            _write_atomic(args.plan, plan_svg(args.layout, plan, L).encode("utf-8"))
        if args.shots:
            _write_atomic(args.shots, shots_text(L).encode("ascii"))
        _write_atomic(args.wad, wad2({n: found[n][1] for n in names}))
        _write_atomic(args.map, text.encode("ascii"))
    except (ArenaError, OSError) as e:
        print(f"generate_arena: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
