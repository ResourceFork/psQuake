#!/usr/bin/env python3
"""Generate the psQuake arena: psqarena.map and the WAD2 of its textures.

The arena is the map New Game opens. Process monsters appear on a floor grid
of 11 x 7 cells, 96 units each (room for Quake's largest walking hull, 64 x 64).
The player starts in a south antechamber, crosses one of four skill pads
(trigger_setskill 0-3) into the hall, and leaves through the north exit
(trigger_changelevel, no intermission) to the first campaign map. Pickups sit
in the hall's margins, outside the grid. The psq_arena point entity marks the
grid for the QuakeC controller:

    origin    grid centre, on the floor
    psq_cols  cells along x       psq_rows  cells along y
    psq_cell  cell size in units

Textures are copied, as Quake miptex, out of LibreQuake's own maps in its
pak0.pak (BSD-3, see LibreQuake's docs/COPYING), so no texture is downloaded
and none is id Software's. Each copied texture is checked against a pinned
SHA-256.

    generate_arena.py --pak data/librequake/id1/pak0.pak \\
        --map build/arena/psqarena.map --wad build/arena/psqarena.wad

--break KIND writes a deliberately broken map, for the validator's tests:
missing-texture, leak, no-exit, cell-in-brush, cell-corner.

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

BREAK_KINDS = ("missing-texture", "leak", "no-exit", "cell-in-brush", "cell-corner")

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


def build(break_kind=None, layout="hall"):
    """Return (worldspawn brushes, [(entity keys, brushes)]) for the arena."""
    if layout != "hall":
        if break_kind is not None:
            raise ArenaError("--break needs the hall layout")
        world, ents, _ = build_layout(layout)
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
    whose top is z1. The footprint's length must split into equal treads."""
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
            out.append(box(x0, a, z0, x1, b, ztop, tex or TEX_TRIM, top=top or TEX_FLOOR))
        else:
            a, b = (x0 + i * tread, x0 + (i + 1) * tread) if sign == "+" else \
                   (x1 - (i + 1) * tread, x1 - i * tread)
            out.append(box(a, y0, z0, b, y1, ztop, tex or TEX_TRIM, top=top or TEX_FLOOR))
    if _PLAN is not None:
        _PLAN.append({"kind": "stairs-arrow", "rect": (x0, y0, x1, y1), "z0": z0,
                      "z1": z1, "zlo": z0, "rise": rise})
    return out


def wall_doors(x0, y0, z0, x1, y1, z1, lo, hi, zb, zt, tex):
    """A wall slab along x with a doorway x [lo, hi], z [zb, zt]."""
    out = [box(x0, y0, z0, lo, y1, z1, tex), box(hi, y0, z0, x1, y1, z1, tex),
           box(lo, y0, zt, hi, y1, z1, tex)]
    if zb > z0:
        out.append(box(lo, y0, z0, hi, y1, zb, tex))
    return out


