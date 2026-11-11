"""The themed psQuake arenas as declarative layouts (arena_layout.py).

MAPS: map name -> layout. Temple (psqtemple) is New Game's; Ruins and Forge
come next, each its own entry. Coordinates are Quake units, z up; the main
floor is z 0. Every value is data: the pieces are arena_kit.py's.

Temple, blockout (placeholder textures from LibreQuake's dev map):

    the sunken courtyard  z -128, 1152 x 1152, sky above: the central
                          spawn area, four 5 x 5 grids round a low plinth,
                          wide stair runs down from all four sides
    the south terrace     z 0, the start room's doorway and the escape
                          balcony (160) open onto it
    the ring              z 0 strips round the courtyard behind
                          colonnades, a 128 drop into it between the stairs
    the side aisles       z 0, west and east, behind columns; galleries
                          over them at z 192 (stairs at both ends), the
                          high ground over the courtyard
    the chapels, crypt    two 256 rooms off each aisle joined by a 128 crypt
                          corridor behind them: close quarters, a loop
    the nave              z 0, north of the courtyard, under octagonal arch
                          frames, with side aisles; the far sightline runs
                          from the courtyard up it to the raised dais (z
                          64) and the exit door behind it
"""

from generate_arena import Tex  # noqa: E402  (a face's texture with offsets)
import arena_kit as K  # noqa: E402

STYLES = {
    # brick walls banded at the foot and under the ceiling, dark stone floors
    "stone": {"floor": "med_csl_flr1_1", "ceil": "med_csl_brk9_1", "wall": "med_csl_brk10",
              "base": ("may_bnd1_2", 32), "frieze": ("med_etrim1", 32)},
    # the courtyard: diamond paving, a skull band along its sunken lip
    "court": {"floor": "grk_flr4_2", "ceil": "med_csl_brk9_1", "wall": "med_csl_brk10",
              "base": ("may_bnd1_2", 32), "frieze": ("may_bnd_skull", 32),
              "sky": True, "sky_tex": "sky_orng"},
    "high": {"floor": "med_csl_flr5_1", "ceil": "med_csl_brk9_1", "wall": "med_csl_brk10",
             "frieze": ("med_etrim1", 32)},
    "close": {"floor": "med_csl_flr4_1", "ceil": "med_csl_brk9_1", "wall": "may_brk1_1",
              "base": ("may_bnd1_2", 24), "frieze": ("may_bnd_skull", 32)},
    "stair": {"floor": "med_csl_flr4_1", "ceil": "med_csl_brk9_1", "wall": "may_bnd1_2"},
    "nave": {"floor": "med_csl_flr5_1", "ceil": "med_brk9_ceil1a", "wall": "med_csl_brk17",
             "base": ("may_bnd1_2", 32), "frieze": ("med_etrim1", 32)},
}
DETAIL = "may_trm1_1"     # arches, alcoves, caps
COLUMN = "may_plr1_1"     # column shafts
TRIM = "med_etrim1"       # beams, parapets
COVER = "med_block_1a"
RUNE = "met_rune_trim32"
SKULL = "may_tskull"
GLASS = ("med_glass1", "med_glass2", "med_glass3")

# The start room's theme (generate_arena.start_room, escape).
START_THEME = {"wall": "med_csl_brk10", "floor": "med_csl_flr1_1", "ceil": "med_csl_brk9_1",
               "pillar": COLUMN, "cap": DETAIL, "base": "may_bnd1_2", "cover": COVER}


def _windows(pieces):
    """Stained-glass windows in the nave aisles' outer walls: a niche 16
    deep cut into the wall (the carves returned), its glass at the back, a
    frame round its mouth, and a coloured light in front, as if lit from
    behind."""
    carves = []
    colours = ("1 0.55 0.75", "1 0.45 0.2", "0.55 0.6 1")
    for k, (x, face) in enumerate(((-448, -1), (448, 1))):
        for n, y in enumerate((1632, 1856)):
            xa, xb = sorted((x, x + 16 * face))
            carves.append({"box": (xa, y - 32, 48, xb, y + 32, 240)})
            gx = (xb - 4, xb) if face > 0 else (xa, xa + 4)
            # the pane shows its whole 64 x 192 picture: on an x-facing face
            # Quake's texture runs s = y, t = -z, offset to start at the
            # pane's corner (y - 32, z 240)
            glass = Tex(GLASS[(k + n) % 3], ((32 - y) % 64, 240 % 192))
            pieces.append(("box", dict(lo=(gx[0], y - 32, 48), hi=(gx[1], y + 32, 240), tex=glass)))
            pieces.append(("frame", dict(axis="y", lo=y - 32, hi=y + 32, at=x, z0=48, z1=240,
                                         depth=16, border=8, tex=DETAIL)))
            pieces.append(("torch", dict(x=x - 48 * face, y=y, z=144, light=110,
                                         colour=colours[(k + n) % 3])))
    return carves


def _torches_court():
    out = []
    # cups on the courtyard's sunken walls, between the stair runs: the
    # walls are 128 high, too low for an alcove's frame over the player
    for y in (96, 928):
        out.append(("sconce", dict(design="cup", axis="y", u=y, at=-576, face="+", z=-58, tex=DETAIL)))
        out.append(("sconce", dict(design="cup", axis="y", u=y, at=576, face="-", z=-58, tex=DETAIL)))
    for x in (-448, 448):
        out.append(("sconce", dict(design="cup", axis="x", u=x, at=-64, face="+", z=-58, tex=DETAIL)))
        out.append(("sconce", dict(design="cup", axis="x", u=x, at=1088, face="-", z=-58, tex=DETAIL)))
    return out


