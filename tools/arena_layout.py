"""Build a themed psQuake arena from its declarative layout (arena_maps.py).

A layout is data, not code: the open spaces of the map as boxes, the stair
runs between their heights, the spawn grids, the pieces (arena_kit.py) set
into it, its pickups, resupply spots, lights and camera stands. build()
turns it into the brushes and entities generate_arena.map_text writes,
and a description for the plan, the shots and the validator:

    name, title          the map name (psq<name>) and its worldspawn message
    bounds               (x0, y0, z0, x1, y1, z1): solid everywhere inside,
                         but for the carved spaces; the outside is sealed
    styles, default      named texture sets {"floor", "ceil", "wall"} (and
                         "sky": True with "sky_tex" for a skylight: the
                         solid over that space is sky)
    areas                [{"id", "box": (x0, y0, z0, x1, y1, z1), "style",
                         "role"}]: each an open box, its floor at z0; role
                         one of central, far, mid, close, high, link
    stairs               [{"rect": (x0, y0, x1, y1), "low", "high", "rise",
                         "top", "style"}]: a stair run carved from `low` at
                         one end to `high` at the end `rise` points to
                         ('+x', '-x', '+y', '-y'), 16 a step, open to top;
                         "solid": True for a run that climbs inside an
                         open area (its steps are brushes, the well over
                         it opened)
    carves               extra open boxes (doorways), as areas
    grids                [{"area", "centre": (x, y), "cols", "rows"}]: spawn
                         grids of 96-unit cells on that area's floor; the
                         first is the psq_arena entity's
    central              the id of the central spawn area
    pieces               [(kind, {args})]: arena_kit pieces, each set as
                         func_detail; "alcove_torch" also gives its torch
    world_pieces         pieces that stay in the world (parapets, the lip)
    items, supply        as generate_arena.py's layouts
    lights               [(x, y, z, light[, _color[, wait]])]; plus "sun"
                         worldspawn keys
    balcony              (y centre, floor) of the escape's balcony
    exit                 (y of the hall's north wall, its floor): the exit
                         alcove behind it
    exit_map             where the exit leads
    sightline            ((x, y, z), (x, y, z)): the far sightline the
                         validator measures (eye to eye, clear)
    cameras, views, pins as generate_arena.py's layouts (--shots)

The start room, its arches and lip, the escape hallway and balcony and the
shoot-to-open exit door are generate_arena.py's own (start_room, escape,
exit_alcove), set into the carved spaces they need, so they carry over
unchanged. The hall they open on runs from y -576 (the start room's
doorway) and has its east wall at x 768 where the escape runs.

Python 3 standard library only. Output is deterministic.
"""

from __future__ import annotations

import math

import arena_kit as K
import generate_arena as G

CELL = G.CELL
STEP = 16


class LayoutError(Exception):
    pass


def stair_ends(s, out=24):
    """((x, y, z) where the run is entered at its low end, (x, y, z) where
    it is left at its high end): the middle of each end, out beyond it."""
    if s.get("kind") == "spiral":
        cx, cy, r0, r1, a0, a1 = s["centre"] + (s["r0"], s["r1"], s["a0"], s["a1"])
        rm = (r0 + r1) / 2
        sgn = 1 if a1 > a0 else -1
        ends = []
        for a, back in ((a0, -1), (a1, 1)):
            t = math.radians(a)
            # out along the tangent, away from the run
            tx, ty = -math.sin(t) * sgn * back, math.cos(t) * sgn * back
            ends.append((cx + rm * math.cos(t) + tx * out, cy + rm * math.sin(t) + ty * out))
        return (ends[0] + (s["low"],), ends[1] + (s["high"],))
    x0, y0, x1, y1 = s["rect"]
    mx, my = (x0 + x1) / 2, (y0 + y1) / 2
    pts = {"+x": ((x0 - out, my), (x1 + out, my)), "-x": ((x1 + out, my), (x0 - out, my)),
           "+y": ((mx, y0 - out), (mx, y1 + out)), "-y": ((mx, y1 + out), (mx, y0 - out))}
    (lx, ly), (hx, hy) = pts[s["rise"]]
    return (lx, ly, s["low"]), (hx, hy, s["high"])


