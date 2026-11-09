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


def box(x0, y0, z0, x1, y1, z1, tex, top=None):
    """An axis-aligned box brush; top overrides the texture of its +z face."""
    if not (x0 < x1 and y0 < y1 and z0 < z1):
        raise ArenaError(f"empty box {x0, y0, z0, x1, y1, z1}")
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
    brushes = [
        box(x0 - t, y0 - t, z0 - t, x1 + t, y1 + t, z0, tex_floor),
        box(x0 - t, y0 - t, z1, x1 + t, y1 + t, z1 + t, tex_ceil),
    ]
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


def build(break_kind=None):
    """Return (worldspawn brushes, [(entity keys, brushes)]) for the arena."""
    if break_kind is not None and break_kind not in BREAK_KINDS:
        raise ArenaError(f"unknown --break kind: {break_kind}")
    world = []

    # Hall: floor inlay, ceiling, walls with a south and a north doorway.
    hall = room(HALL, doors_south=(-DOOR_HALF, DOOR_HALF, DOOR_TOP),
                doors_north=(-DOOR_HALF, DOOR_HALF, DOOR_TOP),
                omit=("east",) if break_kind == "leak" else ())
    world.extend(floor_with_grid(HALL))
    world.extend(hall[1:])  # drop room()'s plain floor; the inlay replaces it

    # Trim band along the hall walls at the top of the doorways.
    hx0, hy0, _, hx1, hy1, _ = HALL
    tz0, tz1 = DOOR_TOP, DOOR_TOP + 16
    world.append(box(hx0, hy0, tz0, hx0 + 8, hy1, tz1, TEX_TRIM))
    world.append(box(hx1 - 8, hy0, tz0, hx1, hy1, tz1, TEX_TRIM))

    # Corner pillars in the margins, outside the grid.
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
        world, ents = build(args.break_kind)
        text = map_text(world, ents, os.path.basename(args.wad))
        _write_atomic(args.wad, wad2({n: found[n][1] for n in names}))
        _write_atomic(args.map, text.encode("ascii"))
    except (ArenaError, OSError) as e:
        print(f"generate_arena: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
