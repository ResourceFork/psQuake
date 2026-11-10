#!/usr/bin/env python3
"""Generate a psQuake arena: its .map and the WAD2 of its textures.

The app ships four arenas (ARENAS). New Game opens psqarena, the Pit; its
north exit leads to psqterraces, Terraces, whose exit leads to psqtowers,
Towers, whose exit leads to the first campaign map. psqtesthall, the original
one-floor hall, is for the tests: `map psqtesthall` loads it, no exit leads
there. Every arena has the same south antechamber: the player starts there
and crosses one of four skill pads (trigger_setskill 0-3) into the hall.

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

Python 3 standard library only. Output is deterministic.
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
    "floor_olive_c": ("maps/lq_e1m7.bsp",
        "eb5199f47a302d39acfc8a33f9ccc9afd81dfbf5939b7e38078064e7d8ba1220"),
    "floor_grey_c": ("maps/dev.bsp",
        "d9967b81011af58e0b7f74ef7f9cc0b3fe86a7e65bf4f3b8d7d5a8227c428e31"),
    "floor_orange_c": ("maps/dev.bsp",
        "9b33f2d9c0885499638ed8efd86bdf138738ae6ef5cfa4cc4821f2b8afc4e699"),
    "floor_red_c": ("maps/dev.bsp",
        "00339b739aa0808f8ded58f081ead5f435762a9fcf9516e1f2045e695b3c731c"),
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
    # Not used on any face: ericw-tools 2.x reserves miptex slot 0 for it.
    "skip": ("maps/dev.bsp",
        "90776c8a801008cff4aceea7eaa79d1376fcc61a472357f31048fb477c28cc23"),
}
TEX_GRID = "tek_flr3"
TEX_FLOOR = "met_brn_flat"
TEX_WALL = "met_blu_rect"
TEX_TRIM = "met_blu_trim32"
TEX_CEIL = "met_gry_slat"
TEX_PADS = ("floor_olive_c", "floor_grey_c", "floor_orange_c", "floor_red_c")
TEX_EXIT_FLOOR = "telepad1"
TEX_EXIT_WALL = "tele_frame1"
TEX_TRIGGER = "trigger"
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


def _tag(kind):
    global _KIND
    _KIND = kind


def box(x0, y0, z0, x1, y1, z1, tex, top=None):
    """An axis-aligned box brush; top overrides the texture of its +z face."""
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
        faces.append((p0, p1, p2, t))
    return faces


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
    brushes = [box(x0 - t, y0 - t, z0 - t, x1 + t, y1 + t, z0, tex_floor)]
    _tag("ceiling")
    brushes.append(box(x0 - t, y0 - t, z1, x1 + t, y1 + t, z1 + t, tex_ceil))
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
            world.append(box(cx - 32, cy - 32, 0, cx + 32, cy + 32, HALL[5], TEX_TRIM))

    # Antechamber and exit alcove. Each sits 2 * WALL from the hall, so its
    # doorway wall slab lies flush against the hall's and the two doorways
    # form one sealed passage.
    world.extend(room(ANTE, doors_north=(-DOOR_HALF, DOOR_HALF, DOOR_TOP)))
    world.extend(room(EXIT, doors_south=(-DOOR_HALF, DOOR_HALF, DOOR_TOP),
                      tex_floor=TEX_EXIT_FLOOR))
    # The exit alcove's back wall face: a teleporter frame.
    ex0, _, _, ex1, ey1, _ = EXIT
    world.append(box(-64, ey1 - 8, 0, 64, ey1, 128, TEX_EXIT_WALL))

    # Skill pads: raised plates under the triggers, side by side across the
    # antechamber so the way to the hall crosses one of them.
    _tag("pad")
    pads = []
    pad_y = -720
    for skill, cx in enumerate((-240, -80, 80, 240)):
        tex = TEX_PADS[skill]
        if break_kind == "missing-texture" and skill == 0:
            tex = TEX_MISSING
        world.append(box(cx - 72, pad_y - 48, 0, cx + 72, pad_y + 48, 8, TEX_FLOOR, top=tex))
        pads.append((cx, skill))

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
    for cx, skill in pads:
        ents.append(({"classname": "trigger_setskill", "message": str(skill)},
                     [box(cx - 72, pad_y - 48, 8, cx + 72, pad_y + 48, 72, TEX_TRIGGER)]))
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
    # one in the exit alcove. All white: a coloured light makes light write
    # a .lit beside the BSP, which the app would need to bundle too.
    for x in (-576, -288, 0, 288, 576):
        for y in (-384, 0, 384):
            ents.append(({"classname": "light", "origin": f"{x} {y} 208",
                          "light": "450"}, []))
    for x in (-160, 160):
        ents.append(({"classname": "light", "origin": f"{x} -800 152",
                      "light": "350"}, []))
    ents.append(({"classname": "light", "origin": "0 736 120", "light": "300"}, []))
    return world, ents


# --- Prototype layouts (--layout) --------------------------------------------
#
# Candidate halls with height and cover, for the human to choose from; the
# default "hall" above is still the arena the app ships. Each candidate keeps
# the antechamber, its pads and the start, opens the hall's south wall on
# the antechamber at z = 0, and puts the exit alcove on the north wall at
# the level the layout chooses.
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

# Shared accents. Hazard stripes mark step and ledge edges; tread plate tops
# stairs and ramps; lamps and light panels are fullbright.
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
            "pillar": "aqsupp01", "cap": "met_brn_trim64",
            "inlay": {-128: "t_flor1b", 0: "met_brn_stile", 128: "t_blok04"}},
    "terraces": {"wall": "met_brn_rect", "base": "met_brn_trim32", "frieze": "met_brn_trim16",
                 "ceil": "met_gry_slat", "floor": "met_dbrn_flat", "cover": "aqtrim01",
                 "pillar": "aqmetl07", "cap": "met_brn_trim64",
                 "inlay": {0: "met_brn_stile", 96: "met_grn_flat", 192: "met_lbrn_slat"}},
    "towers": {"wall": "met_blu_rect", "base": "met_blu_trim32", "frieze": "met_blu_trim16",
               "ceil": "met_blu_slat", "floor": "t_flor2a", "cover": "met_teal_trim32",
               "pillar": "met_blu_sqr", "cap": "met_blu_trim64",
               "inlay": {0: "met_blu_flat", 96: "t_flor1b", 192: "met_blu_sqr"}},
}
COVER_H = 40     # low cover: the player's eye (46 up) sees over it
CRATE_H = 64     # a crate hides a standing player


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
        self.world.append(box(x0, y0, z, x1, y1, z + h, tex or self.T["cover"], top=TX_HAZARD))

    def crate(self, x0, y0, x1, y1, z, h=CRATE_H):
        _tag("cover")
        self.world.append(box(x0, y0, z, x1, y1, z + h, TX_CRATE, top=TX_CRATE_TOP))

    def pillar(self, cx, cy, half, z0, z1):
        """A pillar with a base and a cap 8 units wider all round."""
        _tag("pillar")
        T = self.T
        self.world.append(box(cx - half, cy - half, z0, cx + half, cy + half, z1, T["pillar"]))
        self.world.append(box(cx - half - 8, cy - half - 8, z0, cx + half + 8, cy + half + 8,
                              z0 + 16, T["cap"]))
        self.world.append(box(cx - half - 8, cy - half - 8, z1 - 16, cx + half + 8,
                              cy + half + 8, z1, T["cap"]))

    def torch(self, x, y, z):
        self.ents.append(({"classname": "light_torch_small_walltorch",
                           "origin": f"{x} {y} {z}"}, []))

    def flame(self, x, y, z):
        self.ents.append(({"classname": "light_flame_large_yellow",
                           "origin": f"{x} {y} {z}"}, []))

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
    balconies at 128 along the west and east walls, each reached by ramps
    along the end walls to corner landings."""
    L = {"hall": (-768, -576, 768, 576), "zlo": -128, "zhi": 352, "exit_z": 0}
    _tag("block")
    for b in ((-768, -576, 768, -256), (-768, 256, 768, 576),
              (-768, -256, -384, 256), (384, -256, 768, 256)):
        B.block(b[0], b[1], -128, b[2], b[3], 0, "t_wall2b", "met_brn_tile2")
    for x0, x1 in ((-768, -528), (528, 768)):
        B.block(x0, -320, 0, x1, 320, 128, "t_wall3a", "t_blok03", band="met_brn_trim16")
    for x0, x1 in ((-768, -640), (640, 768)):
        for y0, y1 in ((-576, -320), (320, 576)):
            B.block(x0, y0, 0, x1, y1, 128, "t_wall3a", "t_blok03", band="met_brn_trim16")
    _tag("ramp")
    B.add(ramp(-384, -128, -256, 256, -128, -128, 0, "+y", "aqsupp03", top=TX_TREAD))
    B.add(ramp(256, -256, 384, 128, -128, -128, 0, "-y", "aqsupp03", top=TX_TREAD))
    for y0, y1 in ((-576, -448), (448, 576)):
        B.add(ramp(-640, y0, -384, y1, 0, 0, 128, "-x", "aqsupp03", top=TX_TREAD))
        B.add(ramp(384, y0, 640, y1, 0, 0, 128, "+x", "aqsupp03", top=TX_TREAD))
    _tag("bridge")
    B.add(box(-64, -256, -16, 64, 256, 0, "met_brn_trim16", top=TX_GRATE))
    # Railings round the pit lip (32: you can jump them, monsters cannot)
    # and the balcony parapets.
    for x0, x1 in ((-256, -64), (64, 384)):
        B.cover(x0, 256, x1, 272, 0, h=32)
    for x0, x1 in ((-384, -64), (64, 256)):
        B.cover(x0, -272, x1, -256, 0, h=32)
    B.cover(-400, -256, -384, 128, 0, h=32)
    B.cover(384, -128, 400, 256, 0, h=32)
    for y0, y1 in ((-320, -224), (-96, 96), (224, 320)):
        B.cover(-544, y0, -528, y1, 128, h=32)
        B.cover(528, y0, 544, y1, 128, h=32)
    # Low walls by the entrance.
    B.cover(-320, -448, -160, -416, 0)
    B.cover(160, -448, 320, -416, 0)
    for x in (-448, 448):
        B.pillar(x, -352, 32, 0, 352)
    L["regions"] = [region(0, 0, -128, 5, 5),
                    region(-240, 424, 0, 3, 3), region(240, 424, 0, 3, 3),
                    region(-456, 0, 0, 1, 5), region(456, 0, 0, 1, 5),
                    region(-656, 0, 128, 2, 4), region(656, 0, 128, 2, 4)]
    L["items"] = [("weapon_supershotgun", -352, -528, 16), ("weapon_nailgun", -352, -240, -112),
                  ("item_shells", 320, -528, 16), ("item_spikes", 288, 192, -112),
                  ("item_health", -736, -288, 144), ("item_armor1", 688, 240, 144)]
    # The pit lit from below and by torches on its walls; the ring by the
    # ceiling lamps, a little dimmer than the other arenas.
    L["lights"] = [(x, y, -40, 280) for x in (-192, 192) for y in (-128, 128)]
    L["lamp_add"] = 210
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
                    ("cover", -240, -480, 0, 4, 90)]
    L["views"] = [("overview", 0, -560, 320, 38, 90)]
    L["pins"] = [(-144, -48, -128), (144, 144, -128), (-456, 96, 0), (240, 424, 0),
                 (656, 96, 128), (-240, 520, 0)]
    L["blurb"] = ("The Pit: a ring round a sunken pit, crossed by a bridge, with balconies "
                  "on both side walls. Monsters climb out of the pit; you fight from above.")
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
        B.add(stairs(x0, -224, x1, -64, 0, 96, "+y"))
    for x0, x1 in ((-416, -256), (256, 416)):
        B.add(stairs(x0, 224, x1, 384, 96, 192, "+y"))
    # Parapets on the tier edges (32: cover for whoever holds the edge).
    for x0, x1 in ((-512, -256), (256, 512)):
        B.cover(x0, -64, x1, -48, 96, h=32)
    for x0, x1 in ((-736, -480), (480, 736)):
        B.cover(x0, 384, x1, 400, 192, h=32)
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
                  ("item_health", -720, 240, 112), ("item_armor1", 704, 792, 208)]
    L["lights"] = []
    L["lamp_add"] = 240
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
                    ("cover", -100, -352, 0, 4, 180)]
    L["views"] = [("overview", 0, -560, 400, 34, 90)]
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
    B.add(stairs(-96, -224, 96, -64, 0, 96, "+y"))
    B.add(stairs(-352, 32, -192, 160, 0, 96, "+x"))
    B.add(stairs(192, 32, 352, 160, 0, 96, "-x"))
    # Ground: low walls round the grids and by the door.
    B.cover(-400, -320, -368, -160, 0)
    B.cover(368, -320, 400, -160, 0)
    B.cover(-96, -416, 96, -384, 0)
    B.cover(-320, -480, -288, -352, 0)
    B.cover(288, -480, 320, -352, 0)
    B.cover(-456, 400, -424, 560, 0)
    B.cover(424, 400, 456, 560, 0)
    # Plinth: low walls on its north and south edges, the bridge and stairs open.
    for x0, x1 in ((-192, -80), (80, 192)):
        B.cover(x0, 224, x1, 240, 96)
    for x0, x1 in ((-192, -112), (112, 192)):
        B.cover(x0, -64, x1, -48, 96)
    # Towers: parapets on their inner edges; catwalk railings by the towers.
    B.cover(-528, 400, -512, 688, 192)
    B.cover(512, 400, 528, 688, 192)
    for x0, x1 in ((-512, -384), (384, 512)):
        B.cover(x0, 704, x1, 720, 192, h=32)
    for x in (-288, 288):
        B.pillar(x, 768, 16, 0, 176)
    L["regions"] = [region(0, 528, 0, 8, 3), region(-576, -336, 0, 3, 3),
                    region(576, -336, 0, 3, 3), region(-480, 144, 0, 2, 3),
                    region(480, 144, 0, 2, 3), region(0, 96, 96, 3, 2),
                    region(-640, 624, 192, 2, 3), region(640, 624, 192, 2, 3)]
    L["items"] = [("weapon_supershotgun", -720, -528, 16), ("weapon_nailgun", 200, 752, 208),
                  ("item_shells", 688, -528, 16), ("item_spikes", -232, 752, 208),
                  ("item_health", 0, -40, 112), ("item_armor1", -720, 400, 208)]
    L["lights"] = [(x, 768, 112, 250) for x in (-160, 160)]
    L["lamp_add"] = 260
    for s in (-1, 1):
        B.torch(s * 504, 560, 120)
        B.torch(s * 200, 224, 64)
        B.torch(s * 144, 824, 96)
        B.flame(s * 752, 816, 200)
    L["panels"] = [("w", -320, 320), ("e", -320, 320), ("s", -400, 320), ("s", 400, 320),
                   ("n", -400, 300), ("n", 400, 300)]
    L["cameras"] = [("start", 0, -528, 0, -6, 90), ("perch", 640, 432, 192, 16, 228),
                    ("cover", -336, -240, 0, 4, 180), ("catwalk", 0, 768, 192, 22, 270)]
    L["views"] = [("overview", 0, -560, 420, 32, 90)]
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