def stair_top_sides(s, out=24):
    """The two points beside a straight run's top step, out from its sides:
    a run climbing a stairwell is left sideways onto the floor round it."""
    if s.get("kind") == "spiral":
        return []
    x0, y0, x1, y1 = s["rect"]
    n = (s["high"] - s["low"]) // STEP
    if s["rise"][1] == "y":
        d = (y1 - y0) / n
        ty = y1 - d / 2 if s["rise"][0] == "+" else y0 + d / 2
        return [(x0 - out, ty, s["high"]), (x1 + out, ty, s["high"])]
    d = (x1 - x0) / n
    tx = x1 - d / 2 if s["rise"][0] == "+" else x0 + d / 2
    return [(tx, y0 - out, s["high"]), (tx, y1 + out, s["high"])]


def _stair_segments(s):
    x0, y0, x1, y1 = s["rect"]
    lo, hi, rise = s["low"], s["high"], s["rise"]
    n = (hi - lo) // STEP
    if n < 1 or n * STEP != hi - lo:
        raise LayoutError(f"stairs {s['rect']}: {lo}..{hi} is not whole {STEP} steps")
    along_x = rise[1] == "x"
    a0, a1 = (x0, x1) if along_x else (y0, y1)
    run = (a1 - a0) // n
    if run * n != a1 - a0 or run % 16:
        raise LayoutError(f"stairs {s['rect']}: {a1 - a0} does not split into {n} steps "
                          "on the 16 grid")
    if run < 2 * STEP:
        raise LayoutError(f"stairs {s['rect']}: steps {run} deep, steeper than 16:32")
    out = []
    for i in range(n):
        a, b = a0 + run * i, a0 + run * (i + 1)
        k = i if rise[0] == "+" else n - 1 - i
        z = lo + STEP * (k + 1)
        out.append(((a, y0, b, y1) if along_x else (x0, a, x1, b), z))
    return out


def _area(L, aid):
    for a in L["areas"]:
        if a["id"] == aid:
            return a
    raise LayoutError(f"no area {aid}")