def region(cx, cy, z, cols, rows):
    """A spawn grid: centre, floor height, cells along x and y."""
    return {"origin": (cx, cy, z), "cols": cols, "rows": rows,
            "rect": (cx - cols * CELL // 2, cy - rows * CELL // 2,
                     cx + cols * CELL // 2, cy + rows * CELL // 2)}


def _terraces(w):
    """Three terraces rising south to north (0, 96, 192); the exit is on the
    top one, whose open front is the perch. Stairways between tiers, low
    walls and crates for cover, parapets on the tier edges' outer thirds."""
    L = {"hall": (-768, -576, 768, 832), "zlo": 0, "zhi": 448, "exit_z": 192}
    _tag("block")
    w.append(box(-768, -64, 0, 768, 384, 96, TEX_WALL, top=TEX_GRID))
    w.append(box(-768, 384, 0, 768, 832, 192, TEX_WALL, top=TEX_FLOOR))
    _tag("stairs")
    for x0, x1 in ((-96, 96), (-768, -608), (608, 768)):
        w.extend(stairs(x0, -224, x1, -64, 0, 96, "+y"))
    for x0, x1 in ((-416, -256), (256, 416)):
        w.extend(stairs(x0, 224, x1, 384, 96, 192, "+y"))
    _tag("cover")
    for x0, x1 in ((-512, -256), (256, 512)):
        w.append(box(x0, -64, 96, x1, -48, 128, TEX_TRIM))
    for x0, x1 in ((-736, -480), (480, 736)):
        w.append(box(x0, 384, 192, x1, 400, 224, TEX_TRIM))
    for x0, x1 in ((-160, -128), (128, 160)):
        w.append(box(x0, -432, 0, x1, -272, 48, TEX_TRIM))
    for x0, x1 in ((-128, -64), (64, 128)):
        w.append(box(x0, 128, 96, x1, 192, 160, TEX_TRIM))
    _tag("pillar")
    for x, y in ((-672, -400), (672, -400), (-288, 752), (288, 752)):
        w.append(box(x - 32, y - 32, 0, x + 32, y + 32, 448, TEX_TRIM))
    L["regions"] = [region(-384, -336, 0, 4, 3), region(384, -336, 0, 4, 3),
                    region(-432, 80, 96, 5, 2), region(432, 80, 96, 5, 2),
                    region(-528, 592, 192, 4, 3), region(528, 592, 192, 4, 3)]
    L["items"] = [("weapon_supershotgun", -720, -528, 16), ("weapon_nailgun", 688, -528, 16),
                  ("item_shells", -720, -288, 16), ("item_spikes", 688, -288, 16),
                  ("item_health", -720, 240, 112), ("item_armor1", 704, 792, 208)]
    L["lights"] = []
    L["cameras"] = [("start", 0, -528, 0, -10, 90), ("perch", 0, 432, 192, 28, 270),
                    ("terrace", -720, 40, 96, 8, 10)]
    L["views"] = [("overview", 0, -560, 400, 34, 90)]
    L["pins"] = [(-432, -336, 0), (336, -240, 0), (-336, 128, 96), (432, 32, 96),
                 (-480, 592, 192), (576, 688, 192)]
    L["blurb"] = ("Terraces: three tiers rising to the exit. You start at the bottom; "
                  "monsters hold the high ground and come down five stairways.")
    return L


def _pit(w):
    """A ring floor at 0 round a pit at -128 (two ramps down, a bridge over),
    balconies at 128 along the west and east walls (a ramp at each end)."""
    L = {"hall": (-768, -576, 768, 576), "zlo": -128, "zhi": 352, "exit_z": 0}
    _tag("block")
    for b in ((-768, -576, 768, -256), (-768, 256, 768, 576),
              (-768, -256, -384, 256), (384, -256, 768, 256)):
        w.append(box(b[0], b[1], -128, b[2], b[3], 0, TEX_WALL, top=TEX_FLOOR))
    for x0, x1 in ((-768, -528), (528, 768)):
        w.append(box(x0, -320, 0, x1, 320, 128, TEX_WALL, top=TEX_GRID))
    # Corner landings at 128, each the top of a ramp along the end wall.
    for x0, x1 in ((-768, -640), (640, 768)):
        for y0, y1 in ((-576, -320), (320, 576)):
            w.append(box(x0, y0, 0, x1, y1, 128, TEX_WALL, top=TEX_GRID))
    _tag("ramp")
    w.append(ramp(-384, -128, -256, 256, -128, -128, 0, "+y", TEX_TRIM, top=TEX_FLOOR))
    w.append(ramp(256, -256, 384, 128, -128, -128, 0, "-y", TEX_TRIM, top=TEX_FLOOR))
    for y0, y1 in ((-576, -448), (448, 576)):
        w.append(ramp(-640, y0, -384, y1, 0, 0, 128, "-x", TEX_TRIM, top=TEX_FLOOR))
        w.append(ramp(384, y0, 640, y1, 0, 0, 128, "+x", TEX_TRIM, top=TEX_FLOOR))
    _tag("bridge")
    w.append(box(-64, -256, -16, 64, 256, 0, TEX_TRIM, top=TEX_FLOOR))
    _tag("cover")
    for x0, x1 in ((-256, -64), (64, 384)):
        w.append(box(x0, 256, 0, x1, 272, 32, TEX_TRIM))
    for x0, x1 in ((-384, -64), (64, 256)):
        w.append(box(x0, -272, 0, x1, -256, 32, TEX_TRIM))
    w.append(box(-400, -256, 0, -384, 128, 32, TEX_TRIM))
    w.append(box(384, -128, 0, 400, 256, 32, TEX_TRIM))
    for y0, y1 in ((-320, -224), (-96, 96), (224, 320)):
        w.append(box(-544, y0, 128, -528, y1, 160, TEX_TRIM))
        w.append(box(528, y0, 128, 544, y1, 160, TEX_TRIM))
    w.append(box(-320, -448, 0, -160, -416, 48, TEX_TRIM))
    w.append(box(160, -448, 0, 320, -416, 48, TEX_TRIM))
    _tag("pillar")
    for x in (-448, 448):
        w.append(box(x - 32, -384, 0, x + 32, -320, 352, TEX_TRIM))
    L["regions"] = [region(0, 0, -128, 5, 5),
                    region(-240, 424, 0, 3, 3), region(240, 424, 0, 3, 3),
                    region(-456, 0, 0, 1, 5), region(456, 0, 0, 1, 5),
                    region(-656, 0, 128, 2, 4), region(656, 0, 128, 2, 4)]
    L["items"] = [("weapon_supershotgun", -352, -528, 16), ("weapon_nailgun", -352, -240, -112),
                  ("item_shells", 320, -528, 16), ("item_spikes", 288, 192, -112),
                  ("item_health", -736, -288, 144), ("item_armor1", 688, 240, 144)]
    L["lights"] = [(x, y, -40, 320) for x in (-192, 192) for y in (-128, 128)]
    L["cameras"] = [("start", 0, -528, 0, 14, 90), ("perch", -576, -160, 128, 26, 15),
                    ("rim", -200, -296, 0, 34, 70)]
    L["views"] = [("overview", 0, -560, 320, 38, 90)]
    L["pins"] = [(-144, -48, -128), (144, 144, -128), (-456, 96, 0), (240, 424, 0),
                 (656, 96, 128), (-240, 520, 0)]
    L["blurb"] = ("Pit: a ring round a sunken pit, crossed by a bridge, with balconies "
                  "on both side walls. Monsters climb out of the pit; you fight from above.")
    return L


def _towers(w):
    """An open court: towers at 192 in the north corners joined by a catwalk,
    a plinth at 96 in the middle with a sloped bridge up to the catwalk,
    long ramps up the side walls, low walls scattered on the ground."""
    L = {"hall": (-768, -576, 768, 832), "zlo": 0, "zhi": 448, "exit_z": 0}
    _tag("block")
    for x0, x1 in ((-768, -512), (512, 768)):
        w.append(box(x0, 384, 0, x1, 832, 192, TEX_WALL, top=TEX_GRID))
    w.append(box(-192, -64, 0, 192, 256, 96, TEX_WALL, top=TEX_GRID))
    _tag("bridge")
    w.append(box(-512, 704, 176, 512, 832, 192, TEX_TRIM, top=TEX_FLOOR))
    w.append(ramp(-64, 256, 64, 704, 0, 96, 192, "+y", TEX_TRIM, top=TEX_FLOOR, thick=16))
    _tag("ramp")
    w.append(ramp(-768, 0, -640, 384, 0, 0, 192, "+y", TEX_TRIM, top=TEX_FLOOR))
    w.append(ramp(640, 0, 768, 384, 0, 0, 192, "+y", TEX_TRIM, top=TEX_FLOOR))
    _tag("stairs")
    w.extend(stairs(-96, -224, 96, -64, 0, 96, "+y"))
    w.extend(stairs(-352, 32, -192, 160, 0, 96, "+x"))
    w.extend(stairs(192, 32, 352, 160, 0, 96, "-x"))
    _tag("cover")
    w.append(box(-400, -320, 0, -368, -160, 56, TEX_TRIM))
    w.append(box(368, -320, 0, 400, -160, 56, TEX_TRIM))
    w.append(box(-96, -416, 0, 96, -384, 48, TEX_TRIM))
    for x0, x1 in ((-512, -384), (384, 512)):
        w.append(box(x0, 704, 192, x1, 720, 224, TEX_TRIM))
    _tag("pillar")
    for x in (-288, 288):
        w.append(box(x - 16, 752, 0, x + 16, 784, 176, TEX_TRIM))
    L["regions"] = [region(0, 528, 0, 8, 3), region(-576, -336, 0, 3, 3),
                    region(576, -336, 0, 3, 3), region(-480, 144, 0, 2, 3),
                    region(480, 144, 0, 2, 3), region(0, 96, 96, 3, 2),
                    region(-640, 624, 192, 2, 3), region(640, 624, 192, 2, 3)]
    L["items"] = [("weapon_supershotgun", -720, -528, 16), ("weapon_nailgun", 200, 752, 208),
                  ("item_shells", 688, -528, 16), ("item_spikes", -232, 752, 208),
                  ("item_health", 144, -48, 112), ("item_armor1", -720, 400, 208)]
    L["lights"] = [(x, 768, 112, 250) for x in (-160, 160)]
    L["cameras"] = [("start", 0, -528, 0, -6, 90), ("perch", 640, 432, 192, 16, 228),
                    ("catwalk", 0, 768, 192, 22, 270)]
    L["views"] = [("overview", 0, -560, 420, 32, 90)]
    L["pins"] = [(-576, -336, 0), (0, 144, 96), (-240, 528, 0), (688, 624, 192),
                 (432, 144, 0), (-592, 528, 192)]
    L["blurb"] = ("Towers: an open court with two corner towers, a catwalk over the exit "
                  "and a plinth in the middle, joined by ramps and a sloped bridge.")
    return L


_LAYOUT_FNS = {"terraces": _terraces, "pit": _pit, "towers": _towers}


def hall_description():
    """The shipped hall in the same terms as a candidate (for --plan)."""
    gx, gy = GRID_HALF_X, GRID_HALF_Y
    return {"hall": (HALL[0], HALL[1], HALL[3], HALL[4]), "zlo": 0, "zhi": HALL[5],
            "exit_z": 0,
            "regions": [{"origin": (0, 0, 0), "cols": GRID_COLS, "rows": GRID_ROWS,
                         "rect": (-gx, -gy, gx, gy)}],
            "items": [("weapon_supershotgun", -656, 0, 16), ("weapon_nailgun", 656, 0, 16),
                      ("item_shells", -656, -192, 16), ("item_spikes", 656, -192, 16),
                      ("item_health", -656, 192, 16), ("item_armor1", 656, 192, 16)],
            "cameras": [("start", 0, -536, 0, 8, 90)], "pins": [],
            "blurb": "The shipped hall: one floor, one grid, nowhere to hide."}


def build_layout(name):
    """(world, ents, description) for a candidate layout."""
    if name not in _LAYOUT_FNS:
        raise ArenaError(f"unknown layout: {name}")
    world = []
    L = _LAYOUT_FNS[name](world)
    x0, y0, x1, y1 = L["hall"]
    zlo, zhi, ez = L["zlo"], L["zhi"], L["exit_z"]
    t = WALL
    if y0 != HALL[1]:
        raise ArenaError("a layout's hall must meet the antechamber at y = -576")
    # Shell: floor, ceiling, walls; the south door at 0, the north at the exit.
    _tag("floor")
    world.append(box(x0 - t, y0 - t, zlo - t, x1 + t, y1 + t, zlo,
                     TEX_FLOOR if zlo == 0 else TEX_GRID))
    _tag("ceiling")
    world.append(box(x0 - t, y0 - t, zhi, x1 + t, y1 + t, zhi + t, TEX_CEIL))
    _tag("wall")
    world.append(box(x0 - t, y0 - t, zlo, x0, y1 + t, zhi, TEX_WALL))
    world.append(box(x1, y0 - t, zlo, x1 + t, y1 + t, zhi, TEX_WALL))
    world.extend(wall_doors(x0, y0 - t, zlo, x1, y0, zhi, -DOOR_HALF, DOOR_HALF,
                            0, DOOR_TOP, TEX_WALL))
    world.extend(wall_doors(x0, y1, zlo, x1, y1 + t, zhi, -DOOR_HALF, DOOR_HALF,
                            ez, ez + DOOR_TOP, TEX_WALL))
    # Antechamber, pads, start: as in the shipped hall.
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
    ents.append(({"classname": "trigger_changelevel", "map": EXIT_MAP, "spawnflags": "1"},
                 [box(exit_room[0] + 16, ey1 - 64, ez, exit_room[3] - 16, ey1 - 16,
                      ez + 144, TEX_TRIGGER)]))
    for cls, x, y, z in L["items"]:
        ents.append(({"classname": cls, "origin": f"{x} {y} {z}"}, []))
    # A grid of lights 48 under the ceiling, about 384 apart, each bright
    # enough to give the floor at 0 under it what the shipped hall gets.
    nx, ny = -(-(x1 - x0) // 384), -(-(y1 - y0) // 384)
    zl = zhi - 48
    grid_lights = [(x0 + (x1 - x0) * (2 * i + 1) // (2 * nx),
                    y0 + (y1 - y0) * (2 * j + 1) // (2 * ny), zl, zl + 250)
                   for i in range(nx) for j in range(ny)]
    for x, y, z, lv in grid_lights + L["lights"]:
        ents.append(({"classname": "light", "origin": f"{x} {y} {z}", "light": str(lv)}, []))
    for x in (-160, 160):
        ents.append(({"classname": "light", "origin": f"{x} -800 152", "light": "350"}, []))
    ents.append(({"classname": "light", "origin": f"0 {exit_room[1] + 128} {ez + 120}",
                  "light": "300"}, []))
    L["exit_room"] = exit_room
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


def map_text(world, ents, wad_name):
    out = ["// psQuake arena, generated by tools/generate_arena.py. Do not edit.\n"]

    def brush_text(faces):
        lines = ["{\n"]
        for p0, p1, p2, tex in faces:
            lines.append(f"{_fmt_point(p0)} {_fmt_point(p1)} {_fmt_point(p2)} {tex} 0 0 0 1 1\n")
        lines.append("}\n")
        return "".join(lines)

    worldkeys = {"classname": "worldspawn", "message": "psQuake Arena",
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
    ap.add_argument("--layout", choices=LAYOUTS, default="hall",
                    help="hall (shipped) or a prototype: terraces, pit, towers")
    ap.add_argument("--plan", help="also write a top-down SVG plan of the layout")
    ap.add_argument("--shots", help="also write the render driver's --arena-shots script")
    ap.add_argument("--print-hashes", action="store_true",
                    help="print each texture's source map and SHA-256")
    args = ap.parse_args(argv)

    try:
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
        world, ents = build(args.break_kind, args.layout)
        plan, _PLAN = _PLAN, None
        L = hall_description() if args.layout == "hall" else build_layout(args.layout)[2]
        text = map_text(world, ents, os.path.basename(args.wad))
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