def _temple():
    areas = [
        {"id": "terrace", "box": (-768, -576, 0, 768, -320, 320), "style": "stone", "role": "link"},
        {"id": "court", "box": (-576, -64, -128, 576, 1088, 512), "style": "court",
         "role": "central"},
        {"id": "ledge_s", "box": (-576, -320, 0, 576, -64, 400), "style": "stone", "role": "link"},
        {"id": "strip_w", "box": (-800, -320, 0, -576, 1344, 400), "style": "stone", "role": "mid"},
        {"id": "strip_e", "box": (576, -320, 0, 800, 1344, 400), "style": "stone", "role": "mid"},
        {"id": "strip_n", "box": (-800, 1088, 0, 800, 1344, 400), "style": "stone", "role": "link"},
        {"id": "aisle_w", "box": (-1088, -320, 0, -800, 1344, 176), "style": "stone", "role": "mid"},
        {"id": "aisle_e", "box": (800, -320, 0, 1088, 1344, 176), "style": "stone", "role": "mid"},
        {"id": "gallery_w", "box": (-1088, -320, 192, -800, 1344, 400), "style": "high",
         "role": "high"},
        {"id": "gallery_e", "box": (800, -320, 192, 1088, 1344, 400), "style": "high",
         "role": "high"},
        {"id": "chapel_w1", "box": (-1376, 0, 0, -1120, 256, 192), "style": "close", "role": "close"},
        {"id": "chapel_w2", "box": (-1376, 768, 0, -1120, 1024, 192), "style": "close",
         "role": "close"},
        {"id": "crypt_w", "box": (-1376, 256, 0, -1248, 768, 176), "style": "close", "role": "close"},
        {"id": "chapel_e1", "box": (1120, 0, 0, 1376, 256, 192), "style": "close", "role": "close"},
        {"id": "chapel_e2", "box": (1120, 768, 0, 1376, 1024, 192), "style": "close", "role": "close"},
        {"id": "crypt_e", "box": (1248, 256, 0, 1376, 768, 176), "style": "close", "role": "close"},
        {"id": "nave", "box": (-256, 1344, 0, 256, 1920, 448), "style": "nave", "role": "far"},
        {"id": "nave_aisle_w", "box": (-448, 1408, 0, -256, 2240, 224), "style": "nave",
         "role": "mid"},
        {"id": "nave_aisle_e", "box": (256, 1408, 0, 448, 2240, 224), "style": "nave", "role": "mid"},
        {"id": "dais", "box": (-256, 2048, 64, 256, 2304, 448), "style": "high", "role": "high"},
    ]
    carves = [  # doorways, 128 wide and 176 high
        {"box": (-1120, 64, 0, -1088, 192, 176)}, {"box": (-1120, 832, 0, -1088, 960, 176)},
        {"box": (1088, 64, 0, 1120, 192, 176)}, {"box": (1088, 832, 0, 1120, 960, 176)},
    ]
    stairs = [
        {"rect": (-320, -320, 320, -64), "low": -128, "high": 0, "rise": "-y", "top": 400,
         "style": "stair"},
        {"rect": (-320, 1088, 320, 1344), "low": -128, "high": 0, "rise": "+y", "top": 400,
         "style": "stair"},
        {"rect": (-800, 256, -544, 768), "low": -128, "high": 0, "rise": "-x", "top": 400,
         "style": "stair"},
        {"rect": (544, 256, 800, 768), "low": -128, "high": 0, "rise": "+x", "top": 400,
         "style": "stair"},
        # aisle to gallery, both ends of each aisle (12 steps of 16 x 32)
        {"rect": (-1088, 960, -960, 1344), "solid": True, "low": 0, "high": 192, "rise": "+y", "top": 400,
         "style": "stair"},
        {"rect": (-1088, -320, -960, 64), "solid": True, "low": 0, "high": 192, "rise": "-y", "top": 400,
         "style": "stair"},
        {"rect": (960, 960, 1088, 1344), "solid": True, "low": 0, "high": 192, "rise": "+y", "top": 400,
         "style": "stair"},
        {"rect": (960, -320, 1088, 64), "solid": True, "low": 0, "high": 192, "rise": "-y", "top": 400,
         "style": "stair"},
        # up to the dais
        {"rect": (-256, 1920, 256, 2048), "low": 0, "high": 64, "rise": "+y", "top": 448,
         "style": "stair"},
    ]
    grids = [
        {"area": "court", "centre": (-288, 224), "cols": 5, "rows": 5},
        {"area": "court", "centre": (288, 224), "cols": 5, "rows": 5},
        {"area": "court", "centre": (-288, 800), "cols": 5, "rows": 5},
        {"area": "court", "centre": (288, 800), "cols": 5, "rows": 5},
        {"area": "aisle_w", "centre": (-960, 368), "cols": 2, "rows": 3},
        {"area": "aisle_e", "centre": (960, 368), "cols": 2, "rows": 3},
        {"area": "gallery_w", "centre": (-960, 704), "cols": 2, "rows": 2},
        {"area": "gallery_e", "centre": (960, 704), "cols": 2, "rows": 2},
        {"area": "nave", "centre": (0, 1632), "cols": 3, "rows": 4},
        {"area": "chapel_w1", "centre": (-1248, 128), "cols": 1, "rows": 1},
        {"area": "chapel_w2", "centre": (-1248, 896), "cols": 1, "rows": 1},
        {"area": "chapel_e1", "centre": (1248, 128), "cols": 1, "rows": 1},
        {"area": "chapel_e2", "centre": (1248, 896), "cols": 1, "rows": 1},
    ]
    pieces = []
    # colonnades: the courtyard's edges, the aisles' (under the galleries'
    # edges) and the nave's
    for at, a, b in ((-608, -256, 192), (-608, 832, 1280), (608, -256, 192), (608, 832, 1280)):
        pieces.append(("colonnade", dict(axis="y", a=a, b=b, at=at, r=24, z0=0, z1=400,
                                         spacing=224, tex=COLUMN, cap_tex=DETAIL)))
    for a, b in ((-768, -352), (352, 768)):
        pieces.append(("colonnade", dict(axis="x", a=a, b=b, at=1120, r=24, z0=0, z1=400,
                                         spacing=208, tex=COLUMN, cap_tex=DETAIL)))
    for a, b in ((-544, -352), (352, 544)):
        pieces.append(("colonnade", dict(axis="x", a=a, b=b, at=-96, r=24, z0=0, z1=400,
                                         spacing=192, tex=COLUMN, cap_tex=DETAIL)))
    for at in (-824, 824):   # under the galleries' edges, not out past them
        for a, b in ((-128, 128), (896, 1280)):
            pieces.append(("colonnade", dict(axis="y", a=a, b=b, at=at, r=16, z0=0, z1=176,
                                             spacing=128, tex=COLUMN, cap_tex=DETAIL)))
    for at in (-256, 256):
        pieces.append(("colonnade", dict(axis="y", a=1408, b=1856, at=at, r=24, z0=0, z1=224,
                                         spacing=224, tex=COLUMN, cap_tex=DETAIL)))
    # octagonal arch frames down the nave, a pointed one over the south stairs
    for y in (1376, 1520, 1744):
        pieces.append(("arch", dict(axis="x", at=y, lo=-256, hi=256, z0=0, spring=288, top=352,
                                    depth=32, tex=DETAIL, kind="octagonal", z1=448)))
    pieces.append(("arch", dict(axis="x", at=-336, lo=-320, hi=320, z0=0, spring=224, top=288,
                                depth=32, tex=DETAIL, kind="pointed", z1=320, round_jambs=True)))
    # a coffered nave ceiling: beams across and along it
    for y in range(1440, 1921, 160):
        pieces.append(("box", dict(lo=(-256, y - 8, 416), hi=(256, y + 8, 448), tex=TRIM)))
    for x in (-128, 128):
        pieces.append(("box", dict(lo=(x - 8, 1344, 416), hi=(x + 8, 1920, 448), tex=TRIM)))
    # torch alcoves
    for y in (384, 640):
        pieces.append(("alcove_torch", dict(axis="y", u=y, at=-1088, face="+", z=72, tex=DETAIL)))
        pieces.append(("alcove_torch", dict(axis="y", u=y, at=1088, face="-", z=72, tex=DETAIL)))
    for y in (1520, 1744, 2016):
        pieces.append(("alcove_torch", dict(axis="y", u=y, at=-448, face="+", z=72, tex=DETAIL)))
        pieces.append(("alcove_torch", dict(axis="y", u=y, at=448, face="-", z=72, tex=DETAIL)))
    for x, f in ((-1376, "+"), (1376, "-")):
        for y in (128, 896):
            pieces.append(("alcove_torch", dict(axis="y", u=y, at=x, face=f, z=72, tex=DETAIL)))
        pieces.append(("alcove_torch", dict(axis="y", u=512, at=x, face=f, z=72, tex=DETAIL)))
    pieces += _torches_court()
    # a chamfered coping along the courtyard's sunken lip, between the stairs
    for y0, y1 in ((-64, 256), (768, 1088)):
        for x, sgn in ((-576, 1), (576, -1)):
            pieces.append(("hull", dict(points=[(x, y, z) for y in (y0, y1) for z in (0, -24)]
                                        + [(x + sgn * 16, y, 0) for y in (y0, y1)], tex=DETAIL)))
    for x0, x1 in ((-576, -320), (320, 576)):
        for y, sgn in ((-64, 1), (1088, -1)):
            pieces.append(("hull", dict(points=[(x, y, z) for x in (x0, x1) for z in (0, -24)]
                                        + [(x, y + sgn * 16, 0) for x in (x0, x1)], tex=DETAIL)))
    # torches on the galleries' outer walls
    for x in (-1080, 1080):
        for y in (400, 1040):
            pieces.append(("torch", dict(x=x, y=y, z=288)))
    # braziers: the courtyard's corners, the nave aisles and the dais
    for x, y in ((-552, -40), (552, -40), (-552, 1064), (552, 1064)):
        pieces.append(("brazier", dict(x=x, y=y, z=-128, tex=DETAIL)))
    for x in (-352, 352):
        for y in (1520, 1744, 1968):
            pieces.append(("brazier", dict(x=x, y=y, z=0, tex=DETAIL)))
    for x in (-224, 224):
        pieces.append(("brazier", dict(x=x, y=2272, z=64, tex=DETAIL)))
    # sconces: brackets on the aisles' outer walls, cups by the gallery
    # stairs' feet, trapezoid braziers on the terrace's end walls
    for design, axis, u, at, face, z in (
            ("bracket", "y", 288, -1088, "+", 72), ("bracket", "y", 768, -1088, "+", 72),
            ("bracket", "y", 288, 1088, "-", 72), ("bracket", "y", 768, 1088, "-", 72),
            ("cup", "x", -1024, 1344, "-", 264), ("cup", "x", 1024, 1344, "-", 264),
            ("trapezoid", "y", -448, -768, "+", 96), ("trapezoid", "y", -448, 768, "-", 96)):
        pieces.append(("sconce", dict(design=design, axis=axis, u=u, at=at, face=face, z=z,
                                      tex=DETAIL)))
    # torches either side of the start doorway, on the terrace's south wall
    for x in (-192, 192):
        pieces.append(("alcove_torch", dict(axis="x", u=x, at=-576, face="+", z=72, tex=DETAIL)))
    carves += _windows(pieces)
    # the skylight's frame: a chamfered lintel round the courtyard's opening
    for lo, hi in (((-576, -64, 472), (576, -40, 512)), ((-576, 1064, 472), (576, 1088, 512)),
                   ((-576, -40, 472), (-552, 1064, 512)), ((552, -40, 472), (576, 1064, 512))):
        pieces.append(("box", dict(lo=lo, hi=hi, tex=RUNE)))
    # coffered ceilings: beams across the aisles, the ring and the galleries
    for x0, x1 in ((-1088, -800), (800, 1088)):
        for y in range(-224, 1281, 192):
            if 64 < y < 960:   # not over the stairwells up to the galleries
                pieces.append(("box", dict(lo=(x0, y - 8, 160), hi=(x1, y + 8, 176), tex=TRIM)))
            pieces.append(("box", dict(lo=(x0, y - 8, 384), hi=(x1, y + 8, 400), tex=TRIM)))
    for x0, x1 in ((-800, -576), (576, 800)):
        for y in range(-224, 1281, 224):
            pieces.append(("box", dict(lo=(x0, y - 8, 376), hi=(x1, y + 8, 400), tex=TRIM)))
    # framed doorways into the chapels
    for x in (-1104, 1104):
        for lo in (64, 832):
            pieces.append(("arch", dict(axis="y", at=x, lo=lo, hi=lo + 128, z0=0, spring=128,
                                        top=176, depth=48, tex=DETAIL, jamb=16, z1=192)))
    # skull panels: the dais's back wall, either side of the exit, and
    # the chapels' end walls
    for x0 in (-256, 128):
        pieces.append(("box", dict(lo=(x0, 2300, 128), hi=(x0 + 128, 2304, 256), tex=SKULL)))
    for x0 in (-1280, 1152):   # on the 128 grid, the picture's own
        for y in (0, 1024):
            yy = (y + 1, y + 5) if y == 0 else (y - 5, y - 1)
            pieces.append(("box", dict(lo=(x0, yy[0], 32), hi=(x0 + 128, yy[1], 160),
                                        tex=Tex(SKULL, (0, 160 % 128)))))
    world_pieces = [
        # the courtyard's low cover: a plinth in the middle and four blocks
        ("box", dict(lo=(-40, 472, -128), hi=(40, 552, -88), tex=COVER)),
        ("box", dict(lo=(-32, 96, -128), hi=(32, 160, -88), tex=COVER)),
        ("box", dict(lo=(-32, 864, -128), hi=(32, 928, -88), tex=COVER)),
        ("box", dict(lo=(-320, 480, -128), hi=(-256, 544, -88), tex=COVER)),
        ("box", dict(lo=(256, 480, -128), hi=(320, 544, -88), tex=COVER)),
        # gallery parapets, thin (8) and low (32: the player looks over and
        # jumps them), gaps to drop from
        ("box", dict(lo=(-808, -256, 192), hi=(-800, 256, 224), tex=TRIM)),
        ("box", dict(lo=(-808, 768, 192), hi=(-800, 1280, 224), tex=TRIM)),
        ("box", dict(lo=(800, -256, 192), hi=(808, 256, 224), tex=TRIM)),
        ("box", dict(lo=(800, 768, 192), hi=(808, 1280, 224), tex=TRIM)),
    ]
    items = [
        ("weapon_supershotgun", -544, -448, 16), ("item_shells", 544, -480, 16),
        ("weapon_nailgun", -944, 112, 16), ("item_spikes", 944, 112, 16),
        ("item_health", 0, 240, -112), ("item_health", 0, 784, -112),
        ("item_shells", -160, 512, -112, 1), ("item_spikes", 160, 512, -112, 1),
        ("item_armor1", 944, 880, 16), ("item_health", -880, 896, 208),
        ("item_health", 880, 896, 208), ("item_health", -1312, 224, 16),
        ("item_health", 1312, 224, 16), ("item_health", 0, 2240, 80, 2),
    ]
    supply = [(-688, -160, 16), (688, -160, 16), (-688, 1216, 16), (688, 1216, 16),
              (-448, -192, 16), (448, -192, 16), (-320, -448, 16), (320, -448, 16),
              (-1312, 48, 16), (1312, 976, 16), (-384, 1632, 16), (384, 1632, 16),
              (-880, 1216, 208), (880, -224, 208)]
    lights = []
    for x in (-384, 0, 384):
        for y in (128, 512, 896):
            lights.append((x, y, 220, 380))
    for x in (-944, 944):
        for y in (-192, 192, 576, 960, 1280):
            lights.append((x, y, 140, 220))
            lights.append((x, y, 330, 300))
    for x in (-688, 688):
        for y in (-160, 576, 1216):
            lights.append((x, y, 300, 320))
    for y in (1440, 1680, 1880, 2176):
        lights.append((0, y, 300, 380))
        for x in (-352, 352):
            lights.append((x, y, 180, 200))
    for x in (-512, 0, 512):
        lights.append((x, -448, 220, 360))
    # the galleries' stairs, the aisles' ends and the ring's corners
    for x in (-1024, 1024):
        for y in (-160, 1150):
            lights.append((x, y, 150, 260, "1 0.85 0.65"))
    for x in (-688, 688):
        lights.append((x, -280, 140, 260, "1 0.85 0.65"))
    for x in (-704, 704):
        lights.append((x, -520, 120, 240, "1 0.85 0.65"))
    for x, y in ((-800, -80), (800, -80), (-576, 1216), (576, 1216), (-800, 1216), (800, 1216),
                 (-672, 400), (672, 400), (-672, 624), (672, 624), (0, -192), (0, 1216),
                 (-448, -192), (448, -192), (-448, 1216), (448, 1216), (-160, 1200), (160, 1200),
                 (-960, 1072), (960, 1072), (-672, 272), (672, 272), (-672, 752), (672, 752)):
        lights.append((x, y, 140, 260, "1 0.85 0.65"))
    # down the south stair run into the courtyard, and the side runs' feet
    for x, y, z in ((-200, -200, 40), (200, -200, 40), (0, -140, 0), (-624, 320, 40),
                    (624, 320, 40), (-624, 704, 40), (624, 704, 40)):
        lights.append((x, y, z, 280, "1 0.85 0.65"))
    for x, y in ((-1248, 128), (-1248, 896), (1248, 128), (1248, 896), (-1312, 512),
                 (1312, 512)):
        lights.append((x, y, 140, 180))
    return {
        "name": "psqtemple", "title": "psQuake Arena: Temple",
        "bounds": (-1408, -1408, -256, 1408, 2720, 720),
        "styles": STYLES, "default": "stone", "start_theme": START_THEME,
        "areas": areas, "carves": carves, "stairs": stairs, "grids": grids,
        "central": "court", "pieces": pieces, "world_pieces": world_pieces,
        "items": items, "supply": supply, "lights": lights,
        "sun": {"_sunlight": "280", "_sunlight_color": "1 0.6 0.55", "_sun_mangle": "20 -65 0",
                "_sunlight2": "110", "_sunlight2_color": "1 0.55 0.6"},
        "balcony": (-464, 176), "hall_top": 320, "exit": (2304, 64),
        "exit_map": "psqruins",
        "sightline": ((0, -48, -82), (0, 1952, 62)),
        "cameras": [("start", 0, -528, 0, 10, 90), ("central", 0, 32, -128, 2, 90),
                    ("close", -1312, 300, 0, 0, 90), ("far", 0, 1380, 0, 2, 90),
                    ("high", -944, 640, 192, 18, 0), ("room", 0, -1240, 0, -6, 90),
                    ("balcony", 816, -464, 176, 20, 180), ("perch", 0, 2208, 64, 12, 270),
                    ("cover", -288, 400, -128, 2, 75)],
        "views": [("overview", 0, -40, 400, 38, 90)],
        "pins": [(-288, 224, -128), (288, 800, -128), (-944, 368, 0), (0, 1632, 0),
                 (944, 704, 192), (-1248, 896, 0)],
        "blurb": ("Temple: a sunken, sky-lit courtyard ringed by colonnades, with side "
                  "aisles and galleries, chapels and a crypt behind them, and a long nave "
                  "to the dais and the exit."),
    }


