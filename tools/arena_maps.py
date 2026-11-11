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

STYLES = {
    "stone": {"floor": "floor_grey_c", "ceil": "wall_grey_c", "wall": "wall_tan_c"},
    "court": {"floor": "floor_orange_c", "ceil": "wall_grey_c", "wall": "wall_tan_c",
              "sky": True, "sky_tex": "sky_orng"},
    "high": {"floor": "floor_red_c", "ceil": "wall_grey_c", "wall": "wall_tan_c"},
    "close": {"floor": "floor_grey_c", "ceil": "wall_grey_c", "wall": "wall_green_a"},
    "stair": {"floor": "wall_grey_a", "ceil": "wall_grey_c", "wall": "wall_tan_c"},
    "nave": {"floor": "floor_grey_c", "ceil": "wall_grey_c", "wall": "wall_pink_a"},
}
DETAIL = "wall_blue_a"     # columns, arches, alcoves
TRIM = "wall_blue_b"
COVER = "wall_orange_a"

# The start room's theme (generate_arena.start_room, escape): placeholders.
START_THEME = {"wall": "wall_tan_c", "floor": "floor_grey_c", "ceil": "wall_grey_c",
               "pillar": "wall_grey_a", "cap": DETAIL, "base": TRIM, "cover": COVER}