def build(L):
    """(world brushes, entities, description) for layout L."""
    sp = K.Space(L["bounds"], 16)
    styles = L["styles"]
    for a in L["areas"] + L.get("carves", []):
        sp.carve(*a["box"], a.get("style", L["default"]))
    step_brushes = []
    for s in L.get("stairs", []):
        if s.get("kind") == "spiral":
            # a curved run inside an open area: its wedge steps are brushes
            tex = styles[s.get("style", L["default"])]
            cx, cy = s["centre"]
            step_brushes += K.spiral_stairs(cx, cy, s["r0"], s["r1"], s["a0"], s["a1"],
                                            s["low"], s["high"], s["steps"],
                                            {"side": tex["wall"], "top": tex["floor"],
                                             "bottom": tex["ceil"]})
            continue
        if s.get("solid"):
            # A run inside an open area (up to a gallery): its steps are
            # brushes, and the well over it is opened to its top.
            x0, y0, x1, y1 = s["rect"]
            _stair_segments(s)   # the same checks
            sp.carve(x0, y0, s["low"], x1, y1, s["top"], s.get("style", L["default"]))
            tex = styles[s.get("style", L["default"])]
            step_brushes += K.stairs(x0, y0, x1, y1, s["low"], s["high"], s["rise"],
                                     {"side": tex["wall"], "top": tex["floor"],
                                      "bottom": tex["ceil"]})
            continue
        for (x0, y0, x1, y1), z in _stair_segments(s):
            sp.carve(x0, y0, z, x1, y1, s["top"], s.get("style", L["default"]))
    T = L["start_theme"]
    # The start room, the escape and the exit alcove: their insides carved,
    # their brushes generate_arena's.
    x0, y0, z0, x1, y1, z1 = G.START_ROOM
    yc, H = L["balcony"]
    yb0, yb1 = yc - G.BALCONY_HALF, yc + G.BALCONY_HALF
    zc = H + G.BALCONY_HEAD
    ey0, ey1 = G.ESC_Y
    ex0, ex1 = G.ESC_X
    ny, ez = L["exit"]
    for b in ((x0, y0, z0, x1, y1, z1),
              (-G.DOOR_HALF, y1, 0, G.DOOR_HALF, y1 + 2 * G.WALL, G.START_DOOR_TOP),
              (x1, ey0, 0, x1 + G.WALL, ey1, G.ESC_TOP),
              (x1 + G.WALL, ey0, 0, ex0, ey1, G.ESC_TOP),
              (ex0, ey0, 0, ex1, yb1, zc),
              (ex0 - G.WALL, yb0, H, ex0, yb1, zc),
              (-128, ny + 2 * G.WALL, ez, 128, ny + 2 * G.WALL + G.EXIT_DEPTH, ez + 160),
              (-G.DOOR_HALF, ny, ez, G.DOOR_HALF, ny + 2 * G.WALL, ez + G.DOOR_TOP)):
        sp.carve(*b, L["default"])

    world = sp.emit(styles, L["default"]) + step_brushes
    G._FIT = True
    try:
        start_ents = G.start_room(world, T)
        esc_ents, _ = G.escape(world, T, (-768, -576, 768, ny), L["balcony"], L["hall_top"])
        exit_room, exit_ents = G.exit_alcove(world, ny, ez, L["exit_map"])
    finally:
        G._FIT = False

    ents = [({"classname": "info_player_start",
              "origin": " ".join(str(v) for v in G.START), "angle": "90"}, [])]
    regions = []
    for i, g in enumerate(L["grids"]):
        a = _area(L, g["area"])
        cx, cy = g["centre"]
        z = a["box"][2]
        w, h = g["cols"] * CELL, g["rows"] * CELL
        regions.append({"origin": (cx, cy, z), "cols": g["cols"], "rows": g["rows"],
                        "rect": (cx - w // 2, cy - h // 2, cx + w // 2, cy + h // 2),
                        "area": g["area"]})
        ents.append(({"classname": "psq_arena" if i == 0 else "psq_grid",
                      "origin": f"{cx} {cy} {z}", "psq_cols": str(g["cols"]),
                      "psq_rows": str(g["rows"]), "psq_cell": str(CELL)}, []))
    ents.extend(start_ents)
    ents.extend(esc_ents)
    ents.extend(exit_ents)
    ents.append(({"classname": "info_null",
                  "origin": f"{G.HALL_ENTRY[0]} {G.HALL_ENTRY[1] + 80} {G.HALL_ENTRY[2] + 40}"},
                 []))
    for x, y, z in L.get("supply", ()):
        ents.append(({"classname": "psq_supply", "origin": f"{x} {y} {z}"}, []))
    for item in L["items"]:
        cls, x, y, z = item[:4]
        keys = {"classname": cls, "origin": f"{x} {y} {z}"}
        if len(item) > 4:
            keys["spawnflags"] = str(item[4])
        ents.append((keys, []))
    for lt in L.get("lights", ()):
        x, y, z, lv = lt[:4]
        keys = {"classname": "light", "origin": f"{x} {y} {z}", "light": str(lv)}
        if len(lt) > 4 and lt[4]:
            keys["_color"] = lt[4]
        if len(lt) > 5:
            keys["wait"] = f"{lt[5]:g}"
        ents.append((keys, []))

    plan_pieces = []
    for kind, args in L.get("world_pieces", []):
        brushes, more = _piece(kind, args)
        world.extend(brushes)
        ents.extend(more)
        plan_pieces.append((kind, args))
    for kind, args in L.get("pieces", []):
        brushes, more = _piece(kind, args)
        if brushes:
            ents.append(({"classname": "func_detail"}, brushes))
        ents.extend(more)
        plan_pieces.append((kind, args))

    c = _area(L, L["central"])["box"]
    sl0, sl1 = L["sightline"]
    D = dict(L)
    D.update({
        "space": sp,
        "regions": regions, "exit_room": exit_room, "plan_pieces": plan_pieces,
        "hall": (-768, -576, 768, ny),
        "worldkeys": {
            "light": str(G.LIGHT_MIN), "_minlight_dirt": "0", "_dirt": G.LIGHT_DIRT,
            "_dirtscale": G.LIGHT_DIRT_SCALE,
            "_psq_rules": G.RULES + " " + L.get("rules", "central levels sightline drops sky water framed walkways routelight"),
            "_psq_hall_entry": " ".join(str(v) for v in G.HALL_ENTRY),
            "_psq_central": f"{c[0]} {c[1]} {c[3]} {c[4]} {c[2]}",
            "_psq_sightline": " ".join(str(v) for v in sl0 + sl1),
            **L.get("sun", {})},
    })
    return world, ents, D


def _piece(kind, a):
    """(brushes, entities) of one piece."""
    if kind == "alcove_torch":
        return K.alcove_torch(**a)
    if kind == "torch":
        if "colour" in a:   # a plain coloured light (stained glass lit from behind)
            return [], [({"classname": "light", "origin": f"{a['x']} {a['y']} {a['z']}",
                          "light": str(a["light"]), "_color": a["colour"]}, [])]
        return [], K.fire_ents(a["x"], a["y"], a["z"])
    if kind == "brazier":   # a large flame standing on the floor, its warm pool round it
        return [], K.fire_ents(a["x"], a["y"], a["z"] + 8, K.FLAME)
    if kind == "sconce":
        a = dict(a)
        return K.SCONCES[a.pop("design")](**a)
    fn = {"column": K.column, "colonnade": K.colonnade, "arch": K.arch, "stairs": K.stairs,
          "ramp": K.ramp, "beam": K.beam, "buttress": K.buttress, "rocks": K.rocks,
          "trim": K.trim, "frame": K.frame, "lean_wall": K.lean_wall, "strata": K.strata,
          "pit_funnel": K.pit_funnel}.get(kind)
    if fn is not None:
        return fn(**a), []
    if kind == "hull":
        return [K.hull(a["points"], a["tex"])], []
    if kind == "box":
        return [K.box(a["lo"], a["hi"], a["tex"])], []
    raise LayoutError(f"unknown piece {kind}")


# --- the plan --------------------------------------------------------------


def plan_svg(D):
    """A top-down SVG plan: the areas coloured by floor height, stair runs,
    the spawn grids' cells, the start room and the central area's share of
    the spawn cells."""
    bx0, by0, _, bx1, by1, _ = D["bounds"]
    W, Hh = bx1 - bx0, by1 - by0
    s = 900 / max(W, Hh)

    def X(x):
        return (x - bx0) * s + 20

    def Y(y):
        return (by1 - y) * s + 60

    def rect(x0, y0, x1, y1, fill, stroke="none", op=1.0, extra=""):
        return (f'<rect x="{X(x0):.1f}" y="{Y(y1):.1f}" width="{(x1 - x0) * s:.1f}" '
                f'height="{(y1 - y0) * s:.1f}" fill="{fill}" fill-opacity="{op}" '
                f'stroke="{stroke}" {extra}/>')

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W * s + 300:.0f}" '
           f'height="{Hh * s + 120:.0f}" font-family="Helvetica, Arial, sans-serif">',
           '<rect width="100%" height="100%" fill="#1d1b19"/>']
    total = sum(r["cols"] * r["rows"] for r in D["regions"])
    central = sum(r["cols"] * r["rows"] for r in D["regions"] if r["area"] == D["central"])
    out.append(f'<text x="20" y="28" fill="#eee" font-size="18">{D["title"]} ({D["name"]}): '
               f'plan, blockout</text>')
    out.append(f'<text x="20" y="48" fill="#ccc" font-size="13">central area: {central} of '
               f'{total} spawn cells ({100 * central / total:.0f}%); colour = floor height</text>')
    for a in sorted(D["areas"], key=lambda a: a["box"][2]):
        x0, y0, z0, x1, y1, _ = a["box"]
        out.append(rect(x0, y0, x1, y1, G._height_colour(z0), "#000", 0.85))
    for st in D.get("stairs", []):
        if st.get("kind") == "spiral":
            cx, cy = st["centre"]
            for k in range(st["steps"]):
                t = math.radians(st["a0"] + (st["a1"] - st["a0"]) * (k + 0.5) / st["steps"])
                rm = (st["r0"] + st["r1"]) / 2
                z = st["low"] + (st["high"] - st["low"]) * (k + 1) // st["steps"]
                out.append(f'<circle cx="{X(cx + rm * math.cos(t)):.1f}" '
                           f'cy="{Y(cy + rm * math.sin(t)):.1f}" '
                           f'r="{(st["r1"] - st["r0"]) * s / 2:.1f}" '
                           f'fill="{G._height_colour(z)}" fill-opacity="0.9"/>')
            continue
        for (x0, y0, x1, y1), z in _stair_segments(st):
            out.append(rect(x0, y0, x1, y1, G._height_colour(z), "#333", 0.9))
    sx0, sy0, _, sx1, sy1, _ = G.START_ROOM
    out.append(rect(sx0, sy0, sx1, sy1, "#5a7", "#000", 0.8))
    for reg in D["regions"]:
        x0, y0, x1, y1 = reg["rect"]
        col = "#ffcc33" if reg["area"] == D["central"] else "#66ddff"
        for i in range(reg["cols"]):
            for j in range(reg["rows"]):
                out.append(rect(x0 + i * CELL + 6, y0 + j * CELL + 6, x0 + (i + 1) * CELL - 6,
                                y0 + (j + 1) * CELL - 6, col, "none", 0.55))
    for kind, a in D["plan_pieces"]:
        if kind == "column":
            out.append(f'<circle cx="{X(a["cx"]):.1f}" cy="{Y(a["cy"]):.1f}" '
                       f'r="{a["r"] * s:.1f}" fill="#222"/>')
        if kind == "colonnade":
            n = max(1, int(round((a["b"] - a["a"]) / a["spacing"])))
            for i in range(n + 1):
                p = a["a"] + (a["b"] - a["a"]) * i // n
                cx, cy = (p, a["at"]) if a["axis"] == "x" else (a["at"], p)
                out.append(f'<circle cx="{X(cx):.1f}" cy="{Y(cy):.1f}" r="{a["r"] * s:.1f}" '
                           'fill="#222"/>')
        if kind in ("alcove_torch", "torch", "sconce"):
            if kind == "torch":
                tx, ty = a["x"], a["y"]
            else:
                tx, ty = (a["u"], a["at"]) if a["axis"] == "x" else (a["at"], a["u"])
            out.append(f'<circle cx="{X(tx):.1f}" cy="{Y(ty):.1f}" r="4" fill="#ff7a1a"/>')
    (p0, p1) = D["sightline"]
    out.append(f'<line x1="{X(p0[0]):.1f}" y1="{Y(p0[1]):.1f}" x2="{X(p1[0]):.1f}" '
               f'y2="{Y(p1[1]):.1f}" stroke="#f55" stroke-dasharray="6 4" stroke-width="2"/>')
    for x, y, z in D.get("supply", ()):
        out.append(f'<circle cx="{X(x):.1f}" cy="{Y(y):.1f}" r="5" fill="#4f4"/>')
    for it in D["items"]:
        out.append(f'<circle cx="{X(it[1]):.1f}" cy="{Y(it[2]):.1f}" r="4" fill="#fff"/>')
    ly = 80
    for label, col in (("central spawn cells", "#ffcc33"), ("side spawn cells", "#66ddff"),
                       ("start room", "#5a7"), ("resupply spot", "#4f4"), ("pickup", "#fff"),
                       ("torch", "#ff7a1a"), ("far sightline", "#f55")):
        out.append(f'<rect x="{W * s + 40:.0f}" y="{ly}" width="14" height="14" fill="{col}"/>')
        out.append(f'<text x="{W * s + 62:.0f}" y="{ly + 12}" fill="#ddd" font-size="13">'
                   f'{label}</text>')
        ly += 22
    for z in sorted({a["box"][2] for a in D["areas"]}):
        out.append(f'<rect x="{W * s + 40:.0f}" y="{ly}" width="14" height="14" '
                   f'fill="{G._height_colour(z)}"/>')
        out.append(f'<text x="{W * s + 62:.0f}" y="{ly + 12}" fill="#ddd" font-size="13">'
                   f'floor z {z}</text>')
        ly += 22
    out.append("</svg>\n")
    return "\n".join(out)