def build_layout(name, exit_map=EXIT_MAP):
    """(world, ents, description) for a multi-level layout."""
    if name not in _LAYOUT_FNS:
        raise ArenaError(f"unknown layout: {name}")
    T = THEMES[name]
    B = _Builder(T)
    L = _LAYOUT_FNS[name](B)
    x0, y0, x1, y1 = L["hall"]
    zlo, zhi, ez = L["zlo"], L["zhi"], L["exit_z"]
    t = WALL
    if y0 != HALL[1]:
        raise ArenaError("a layout's hall must meet the antechamber at y = -576")
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
    world.extend(banded(x1, y0 - t, zlo, x1 + t, y1 + t, zhi, layers))
    world.extend(wall_doors(x0, y0 - t, zlo, x1, y0, zhi, -DOOR_HALF, DOOR_HALF,
                            0, DOOR_TOP, layers))
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
    # Antechamber, pads, start: as in the test hall.
    world.extend(room(ANTE, doors_north=(-DOOR_HALF, DOOR_HALF, DOOR_TOP)))
    exit_room = (-128, y1 + 2 * t, ez, 128, y1 + 2 * t + 256, ez + 160)
    world.extend(room(exit_room, doors_south=(-DOOR_HALF, DOOR_HALF, ez + DOOR_TOP),
                      tex_floor=TEX_EXIT_FLOOR))
    ey1 = exit_room[4]
    world.append(box(-64, ey1 - 8, ez, 64, ey1, ez + 128, TEX_EXIT_WALL))
    _tag("pad")
    pad_y = -720
    pads = []
    for skill, cx in enumerate((-240, -80, 80, 240)):
        world.append(box(cx - 72, pad_y - 48, 0, cx + 72, pad_y + 48, 8, TEX_FLOOR,
                         top=TEX_PADS[skill]))
        pads.append((cx, skill))
    # Ceiling lamps: a fixture about every 384 units, a light just under it
    # bright enough for the main floor.
    _tag("lamp")
    nx, ny = -(-(x1 - x0) // 384), -(-(y1 - y0) // 384)
    zl = zhi - 40
    lamps = [(x0 + (x1 - x0) * (2 * i + 1) // (2 * nx),
              y0 + (y1 - y0) * (2 * j + 1) // (2 * ny)) for i in range(nx) for j in range(ny)]
    for x, y in lamps:
        world.append(box(x - 32, y - 32, zhi - 12, x + 32, y + 32, zhi, TX_LAMP))
    _tag("trigger")
    ents = [({"classname": "info_player_start", "origin": "0 -896 24", "angle": "90"}, [])]
    for i, r in enumerate(L["regions"]):
        ox, oy, oz = r["origin"]
        ents.append(({"classname": "psq_arena" if i == 0 else "psq_grid",
                      "origin": f"{ox} {oy} {oz}", "psq_cols": str(r["cols"]),
                      "psq_rows": str(r["rows"]), "psq_cell": str(CELL)}, []))
    for cx, skill in pads:
        ents.append(({"classname": "trigger_setskill", "message": str(skill)},
                     [box(cx - 72, pad_y - 48, 8, cx + 72, pad_y + 48, 72, TEX_TRIGGER)]))
    ents.append(({"classname": "trigger_changelevel", "map": exit_map, "spawnflags": "1"},
                 [box(exit_room[0] + 16, ey1 - 64, ez, exit_room[3] - 16, ey1 - 16,
                      ez + 144, TEX_TRIGGER)]))
    for cls, x, y, z in L["items"]:
        ents.append(({"classname": cls, "origin": f"{x} {y} {z}"}, []))
    lights = [(x, y, zl, zl + L.get("lamp_add", 250)) for x, y in lamps] + L["lights"]
    for side, a, z in L.get("panels", ()):
        px, py = {"w": (x0 + 24, a), "e": (x1 - 24, a), "s": (a, y0 + 24), "n": (a, y1 - 24)}[side]
        lights.append((px, py, z + 32, 150))
    for x, y, z, lv in lights:
        ents.append(({"classname": "light", "origin": f"{x} {y} {z}", "light": str(lv)}, []))
    ents.extend(B.ents)
    for x in (-160, 160):
        ents.append(({"classname": "light", "origin": f"{x} -800 152", "light": "350"}, []))
    ents.append(({"classname": "light", "origin": f"0 {exit_room[1] + 128} {ez + 120}",
                  "light": "300"}, []))
    L["exit_room"] = exit_room
    L["exit_map"] = exit_map
    L["title"] = ARENA_TITLES[name]
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
    vx0, vx1 = x0 - 40, x1 + 40
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

    items = [p for p in plan if p["kind"] not in ("ceiling", "trigger")]
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
    # Pads, start, exit.
    for i, cx in enumerate((-240, -80, 80, 240)):
        col = ("#7d8a3a", "#9a9a9a", "#d07a2a", "#b03030")[i]
        s.append(rect((cx - 72, -768, cx + 72, -672), col, 'stroke="#000"'))
        s.append(f'<text x="{X(cx)}" y="{Y(-720) + 8}" text-anchor="middle" font-size="22" '
                 f'fill="#000">skill {i}</text>')
    s.append(f'<path d="M{X(0)} {Y(-860)} l-22 40 h44 z" fill="#4cd964" stroke="#000" stroke-width="2"/>')
    s.append(f'<text x="{X(0)}" y="{Y(-930)}" text-anchor="middle" font-size="22" '
             f'fill="#4cd964">start</text>')
    er = L.get("exit_room") or (EXIT[0], EXIT[1], 0, EXIT[3], EXIT[4], 0)
    s.append(rect((er[0], er[1], er[3], er[4]), "#c03ec0", 'fill-opacity="0.75" stroke="#000"'))
    s.append(f'<text x="{X(0)}" y="{Y((er[1] + er[4]) / 2) + 10}" text-anchor="middle" '
             f'font-size="28" font-weight="bold" fill="#fff">EXIT z{L["exit_z"]:+d}</text>')
    for cls, x, y, z in L["items"]:
        s.append(f'<rect x="{X(x) - 10}" y="{Y(y) - 10}" width="20" height="20" '
                 f'transform="rotate(45 {X(x)} {Y(y)})" fill="#ffd60a" stroke="#000" stroke-width="2"/>')
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
             f'pillar · dashed: spawn grid / bridge · arrow: up · blue: camera · yellow: pickup</text>')
    s.append("</svg>\n")
    return "\n".join(s)


def _fmt_point(p):
    return "( " + " ".join(str(int(v)) for v in p) + " )"


def map_text(world, ents, wad_name, message="psQuake Arena"):
    out = ["// psQuake arena, generated by tools/generate_arena.py. Do not edit.\n"]

    def brush_text(faces):
        lines = ["{\n"]
        for p0, p1, p2, tex in faces:
            lines.append(f"{_fmt_point(p0)} {_fmt_point(p1)} {_fmt_point(p2)} {tex} 0 0 0 1 1\n")
        lines.append("}\n")
        return "".join(lines)

    worldkeys = {"classname": "worldspawn", "message": message,
                 "worldtype": "2", "sounds": "0", "light": "40",
                 "wad": wad_name}
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
        if bad:
            raise ArenaError("texture hash mismatch:\n  " + "\n  ".join(bad))
        global _PLAN
        _PLAN = []
        world, ents = build(args.break_kind, args.layout, exit_map)
        plan, _PLAN = _PLAN, None
        L = hall_description() if args.layout == "hall" else \
            build_layout(args.layout, exit_map)[2]
        text = map_text(world, ents, os.path.basename(args.wad), ARENA_TITLES[args.layout])
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