def _torches_court():
    out = []
    # on the courtyard's sunken walls, between the stair runs
    for y in (96, 928):
        out.append(("alcove_torch", dict(axis="y", u=y, at=-576, face="+", z=-112, tex=DETAIL)))
        out.append(("alcove_torch", dict(axis="y", u=y, at=576, face="-", z=-112, tex=DETAIL)))
    for x in (-448, 448):
        out.append(("alcove_torch", dict(axis="x", u=x, at=-64, face="+", z=-112, tex=DETAIL)))
        out.append(("alcove_torch", dict(axis="x", u=x, at=1088, face="-", z=-112, tex=DETAIL)))
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
                                         spacing=224, tex=DETAIL)))
    for a, b in ((-768, -352), (352, 768)):
        pieces.append(("colonnade", dict(axis="x", a=a, b=b, at=1120, r=24, z0=0, z1=400,
                                         spacing=208, tex=DETAIL)))
    for a, b in ((-544, -352), (352, 544)):
        pieces.append(("colonnade", dict(axis="x", a=a, b=b, at=-96, r=24, z0=0, z1=400,
                                         spacing=192, tex=DETAIL)))
    for at in (-824, 824):   # under the galleries' edges, not out past them
        for a, b in ((-128, 128), (896, 1280)):
            pieces.append(("colonnade", dict(axis="y", a=a, b=b, at=at, r=16, z0=0, z1=176,
                                             spacing=128, tex=DETAIL)))
    for at in (-256, 256):
        pieces.append(("colonnade", dict(axis="y", a=1408, b=1856, at=at, r=24, z0=0, z1=224,
                                         spacing=224, tex=DETAIL)))
    # octagonal arch frames down the nave, a pointed one over the south stairs
    for y in (1376, 1520, 1744):
        pieces.append(("arch", dict(axis="x", at=y, lo=-256, hi=256, z0=0, spring=288, top=352,
                                    depth=32, tex=DETAIL, kind="octagonal", z1=448)))
    pieces.append(("arch", dict(axis="x", at=-336, lo=-320, hi=320, z0=0, spring=224, top=288,
                                depth=32, tex=DETAIL, kind="pointed", z1=320)))
    # a coffered nave ceiling: beams across and along it
    for y in range(1440, 1921, 160):
        pieces.append(("box", dict(lo=(-256, y - 8, 416), hi=(256, y + 8, 448), tex=TRIM)))
    for x in (-128, 128):
        pieces.append(("box", dict(lo=(x - 8, 1344, 416), hi=(x + 8, 1920, 448), tex=TRIM)))
    # torch alcoves
    for y in (384, 640):
        pieces.append(("alcove_torch", dict(axis="y", u=y, at=-1088, face="+", z=48, tex=DETAIL)))
        pieces.append(("alcove_torch", dict(axis="y", u=y, at=1088, face="-", z=48, tex=DETAIL)))
    for y in (1520, 1744, 2016):
        pieces.append(("alcove_torch", dict(axis="y", u=y, at=-448, face="+", z=48, tex=DETAIL)))
        pieces.append(("alcove_torch", dict(axis="y", u=y, at=448, face="-", z=48, tex=DETAIL)))
    for x, f in ((-1376, "+"), (1376, "-")):
        for y in (128, 896):
            pieces.append(("alcove_torch", dict(axis="y", u=y, at=x, face=f, z=48, tex=DETAIL)))
        pieces.append(("alcove_torch", dict(axis="y", u=512, at=x, face=f, z=48, tex=DETAIL)))
    pieces += _torches_court()
    world_pieces = [
        # the courtyard's low cover: a plinth in the middle and four blocks
        ("box", dict(lo=(-40, 472, -128), hi=(40, 552, -88), tex=COVER)),
        ("box", dict(lo=(-32, 96, -128), hi=(32, 160, -88), tex=COVER)),
        ("box", dict(lo=(-32, 864, -128), hi=(32, 928, -88), tex=COVER)),
        ("box", dict(lo=(-320, 480, -128), hi=(-256, 544, -88), tex=COVER)),
        ("box", dict(lo=(256, 480, -128), hi=(320, 544, -88), tex=COVER)),
        # gallery parapets (32: the player jumps them), gaps to drop from
        ("box", dict(lo=(-816, -256, 192), hi=(-800, 256, 224), tex=TRIM)),
        ("box", dict(lo=(-816, 768, 192), hi=(-800, 1280, 224), tex=TRIM)),
        ("box", dict(lo=(800, -256, 192), hi=(816, 256, 224), tex=TRIM)),
        ("box", dict(lo=(800, 768, 192), hi=(816, 1280, 224), tex=TRIM)),
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
            lights.append((x, y, 200, 420))
    for x in (-944, 944):
        for y in (-192, 192, 576, 960, 1280):
            lights.append((x, y, 140, 320))
            lights.append((x, y, 340, 300))
    for x in (-688, 688):
        for y in (-160, 576, 1216):
            lights.append((x, y, 300, 340))
    for y in (1440, 1680, 1880, 2176):
        lights.append((0, y, 360, 440))
        for x in (-352, 352):
            lights.append((x, y, 180, 280))
    for x in (-512, 0, 512):
        lights.append((x, -448, 220, 420))
    for x, y in ((-1248, 128), (-1248, 896), (1248, 128), (1248, 896), (-1312, 512),
                 (1312, 512)):
        lights.append((x, y, 140, 240))
    return {
        "name": "psqtemple", "title": "psQuake Arena: Temple",
        "bounds": (-1408, -1408, -256, 1408, 2720, 720),
        "styles": STYLES, "default": "stone", "start_theme": START_THEME,
        "areas": areas, "carves": carves, "stairs": stairs, "grids": grids,
        "central": "court", "pieces": pieces, "world_pieces": world_pieces,
        "items": items, "supply": supply, "lights": lights,
        "sun": {"_sunlight": "160", "_sunlight_color": "1 0.6 0.55", "_sun_mangle": "20 -65 0",
                "_sunlight2": "70", "_sunlight2_color": "1 0.55 0.6"},
        "balcony": (-464, 176), "hall_top": 320, "exit": (2304, 64),
        "exit_map": "psqarena",
        "sightline": ((0, -48, -82), (0, 1952, 62)),
        "cameras": [("start", 0, -528, 0, 10, 90), ("central", 0, 32, -128, 2, 90),
                    ("close", -1312, 300, 0, 0, 90), ("far", 0, 1380, 0, 2, 90),
                    ("high", -944, 640, 192, 18, 0), ("room", 0, -1240, 0, -6, 90),
                    ("balcony", 816, -464, 176, 20, 180)],
        "views": [("overview", 0, -40, 400, 38, 90)],
        "pins": [(-288, 224, -128), (288, 800, -128), (-944, 368, 0), (0, 1632, 0),
                 (944, 704, 192), (-1248, 896, 0)],
        "blurb": ("Temple: a sunken, sky-lit courtyard ringed by colonnades, with side "
                  "aisles and galleries, chapels and a crypt behind them, and a long nave "
                  "to the dais and the exit."),
    }


MAPS = {"psqtemple": _temple()}