# --- Ruins ------------------------------------------------------------------
#
# Outdoor mossy rock under a blue sky:
#
#   the field        z 0, 1280 square of grass, four 5 x 5 grids round a
#                    cross of broken walls and fallen blocks: the central
#                    spawn area; the approach south of it takes the start
#                    room's doorway and the escape balcony
#   west terraces    z 64 then 128, ruined retaining walls and trees, a
#                    stair run up to the ruin tower's top (z 256, a thin rail)
#   the chasm        across the north, its floor z -192 under shallow water
#                    with a stair run out at each end; a stone bridge over
#                    it to the north plateau and the exit: the far sightline
#   the keep         east: a roofed hall and a roofless room, close quarters,
#                    a stair up to the cliff ledge path (z 128) along the
#                    chasm's east side, down to the plateau

RUINS_STYLES = {
    "field": {"floor": "gardgrass_1", "ceil": "med_rock5", "wall": "rock1_2",
              "base": ("med_rock5", 48), "frieze": ("med_rock5_g2", 64),
              "sky": True, "sky_tex": "sky5_blu"},
    "path": {"floor": "med_cobstn2_1a", "ceil": "med_rock5", "wall": "rock1_2",
             "base": ("med_rock5", 48), "frieze": ("med_rock5_g2", 64),
             "sky": True, "sky_tex": "sky5_blu"},
    "terrace": {"floor": "grass1", "ceil": "med_rock5", "wall": "rock1_2",
                "base": ("grk_bl_trim1", 16), "frieze": ("med_rock5_g2", 64),
                "sky": True, "sky_tex": "sky5_blu"},
    "chasm": {"floor": "med_rock4", "ceil": "med_rock5", "wall": "darkrock",
              "base": ("med_rock4", 48), "sky": True, "sky_tex": "sky5_blu"},
    "keep": {"floor": "med_cobstn1_1", "ceil": "grk_bl_flt1", "wall": "grk_bl_brk1a",
             "base": ("grk_bl_trim1", 16), "frieze": ("grk_bl_wll2", 32)},
    "keep_open": {"floor": "med_cobstn1_1", "ceil": "grk_bl_flt1", "wall": "grk_bl_brk1a",
                  "base": ("grk_bl_trim1", 16), "sky": True, "sky_tex": "sky5_blu"},
    "stair": {"floor": "med_cobstn2_1a", "ceil": "med_rock5", "wall": "grk_bl_brk2a",
              "sky": True, "sky_tex": "sky5_blu"},
    "cliff": {"floor": "grass1", "ceil": "med_rock5", "wall": "rock1_2"},
}
RUIN = "grk_bl_brk2a"
RUIN_TRIM = "grk_bl_trim1"
RUIN_CAP = "grk_arch_trim"     # column bases and caps: no wall's texture
PILLAR = "grk_bl_pil1"
BARK = "woodbark64"
LEAVES = "grk_leaf1_1"
ROCK = "med_rock5_g2"
DARK = "darkrock"
RUINS_START = {"wall": "grk_bl_brk1a", "floor": "med_cobstn1_1", "ceil": "grk_bl_flt1",
               "pillar": PILLAR, "cap": RUIN_TRIM, "base": RUIN_TRIM, "cover": RUIN}


def _broken_wall(x0, y0, x1, y1, z, h, seed, tex=RUIN):
    """A low ruined wall over the footprint, in pieces with ragged tops
    between h/2 and h (seeded)."""
    import random
    rng = random.Random(seed)
    along_x = x1 - x0 >= y1 - y0
    a0, a1 = (x0, x1) if along_x else (y0, y1)
    out, a = [], a0
    while a < a1:
        b = min(a1, a + rng.choice((32, 48, 64)))
        t0, t1 = z + rng.randint(h // 2, h), z + rng.randint(h // 2, h)
        if along_x:
            pts = [(a, y, z) for y in (y0, y1)] + [(b, y, z) for y in (y0, y1)] + \
                  [(a, y, t0) for y in (y0, y1)] + [(b, y, t1) for y in (y0, y1)]
        else:
            pts = [(x, a, z) for x in (x0, x1)] + [(x, b, z) for x in (x0, x1)] + \
                  [(x, a, t0) for x in (x0, x1)] + [(x, b, t1) for x in (x0, x1)]
        out.append(("hull", dict(points=pts, tex=tex)))
        a = b + rng.choice((0, 0, 16))
    return out


def _tree(x, y, z, seed):
    """A leafy brush tree: a bark trunk and a seeded canopy of leaf rocks."""
    return [("shaft", dict(cx=x, cy=y, r=12, z0=z, z1=z + 152, tex=BARK)),
            ("rocks", dict(seed=seed, cx=x, cy=y, z0=z + 144, radius=40, count=4, size=96,
                           tex=LEAVES))]


def _ruins_cliffs(carves, pieces):
    """The outer cliffs lean back 10 degrees off vertical as they rise (the
    arena widens toward the sky, a funnel): each wall plane is carved back
    by the lean at its top and a rock wedge fills its foot. Seeded outcrops
    and strata stand out of the slope above head height, boulders at its
    foot off the paths. Returns the wedges (world brushes)."""
    world = []
    # (axis, a0, a1, wall plane, room side, floor, top, style of the carve)
    walls = [("y", -320, 448, -1152, "+", 64, 448, "terrace"),
             ("y", 448, 960, -1152, "+", 128, 448, "terrace"),
             ("y", 1504, 2048, -768, "+", 0, 448, "field"),
             ("y", 1728, 2048, 1024, "-", 0, 448, "field"),
             ("x", -768, -160, 2048, "-", 0, 448, "field"),
             ("x", 160, 1024, 2048, "-", 0, 448, "field"),
             # the field, 15 degrees (back to the terraces' plane): west round
             # the terrace stair, east round the keep's doors
             ("y", -320, -64, -640, "+", 0, 448, "field", 15),
             ("y", 192, 960, -640, "+", 0, 448, "field", 15),
             ("y", -320, 96, 640, "-", 0, 448, "field", 15),
             ("y", 224, 576, 640, "-", 0, 448, "field", 15),
             ("y", 704, 960, 640, "-", 0, 448, "field", 15),
             # the approach's west end (the escape balcony is in its east one)
             ("y", -576, -320, -768, "+", 0, 448, "path")]
    for k, (axis, a0, a1, at, face, z0, z1, style, *lean) in enumerate(walls):
        lean = lean[0] if lean else 10
        d = K.lean_depth(z0, z1, lean)
        sgn = 1 if face == "+" else -1
        back = at - sgn * d
        lo, hi = sorted((back, at))
        if axis == "y":
            carves.append({"box": (lo, a0, z0, hi, a1, z1), "style": style})
        else:
            carves.append({"box": (a0, lo, z0, a1, hi, z1), "style": style})
        world.append(("lean_wall", dict(axis=axis, a0=a0, a1=a1, at=at, face=face, z0=z0,
                                        z1=z1, tex=ROCK, lean=lean)))

        def plane(z):   # the slope's distance out from the carved-back plane
            return back + sgn * d * (z1 - z) / (z1 - z0)
        for n, z in enumerate((z0 + 136, z0 + 248, z0 + 344)):
            pieces.append(("strata", dict(axis=axis, a0=a0 + 32, a1=a1 - 32, at=round(plane(z)),
                                          face=face, z=z, seed=100 * k + n, tex=DARK)))
        for n, a in enumerate(range(a0 + 96, a1 - 63, 160)):
            z = z0 + 128 + 56 * (n % 4)
            o = plane(z) + sgn * 8
            cx, cy = (o, a) if axis == "y" else (a, o)
            pieces.append(("rocks", dict(seed=300 + 10 * k + n, cx=round(cx), cy=round(cy),
                                         z0=z - 40, radius=24, count=3, size=72, tex=ROCK)))
    return world


def _ruins_sconces():
    """Sconces with their flames: the chasm's walls just over the water and
    under the bridge, by the spiral stairs, the keep's corners, the
    terraces' dim corners and the approach."""
    out = []
    cup, trap, br = "cup", "trapezoid", "bracket"
    for design, axis, u, at, face, z in (
            (trap, "x", -352, 1472, "-", -72), (trap, "x", 352, 1472, "-", -72),
            (cup, "x", -160, 1472, "-", -68), (cup, "x", 160, 1472, "-", -68),
            (trap, "x", -320, 1088, "+", -128), (trap, "x", 320, 1088, "+", -128),
            (cup, "y", 1280, -768, "+", -120), (cup, "y", 1280, 768, "-", -120),
            (br, "x", -400, 1088, "+", -40), (br, "x", 400, 1088, "+", -40),
            (cup, "x", 900, 864, "-", 72), (cup, "y", 300, 800, "+", 72),
            (cup, "x", -960, -320, "+", 136), (cup, "x", -480, -576, "+", 72),
            (cup, "x", 480, -576, "+", 72), (br, "y", 1100, 1024, "-", 192),
            (trap, "x", -816, 1216, "+", 360)):
        out.append(("sconce", dict(design=design, axis=axis, u=u, at=at, face=face, z=z,
                                   tex=RUIN_TRIM)))
    return out


def _ruins():
    areas = [
        {"id": "approach", "box": (-768, -576, 0, 768, -320, 448), "style": "path", "role": "link"},
        {"id": "field", "box": (-640, -320, 0, 640, 960, 448), "style": "field", "role": "central"},
        {"id": "field_n", "box": (-768, 960, 0, 768, 1088, 448), "style": "field", "role": "link"},
        {"id": "terrace1", "box": (-1152, -320, 64, -768, 448, 448), "style": "terrace",
         "role": "mid"},
        {"id": "terrace2", "box": (-1152, 448, 128, -768, 960, 448), "style": "terrace",
         "role": "mid"},
        {"id": "tower", "box": (-1024, 1216, 256, -768, 1472, 448), "style": "path", "role": "high"},
        {"id": "chasm", "box": (-768, 1088, -192, 768, 1472, 448), "style": "chasm", "role": "far"},
        {"id": "plateau", "box": (-768, 1472, 0, 1024, 2048, 448), "style": "field", "role": "far"},
        {"id": "keep_hall", "box": (800, 32, 0, 1184, 384, 192), "style": "keep", "role": "close"},
        {"id": "keep_room", "box": (800, 448, 0, 1184, 864, 448), "style": "keep_open",
         "role": "close"},
        {"id": "ledge", "box": (768, 864, 128, 1024, 1472, 448), "style": "path", "role": "high"},
    ]
    carves = [
        {"box": (640, 96, 0, 800, 224, 160)}, {"box": (640, 576, 0, 800, 704, 160)},   # keep doors
        {"box": (928, 384, 0, 1056, 448, 160)},                                       # inner door
        {"box": (1024, 864, 128, 1184, 992, 448), "style": "path"},   # the keep stair's landing
    ]
    stairs = [
        {"rect": (-768, -64, -640, 192), "low": 0, "high": 64, "rise": "-x", "top": 448,
         "style": "stair"},
        {"rect": (-1024, 448, -896, 576), "low": 64, "high": 128, "rise": "+y", "top": 448,
         "style": "stair"},
        {"rect": (-1024, 960, -896, 1216), "low": 128, "high": 256, "rise": "+y", "top": 448,
         "style": "stair"},
        # spiral stairs out of the chasm's water at both ends, a quarter turn
        # each in its corner: the foot opens toward the middle of the water,
        # the steps run out to the corner's walls (nothing to stand behind),
        # and the top steps off south onto field_n's end
        {"kind": "spiral", "centre": (-608, 1088), "r0": 48, "r1": 160, "a0": 90, "a1": 180,
         "low": -192, "high": 0, "steps": 12, "style": "stair", "from_water": -168,
         "fill": (-768, 1088, -608, 1248)},
        {"kind": "spiral", "centre": (608, 1088), "r0": 48, "r1": 160, "a0": 90, "a1": 0,
         "low": -192, "high": 0, "steps": 12, "style": "stair", "from_water": -168,
         "fill": (608, 1088, 768, 1248)},
        # the keep up to the ledge, the ledge down to the plateau
        {"rect": (1056, 608, 1184, 864), "solid": True, "low": 0, "high": 128, "rise": "+y",
         "top": 448, "style": "stair"},
        {"rect": (768, 1472, 1024, 1728), "solid": True, "low": 0, "high": 128, "rise": "-y",
         "top": 448, "style": "stair"},
    ]
    grids = [
        {"area": "field", "centre": (-320, 0), "cols": 5, "rows": 5},
        {"area": "field", "centre": (320, 0), "cols": 5, "rows": 5},
        {"area": "field", "centre": (-320, 640), "cols": 5, "rows": 5},
        {"area": "field", "centre": (320, 640), "cols": 5, "rows": 5},
        {"area": "terrace1", "centre": (-960, 64), "cols": 3, "rows": 4},
        {"area": "terrace2", "centre": (-960, 736), "cols": 3, "rows": 3},
        {"area": "tower", "centre": (-896, 1344), "cols": 2, "rows": 2},
        {"area": "keep_hall", "centre": (992, 208), "cols": 3, "rows": 2},
        {"area": "keep_room", "centre": (912, 656), "cols": 2, "rows": 2},
        {"area": "plateau", "centre": (0, 1800), "cols": 4, "rows": 2},
        {"area": "ledge", "centre": (896, 1184), "cols": 1, "rows": 3},
    ]
    world = [
        # the stone bridge over the chasm, its kerbs (32: the player jumps)
        ("box", dict(lo=(-64, 1088, -32), hi=(64, 1472, 0), tex={"side": RUIN, "top": "med_cobstn2_1a",
                                                                 "bottom": RUIN})),
        ("box", dict(lo=(-64, 1088, 0), hi=(-48, 1472, 32), tex=RUIN_TRIM)),
        ("box", dict(lo=(48, 1088, 0), hi=(64, 1472, 32), tex=RUIN_TRIM)),
        # the chasm's water, 24 deep: walked in, not swum
        ("box", dict(lo=(-768, 1088, -192), hi=(768, 1472, -168), tex="*water0")),
        # thin rails along the tower's open east edge and the ledge's edge
        # over the chasm (gaps to drop into the water): the player looks
        # over them at the arena (their tops under the eye) and cannot walk
        # off
        ("rail", dict(axis="y", a0=1216, a1=1472, at=-768, face="-", z=256, tex=RUIN_CAP)),
        ("rail", dict(axis="y", a0=864, a1=1088, at=768, face="+", z=128, tex=RUIN_CAP)),
        ("rail", dict(axis="y", a0=1216, a1=1472, at=768, face="+", z=128, tex=RUIN_CAP)),
    ]
    pieces = []
    world += _ruins_cliffs(carves, pieces)
    # broken walls and fallen blocks: the field's cross, its edges, the terraces
    pieces += _broken_wall(-48, -240, -24, 160, 0, 40, 1)
    pieces += _broken_wall(24, 480, 48, 880, 0, 40, 2)
    pieces += _broken_wall(-560, 304, -160, 328, 0, 40, 3)
    pieces += _broken_wall(160, 312, 560, 336, 0, 40, 4)
    pieces += _broken_wall(-1144, -312, -1120, 440, 64, 96, 5)
    pieces += _broken_wall(-1144, 456, -1120, 952, 128, 96, 6)
    pieces += _broken_wall(-1016, 1976, -320, 2000, 0, 112, 7)
    pieces += _broken_wall(320, 1976, 1016, 2000, 0, 112, 8)
    for seed, (x, y) in enumerate(((0, 320), (-400, 320), (400, 320), (0, -280), (0, 920))):
        pieces.append(("rocks", dict(seed=20 + seed, cx=x, cy=y, z0=0, radius=24, count=2,
                                     size=40, tex=RUIN)))
    # broken pillars of a ruined colonnade along the approach
    for x in (-640, -448, 448, 640):
        h = 288 if x in (-640, 448) else 160
        pieces.append(("shaft", dict(cx=x, cy=-336, r=24, z0=0, z1=h, tex=PILLAR)))
    # mossy rock clusters along the cliffs
    feet = [(-1120, -280, 64), (-1120, 900, 128), (-740, 2016, 0), (990, 2016, 0),
            (-736, 1440, -192), (736, 1440, -192), (0, 1456, -192), (-420, 1448, -192),
            (380, 1450, -192), (990, 1440, 128), (-740, 1500, 0), (-740, 1700, 0),
            (-740, 1900, 0), (-400, 2020, 0), (400, 2020, 0), (990, 1800, 0), (-740, -540, 0),
            (740, -300, 0), (-200, 1450, -192), (200, 1110, -192), (-1120, 400, 64)]
    for seed, (x, y, z) in enumerate(feet):
        pieces.append(("rocks", dict(seed=40 + seed, cx=x, cy=y, z0=z, radius=48, count=4,
                                     size=72, tex=ROCK)))
    # newel posts at the spirals' hubs, standing over field_n's ends; wide
    # enough (inradius 48) to meet the steps' inner edge, leaving no slot
    for x in (-608, 608):
        pieces.append(("column", dict(cx=x, cy=1088, r=52, z0=-192, z1=112, tex=PILLAR,
                                      cap_tex=RUIN_CAP)))
    # arches under the bridge
    for y in (1216, 1344):
        pieces.append(("arch", dict(axis="x", at=y, lo=-64, hi=64, z0=-192, spring=-96,
                                    top=-48, depth=32, tex=RUIN, kind="pointed", z1=-32,
                                    round_jambs=True)))
    # a ruined arcade along the field's east edge: columns broken off at
    # different heights, two still carrying a lintel
    for k, y in enumerate(range(-240, 881, 160)):
        h = (288, 96, 224, 64, 288, 160, 80, 256)[k % 8]
        pieces.append(("column", dict(cx=608, cy=y, r=16, z0=0, z1=h, tex=PILLAR, cap_tex=RUIN_CAP)))
    pieces.append(("box", dict(lo=(584, -240, 288), hi=(632, 400, 312), tex=RUIN_CAP)))
    # trees on the terraces and the plateau
    for seed, (x, y, z) in enumerate(((-1104, 320, 64), (-816, -272, 64), (-1104, 912, 128),
                                       (-816, 440 + 64, 128), (-688, 1600, 0), (688, 1600, 0),
                                       (-688, 1960, 0), (960, 1960, 0))):
        pieces += _tree(x, y, z, 60 + seed)
    # torches in the keep
    for x in (864, 1120):
        pieces.append(("alcove_torch", dict(axis="x", u=x, at=32, face="+", z=72, tex=RUIN_TRIM)))
    pieces.append(("alcove_torch", dict(axis="y", u=208, at=1184, face="-", z=72, tex=RUIN_TRIM)))
    pieces.append(("alcove_torch", dict(axis="y", u=520, at=1184, face="-", z=72, tex=RUIN_TRIM)))
    pieces.append(("alcove_torch", dict(axis="x", u=-320, at=-576, face="+", z=72, tex=RUIN_TRIM)))
    pieces.append(("alcove_torch", dict(axis="x", u=320, at=-576, face="+", z=72, tex=RUIN_TRIM)))
    pieces += _ruins_sconces()
    pieces.append(("brazier", dict(x=-160, y=2016, z=0)))
    pieces.append(("brazier", dict(x=160, y=2016, z=0)))
    items = [
        ("weapon_supershotgun", -544, -448, 16), ("item_shells", 544, -480, 16),
        ("weapon_nailgun", -1104, -200, 80), ("item_spikes", 1150, 60, 16),
        ("item_health", 0, 240, 16), ("item_health", 0, 400, 16),
        ("item_shells", -100, 280, 16, 1), ("item_spikes", 600, 320, 16, 1),
        ("item_armor1", 840, 830, 16), ("item_health", -860, 520, 144),
        ("item_health", 896, 960, 144),
        ("item_health", 0, 1960, 16, 2),
    ]
    supply = [(-600, -260, 16), (500, -290, 16), (-600, 900, 16), (500, 900, 16),
              (-200, 1040, 16), (200, 1040, 16), (-1060, -280, 80), (-860, 920, 144),
              (1120, 320, 16), (880, 800, 16), (-400, 1700, 16), (400, 1700, 16)]
    lights = [(x, y, 120, 220) for x, y in ((992, 120), (992, 300), (912, 520), (1100, 760))]
    # the terraces' cliff feet, the tower stair and its top
    lights += [(x, y, z, 240, "1 0.85 0.65") for x, y, z in ((-1100, -200, 200), (-1100, 700, 260),
                                                          (-960, 1088, 330), (-960, 1232, 360))]
    lights += [(x, -448, 160, 260) for x in (-480, 0, 480)]
    lights.append((880, 760, 120, 220))     # the keep room's west corner
    # the field's leaning walls, and the spirals' feet in the water
    lights += [(x, y, 200, 360, "1 0.9 0.75") for x in (-576, 576)
               for y in (-200, 100, 400, 700, 900)]
    lights += [(x, y, -120, 300, "1 0.85 0.65") for x in (-520, 520) for y in (1160, 1300)]
    # field_n's ends and the spirals' tops
    lights += [(x, y, z, 240, "1 0.85 0.65") for sx in (-1, 1)
               for x, y, z in ((sx * 704, 1024, 120), (sx * 680, 1168, 60))]
    return {
        "name": "psqruins", "title": "psQuake Arena: Ruins",
        "bounds": (-1408, -1408, -256, 1408, 2432, 768),
        "styles": RUINS_STYLES, "default": "cliff", "start_theme": RUINS_START,
        "areas": areas, "carves": carves, "stairs": stairs, "grids": grids,
        "central": "field", "pieces": pieces, "world_pieces": world,
        "items": items, "supply": supply, "lights": lights,
        "sun": {"_sunlight": "300", "_sunlight_color": "1 1 1", "_sun_mangle": "35 -55 0",
                "_sunlight2": "150", "_sunlight2_color": "0.75 0.85 1"},
        "balcony": (-464, 176), "hall_top": 448, "exit": (2048, 0),
        "exit_map": "psqforge",
        "sightline": ((0, 0, 46), (0, 1984, 46)),
        "cameras": [("start", 0, -528, 0, 8, 90), ("central", 0, 120, 0, 2, 90),
                    ("close", 992, 120, 0, 0, 90), ("far", 0, 1040, 0, 4, 90),
                    ("high", -896, 1400, 256, 16, 310), ("room", 0, -1240, 0, -6, 90),
                    ("balcony", 816, -464, 176, 20, 180), ("perch", 880, 1150, 128, 10, 180),
                    ("cover", -320, 280, 0, 2, 80), ("stairs", -400, 1320, -192, 12, 225),
                    ("keepstair", 1120, 560, 0, 12, 90), ("cliff", -880, 100, 64, 8, 165),
                    ("chasm", 300, 1360, -192, 2, 180)],
        "views": [("overview", 0, -420, 520, 34, 90)],
        "pins": [(-320, 0, 0), (320, 640, 0), (-960, 64, 64), (0, 1800, 0),
                 (992, 208, 0), (-896, 1344, 256)],
        "blurb": ("Ruins: a grassy field among broken walls under open sky, terraces "
                  "climbing west to a ruined tower, a chasm with a stone bridge to the "
                  "north, and a ruined keep and cliff ledge to the east."),
    }


# --- Forge ------------------------------------------------------------------
#
# Lava and stone, metal and fire, stars through the roof:
#
#   the foundry hall  z 0, 1280 square of stone floor, four 5 x 5 grids,
#                     lava channels kept to its west and east edges, roof
#                     trusses of angled beams, a starfield through two slots
#                     in the roof: the central spawn area
#   the forecourt     north of it, a lava pool with stone rims between twin
#                     staircases up to the altar platform (z 192) and its
#                     statue wall; the exit is behind the altar
#   the catwalks      grated, at z 192 over the lava channels and across the
#                     hall's south end: a loop round the hall, exposed
#   the undercrofts   west and east of the hall, dark low rooms behind
#                     buttressed walls: close quarters, two doors each

FORGE_STYLES = {
    "hall": {"floor": "med_csl_flr1_1", "ceil": "met_gry_slat", "wall": "med_csl_brk9_1",
             "base": ("met_blc_trim32", 32), "frieze": ("met_brn_trim32", 32)},
    "slot": {"floor": "med_csl_flr1_1", "ceil": "met_gry_slat", "wall": "met_brn_pan4",
             "sky": True, "sky_tex": "sky_star"},
    "channel": {"floor": "darkrock", "ceil": "met_gry_slat", "wall": "darkrock"},
    "altar": {"floor": "met_cop_flat", "ceil": "met_gry_slat", "wall": "med_csl_brk9_1",
              "base": ("met_blc_trim32", 32), "frieze": ("met_rune_trim32", 32)},
    "under": {"floor": "med_csl_flr4_1", "ceil": "met_gry_slat", "wall": "met_brn_pan4",
              "base": ("met_blc_trim32", 32)},
    "stair": {"floor": "met_blc_block", "ceil": "met_gry_slat", "wall": "met_brn_trim32"},
    "rock": {"floor": "darkrock", "ceil": "met_gry_slat", "wall": "med_csl_brk9_1"},
}
GRATE = "met_brn_grate3"
BEAM = "met_gry_beam"
BEAM2 = "met_mix_beam"
LAVA = "*lava1"
RIM = "met_blc_trim32"
LAVA_LIGHT = "1 0.38 0.08"
FORGE_START = {"wall": "med_csl_brk9_1", "floor": "med_csl_flr1_1", "ceil": "met_gry_slat",
               "pillar": "met_brn_trim64", "cap": "met_blc_trim32", "base": "met_blc_trim32",
               "cover": "met_blc_block"}


def _forge_sconces():
    """Sconces with their flames: along the twin staircases, in the corner
    escapes to the undercrofts and down their stairs, and over the catwalks."""
    out = []
    for design, axis, u, at, face, z in (
            # the twin staircases' outer walls, under the catwalks
            ("trapezoid", "y", 1152, -640, "+", 72), ("trapezoid", "y", 1152, 640, "-", 72),
            # the corner escapes and the undercrofts' stairs
            ("cup", "x", -900, -320, "+", 72), ("cup", "x", 900, -320, "+", 72),
            ("cup", "x", -900, 960, "-", 72), ("cup", "x", 900, 960, "-", 72),
            ("cup", "y", -96, -1088, "+", 56), ("cup", "y", -96, 1120, "-", 56),
            ("cup", "y", 896, -1088, "+", 56), ("cup", "y", 896, 1120, "-", 56),
            # over the catwalks, on the hall's walls
            ("bracket", "y", -128, -640, "+", 256), ("bracket", "y", 384, -640, "+", 256),
            ("bracket", "y", 832, -640, "+", 256), ("bracket", "y", -128, 640, "-", 256),
            ("bracket", "y", 384, 640, "-", 256), ("bracket", "y", 832, 640, "-", 256),
            ("bracket", "x", -560, -576, "+", 120), ("bracket", "x", 560, -576, "+", 120),
            ("bracket", "y", 1152, -640, "+", 256), ("bracket", "y", 1152, 640, "-", 256)):
        out.append(("sconce", dict(design=design, axis=axis, u=u, at=at, face=face, z=z,
                                   tex="met_brn_trim64")))
    return out


def _forge():
    areas = [
        {"id": "approach", "box": (-768, -576, 0, 768, -320, 512), "style": "hall", "role": "link"},
        {"id": "hall", "box": (-640, -320, 0, 640, 960, 512), "style": "hall", "role": "central"},
        {"id": "slot_w", "box": (-448, 96, 512, -96, 544, 640), "style": "slot", "role": "link"},
        {"id": "slot_e", "box": (96, 96, 512, 448, 544, 640), "style": "slot", "role": "link"},
        {"id": "forecourt", "box": (-640, 960, 0, 640, 1344, 512), "style": "hall", "role": "far"},
        # the catwalk loop's air (its grates are world pieces): the mid range
        {"id": "catwalks", "box": (-640, -320, 192, 640, 960, 512), "style": "hall",
         "role": "mid"},
        {"id": "altar", "box": (-640, 1344, 192, 640, 1600, 512), "style": "altar", "role": "high"},
        {"id": "under", "box": (-320, 1344, 0, 320, 1600, 176), "style": "under", "role": "close"},
        {"id": "under_w", "box": (-1088, 0, -64, -768, 832, 112), "style": "under", "role": "close"},
        {"id": "under_e", "box": (800, 0, -64, 1120, 832, 112), "style": "under", "role": "close"},
    ]
    carves = [
        # the lava channels along the hall's west and east edges
        {"box": (-640, -192, -64, -576, 832, 0), "style": "channel"},
        {"box": (576, -192, -64, 640, 832, 0), "style": "channel"},
        # the forecourt's lava pool, narrow: walked round, no rim
        {"box": (-192, 1056, -64, 192, 1248, 0), "style": "channel"},
        # the hall's two lava pits, down the middle between the grids
        {"box": (-144, -224, -128, 144, 192, 0), "style": "channel"},
        {"box": (-144, 448, -128, 144, 864, 0), "style": "channel"},
        # undercroft doors, past the channels' ends, two each
        {"box": (-768, -320, 0, -640, -192, 160)}, {"box": (-1088, -320, 0, -768, -192, 160)},
        {"box": (-768, 832, 0, -640, 960, 160)}, {"box": (-960, 832, 0, -768, 960, 160)},
        {"box": (640, -320, 0, 800, -192, 160)}, {"box": (800, -320, 0, 1120, -192, 160)},
        {"box": (640, 832, 0, 800, 960, 160)}, {"box": (800, 832, 0, 960, 960, 160)},
    ]
    stairs = [
        # down into the sunken undercrofts (z -64), at both ends of each
        {"rect": (-1088, -192, -960, 0), "low": -64, "high": 0, "rise": "-y", "top": 160,
         "style": "stair"},
        {"rect": (-1088, 832, -960, 960), "low": -64, "high": 0, "rise": "+y", "top": 160,
         "style": "stair"},
        {"rect": (960, -192, 1120, 0), "low": -64, "high": 0, "rise": "-y", "top": 160,
         "style": "stair"},
        {"rect": (960, 832, 1120, 960), "low": -64, "high": 0, "rise": "+y", "top": 160,
         "style": "stair"},
        # the twin staircases up to the altar, an arched tunnel under each
        # one's upper half: from the corridor outside it to behind the pool
        {"rect": (-512, 960, -384, 1344), "solid": True, "low": 0, "high": 192, "rise": "+y",
         "top": 512, "style": "stair", "tunnel": (1216, 1344, 96)},
        {"rect": (384, 960, 512, 1344), "solid": True, "low": 0, "high": 192, "rise": "+y",
         "top": 512, "style": "stair", "tunnel": (1216, 1344, 96)},
    ]
    grids = [
        {"area": "hall", "centre": (-352, 0), "cols": 4, "rows": 5},
        {"area": "hall", "centre": (352, 0), "cols": 4, "rows": 5},
        {"area": "hall", "centre": (-352, 640), "cols": 4, "rows": 5},
        {"area": "hall", "centre": (352, 640), "cols": 4, "rows": 5},
        {"area": "altar", "centre": (0, 1472), "cols": 4, "rows": 2},
        {"area": "under_w", "centre": (-928, 416), "cols": 2, "rows": 4},
        {"area": "under_e", "centre": (960, 416), "cols": 2, "rows": 4},
    ]
    world = [
        # lava, 48 deep, in the channels and the pool; stone rims round them
        ("box", dict(lo=(-640, -192, -64), hi=(-576, 832, -16), tex=LAVA)),
        ("box", dict(lo=(576, -192, -64), hi=(640, 832, -16), tex=LAVA)),
        ("box", dict(lo=(-192, 1056, -64), hi=(192, 1248, -16), tex=LAVA)),
        ("box", dict(lo=(-144, -224, -128), hi=(144, 192, -96), tex=LAVA)),
        ("box", dict(lo=(-144, 448, -128), hi=(144, 864, -96), tex=LAVA)),
        # steep funnel walls into the pits (63 degrees: nothing stands on
        # them), and a grated bridge across the north pit
        ("pit_funnel", dict(x0=-144, y0=-224, x1=144, y1=192, ztop=0, zbot=-96, inset=48,
                            tex="darkrock")),
        ("pit_funnel", dict(x0=-144, y0=448, x1=144, y1=864, ztop=0, zbot=-96, inset=48,
                            tex="darkrock")),
        ("box", dict(lo=(-32, 448, -16), hi=(32, 864, 0), tex=GRATE)),
        ("box", dict(lo=(-32, -224, -16), hi=(32, 192, 0), tex=GRATE)),
        # narrow lava channels from the side channels, along the floor
        # between the grids, each pouring over a central pit's edge
        ("lava_channel", dict(path=[(-576, 296), (-88, 296), (-88, 192)], width=16, z=0,
                              depth=24, surface=8, tex=LAVA, style="channel",
                              pour=dict(face="-y", zb=-96, lip_tex=RIM))),
        ("lava_channel", dict(path=[(576, 344), (88, 344), (88, 448)], width=16, z=0,
                              depth=24, surface=8, tex=LAVA, style="channel",
                              pour=dict(face="+y", zb=-96, lip_tex=RIM))),
        # grated catwalks at 192: over each channel, across the south end,
        # and to the altar's corners; the twin stairs climb onto them
        ("box", dict(lo=(-640, -320, 176), hi=(-560, 960, 192), tex=GRATE)),
        ("box", dict(lo=(560, -320, 176), hi=(640, 960, 192), tex=GRATE)),
        ("box", dict(lo=(-560, -320, 176), hi=(560, -240, 192), tex=GRATE)),
        ("box", dict(lo=(-640, 960, 176), hi=(-512, 1344, 192), tex=GRATE)),
        ("box", dict(lo=(512, 960, 176), hi=(640, 1344, 192), tex=GRATE)),
        # kick rails on the catwalks' open sides (24: the player jumps them)
        ("box", dict(lo=(-568, -240, 192), hi=(-560, 960, 216), tex=RIM)),
        ("box", dict(lo=(560, -240, 192), hi=(568, 960, 216), tex=RIM)),
        ("box", dict(lo=(-560, -248, 192), hi=(-96, -240, 216), tex=RIM)),
        ("box", dict(lo=(96, -248, 192), hi=(560, -240, 216), tex=RIM)),
    ]
    pieces = []
    # roof trusses: angled beams across the hall, a tie at their feet
    for y in (-192, 160, 480, 832):
        pieces.append(("beam", dict(a=(-640, y, 448), b=(0, y, 504), w=16, h=16, tex=BEAM)))
        pieces.append(("beam", dict(a=(0, y, 504), b=(640, y, 448), w=16, h=16, tex=BEAM)))
        pieces.append(("box", dict(lo=(-640, y - 8, 432), hi=(640, y + 8, 448), tex=BEAM2)))
    # buttresses against the forecourt's side walls, under the catwalks'
    # north stretch (none across the catwalks themselves)
    for x, f in ((-640, "+x"), (640, "-x")):
        for y in (1040, 1264):
            pieces.append(("buttress", dict(x=x, y=y, face=f, width=24, z0=0, z1=176,
                                            depth=48, tex="met_brn_trim64")))
    # beams under the catwalks, to the walls
    for y in (-64, 320, 704):
        pieces.append(("beam", dict(a=(-640, y, 64), b=(-560, y, 176), w=16, h=16, tex=BEAM)))
        pieces.append(("beam", dict(a=(640, y, 64), b=(560, y, 176), w=16, h=16, tex=BEAM)))
    # the statue wall behind the altar: rune and skull panels, two flames
    for x0 in (-384, -256, 128, 256):
        pieces.append(("box", dict(lo=(x0, 1596, 256), hi=(x0 + 128, 1600, 384), tex="may_tskull")))
    for x in (-224, 224):
        pieces.append(("brazier", dict(x=x, y=1560, z=192)))
    # posts under the catwalks' inner edges, on the channels' rims
    for x in (-592, 576):   # standing in the lava, clear of the floor's cells
        for y in range(-192, 833, 256):
            pieces.append(("shaft", dict(cx=x + 8, cy=y, r=8, z0=-64, z1=176,
                                          tex="met_brn_trim64")))
    # braziers in the hall's cross, between the grids
    for x, y in ((0, -280), (-104, 320), (104, 320), (0, 920)):
        pieces.append(("brazier", dict(x=x, y=y, z=0)))
    # torches in the undercrofts and the approach
    for x in (-1088, 1120):
        f = "+" if x < 0 else "-"
        for y in (224, 608):
            pieces.append(("alcove_torch", dict(axis="y", u=y, at=x, face=f, z=8,
                                                tex="met_blc_trim32")))
    for x in (-320, 320):
        pieces.append(("alcove_torch", dict(axis="x", u=x, at=-576, face="+", z=72,
                                            tex="met_blc_trim32")))
    pieces += _forge_sconces()
    # arch frames round the tunnels' mouths under the twin staircases
    for x in (-512, -384, 384, 512):
        pieces.append(("arch", dict(axis="y", at=x, lo=1216, hi=1344, z0=0, spring=64, top=96,
                                    depth=16, tex="met_brn_trim64", kind="octagonal", z1=112,
                                    jamb=16)))

    items = [
        ("weapon_supershotgun", -544, -448, 16), ("item_shells", 544, -480, 16),
        ("weapon_nailgun", -1040, 760, -48), ("item_spikes", 1060, 760, -48),
        ("item_health", 0, 240, 16), ("item_health", 0, 400, 16),
        ("item_shells", -160, 320, 16, 1), ("item_spikes", 160, 320, 16, 1),
        ("item_armor1", 280, 1450, 16), ("item_health", -440, 1376, 208),
        # behind the pool, through the tunnels or round it: the yellow armour
        ("item_armor2", 0, 1296, 16),
        # the bridge over the north pit carries the strongest guns: the
        # prize for crossing above the lava
        ("weapon_rocketlauncher", 0, 592, 16), ("weapon_supernailgun", 0, 720, 16),
        ("item_health", 440, 1376, 208), ("item_health", -560, 1500, 208, 2),
    ]
    supply = [(-520, -280, 16), (520, -280, 16), (-520, 920, 16), (520, 920, 16),
              (-260, 930, 16), (228, 930, 16), (-616, 520, 208), (584, 520, 208),
              (-1040, 80, -48), (1060, 80, -48), (-240, 1560, 16), (240, 1560, 16)]
    lights = []
    for x in (-608, 608):
        for y in range(-160, 833, 192):
            lights.append((x, y, 24, 240, LAVA_LIGHT, 0.8))
            lights.append((x, y, 300, 160, LAVA_LIGHT))
    for x in (-128, 0, 128):
        for y in (1104, 1200):
            lights.append((x, y, 24, 260, LAVA_LIGHT, 0.8))
    # the tunnels under the twin staircases, and behind the pool
    lights += [(x, 1280, 56, 220, "1 0.8 0.6") for x in (-448, 448)]
    lights += [(x, 1296, 72, 220, "1 0.8 0.6") for x in (-256, 256)]
    for x in (-320, 0, 320):
        for y in (-160, 160, 480, 800):
            lights.append((x, y, 360, 300, "1 0.82 0.62"))
    for x in (-400, 0, 400):
        lights.append((x, 1150, 380, 260, "1 0.8 0.6"))
    for x in (-600, 600):
        for y in (-280, 320, 920):
            lights.append((x, y, 260, 200, "1 0.7 0.45"))
    lights += [(0, 320, 300, 200), (0, 1472, 400, 260), (0, 1472, 120, 120)]
    lights += [(x, -448, 200, 260) for x in (-480, 0, 480)]
    lights += [(x, y, 56, 220, "1 0.8 0.6") for x in (-928, 960) for y in (100, 400, 700)]
    # the twin staircases, the corner escapes and their stairs, the catwalks
    lights += [(x, y, 240, 300, "1 0.8 0.6") for x in (-448, 448) for y in (1040, 1240)]
    lights += [(x, y, 200, 300, "1 0.8 0.6") for x in (-256, 256) for y in (560, 600)]
    lights += [(x, 1470, 120, 240, "1 0.8 0.6") for x in (-160, 160)]
    lights += [(x, -500, 120, 260, "1 0.8 0.6") for x in (-640, 640)]
    lights += [(x, y, 96, 260, "1 0.8 0.6") for x in (-1024, -704, 704, 1040)
               for y in (-256, 896)]
    lights += [(x, -96, 40, 220, "1 0.8 0.6") for x in (-1024, 1040)]
    lights += [(x, y, 290, 240, "1 0.8 0.6") for x in (-600, 600)
               for y in (-256, 64, 384, 704, 960, 1200)]
    lights += [(x, -280, 290, 240, "1 0.8 0.6") for x in (-320, 0, 320)]
    lights += [(x, y, 120, 280, "1 0.8 0.6") for x in (-256, 256, -576, 576) for y in (1100, 1250)]
    # the hall's lava pits glow from inside
    lights += [(x, y, -48, 320, LAVA_LIGHT, 0.8) for x in (-72, 72)
               for y in (-120, 88, 552, 760)]
    return {
        "name": "psqforge", "title": "psQuake Arena: Forge",
        # places worth going that must be walked to, no jump or drop
        "walkto": [(0, 1296, 0)],
        "bounds": (-1408, -1408, -256, 1408, 2048, 768),
        "styles": FORGE_STYLES, "default": "rock", "start_theme": FORGE_START,
        "areas": areas, "carves": carves, "stairs": stairs, "grids": grids,
        "central": "hall", "pieces": pieces, "world_pieces": world,
        "items": items, "supply": supply, "lights": lights,
        "sun": {"_sunlight": "120", "_sunlight_color": "0.7 0.75 1", "_sun_mangle": "0 -80 0",
                "_sunlight2": "40", "_sunlight2_color": "0.6 0.65 1"},
        "balcony": (-464, 176), "hall_top": 512, "exit": (1600, 192),
        "exit_map": "psqarena",
        "sightline": ((0, -280, 46), (0, 1500, 238)),
        "cameras": [("start", 0, -528, 0, 8, 90), ("central", 0, 120, 0, 2, 90),
                    ("close", -928, 120, -64, 0, 90), ("far", 144, -280, 0, 4, 90),
                    ("high", 0, 1400, 192, 18, 270), ("room", 0, -1240, 0, -6, 90),
                    ("balcony", 816, -464, 176, 20, 180), ("perch", -600, 880, 192, 14, 315),
                    ("cover", 0, 220, 0, 2, 100), ("stairs", 160, 940, 0, 10, 100),
                    ("corner", -560, -270, 0, 4, 180), ("pit", 0, 420, 0, 14, 270),
                    ("catwalk", -600, -280, 192, 6, 90)],
        "views": [("overview", 0, -150, 430, 32, 90)],
        "pins": [(-320, 0, 0), (320, 640, 0), (-928, 416, -64), (0, 1472, 192),
                 (960, 416, -64), (320, 0, 0)],
        "blurb": ("Forge: a foundry hall with lava channels at its edges and stars through "
                  "the roof, the strongest guns on a bridge over a lava pit, twin stairs over a lava pool to the altar, grated catwalks round "
                  "the hall and dark undercrofts either side."),
    }


MAPS = {"psqtemple": _temple(), "psqruins": _ruins(), "psqforge": _forge()}
