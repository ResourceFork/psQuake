"""psQuake's building-piece library: brushes and pieces for the themed arenas.

The banked arenas keep their own generator (generate_arena.py); the themed
arenas (Temple, then Ruins and Forge) are built from these pieces by
arena_layout.py, from the declarative layouts in arena_maps.py. Every piece
returns brushes in the form generate_arena.map_text writes: a list of faces,
each (p0, p1, p2, texture) with integer points, the face's outward normal
cross(p0 - p1, p2 - p1), as qbsp reads it.

Pieces (each a list of brushes unless it says otherwise):

    hull(points, tex)            the convex hull of integer points: one brush
    box(lo, hi, tex)             an axis-aligned box; tex a name or
                                 {"top", "bottom", "side"} names
    prism(poly, z0, z1, tex)     a convex polygon (x, y) extruded up
    column(cx, cy, r, z0, z1)    an octagonal shaft with a base and a cap
    arch(...)                    a chamfered (octagonal) or pointed arch
                                 frame over an opening, with its jambs
    stairs(...), ramp(...)       a stair run (16 x 32 steps by default) and
                                 a 1:2 or gentler ramp
    beam(a, b, w, h, tex)        a straight beam between two points
    buttress(...)                an angled buttress against a wall
    rocks(seed, ...)             a seeded cluster of irregular convex rocks
    trim(...), frame(...)        a strip along a wall; a window frame
    alcove_torch(...)            a torch alcove: niche, sill and its torch
                                 entities (light_torch_small_walltorch,
                                 with the warm two-light fire of the arenas)

Space carves the playable space out of solid: the map starts solid
everywhere inside its bounds, each carve() opens a box, and emit() turns the
solid left over into merged axis-aligned brushes, so the outside is always
sealed and floors, walls and ceilings fall out of the heights of the spaces.

Python 3 standard library only. Output is deterministic.
"""

from __future__ import annotations

import math
import random
from math import gcd

# --- brushes -----------------------------------------------------------------


class KitError(Exception):
    pass


def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _tex_for(tex, normal):
    if isinstance(tex, dict):
        if normal[2] > 0 and abs(normal[2]) * 2 > abs(normal[0]) + abs(normal[1]):
            return tex.get("top", tex["side"])
        if normal[2] < 0 and abs(normal[2]) * 2 > abs(normal[0]) + abs(normal[1]):
            return tex.get("bottom", tex["side"])
        return tex["side"]
    return tex


def hull(points, tex):
    """The convex hull of integer points as one brush. tex: a name, or a
    {"top", "bottom", "side"} dict chosen by each face's normal."""
    pts = sorted({tuple(int(round(v)) for v in p) for p in points})
    if len(pts) < 4:
        raise KitError(f"a hull needs 4 points, not {len(pts)}")
    faces, seen = [], set()
    n = len(pts)
    for i in range(n):
        for j in range(i + 1, n):
            for k in range(j + 1, n):
                a, b, c = pts[i], pts[j], pts[k]
                nrm = _cross(_sub(b, a), _sub(c, a))
                if nrm == (0, 0, 0):
                    continue
                d = _dot(nrm, a)
                side = [_dot(nrm, p) - d for p in pts]
                if all(s <= 0 for s in side):
                    p0, p1, p2 = b, a, c          # cross(p0 - p1, p2 - p1) = nrm
                elif all(s >= 0 for s in side):
                    nrm, d = (-nrm[0], -nrm[1], -nrm[2]), -d
                    p0, p1, p2 = c, a, b
                else:
                    continue
                g = gcd(gcd(abs(nrm[0]), abs(nrm[1])), gcd(abs(nrm[2]), abs(d))) or 1
                key = (nrm[0] // g, nrm[1] // g, nrm[2] // g, d // g)
                if key in seen:
                    continue
                seen.add(key)
                faces.append((p0, p1, p2, _tex_for(tex, nrm)))
    if len(faces) < 4:
        raise KitError(f"flat hull: {pts}")
    return faces


def box(lo, hi, tex):
    """An axis-aligned box brush (lo, hi: (x, y, z))."""
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    if not (x0 < x1 and y0 < y1 and z0 < z1):
        raise KitError(f"empty box {lo} {hi}")
    return hull([(x, y, z) for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)], tex)


def prism(poly, z0, z1, tex):
    """A convex polygon [(x, y), ...] extruded from z0 to z1."""
    return hull([(x, y, z) for x, y in poly for z in (z0, z1)], tex)


def octagon(cx, cy, r, chamfer=None):
    """An axis-aligned octagon round (cx, cy), half-width r, corners cut by
    chamfer (default r * 0.4, rounded to the 4-unit grid)."""
    c = chamfer if chamfer is not None else max(4, int(round(r * 0.4 / 4)) * 4)
    return [(cx - r + c, cy - r), (cx + r - c, cy - r), (cx + r, cy - r + c), (cx + r, cy + r - c),
            (cx + r - c, cy + r), (cx - r + c, cy + r), (cx - r, cy + r - c), (cx - r, cy - r + c)]


# --- pieces ------------------------------------------------------------------


def column(cx, cy, r, z0, z1, tex, cap_tex=None):
    """An octagonal column: a square base and cap 8 wider all round, 16 high."""
    ct = cap_tex or tex
    return [box((cx - r - 8, cy - r - 8, z0), (cx + r + 8, cy + r + 8, z0 + 16), ct),
            prism(octagon(cx, cy, r), z0 + 16, z1 - 16, tex),
            box((cx - r - 8, cy - r - 8, z1 - 16), (cx + r + 8, cy + r + 8, z1), ct)]


def colonnade(a, b, axis, at, r, z0, z1, spacing, tex, cap_tex=None):
    """Columns along axis ('x' or 'y') from a to b at the other coordinate
    `at`, about spacing apart, the first and last on a and b."""
    n = max(1, int(round((b - a) / spacing)))
    out = []
    for i in range(n + 1):
        p = a + (b - a) * i // n
        cx, cy = (p, at) if axis == "x" else (at, p)
        out.extend(column(cx, cy, r, z0, z1, tex, cap_tex))
    return out


def arch(axis, at, lo, hi, z0, spring, top, depth, tex, kind="octagonal", jamb=32, z1=None):
    """An arch frame across an opening [lo, hi] along axis ('x': the opening
    runs along x, the frame's depth along y, centred on y = at; 'y' the
    other way). Jambs jamb wide outside the opening from z0; the soffit
    springs at `spring` and closes at `top`: "octagonal" cuts both upper
    corners at 45 degrees, "pointed" rises in two straight halves to a point
    at the top. Above `top` a lintel block runs to z1 (default top + 32)."""
    z1 = z1 if z1 is not None else top + 32
    w = hi - lo
    d0, d1 = at - depth // 2, at + depth // 2

    def xy(u, v):          # along the opening u, across it v
        return (u, v) if axis == "x" else (v, u)

    def blk(u0, u1, za, zb):
        (xa, ya), (xb, yb) = xy(u0, d0), xy(u1, d1)
        return box((min(xa, xb), min(ya, yb), za), (max(xa, xb), max(ya, yb), zb), tex)

    def wedge(poly_uz):    # a convex (u, z) polygon, extruded across the frame
        pts = []
        for u, z in poly_uz:
            for v in (d0, d1):
                x, y = xy(u, v)
                pts.append((x, y, z))
        return hull(pts, tex)

    out = [blk(lo - jamb, lo, z0, z1), blk(hi, hi + jamb, z0, z1), blk(lo, hi, top, z1)]
    rise = top - spring
    if rise <= 0:
        return out
    if kind == "pointed":
        mid = (lo + hi) // 2
        out.append(wedge([(lo, spring), (lo, top), (mid, top)]))
        out.append(wedge([(hi, spring), (hi, top), (mid, top)]))
    else:
        c = min(rise, w // 3)
        out.append(wedge([(lo, top - c), (lo, top), (lo + c, top)]))
        out.append(wedge([(hi, top - c), (hi, top), (hi - c, top)]))
    return out


def stairs(x0, y0, x1, y1, z0, z1, rise, tex, step=16):
    """A stair run over the footprint, from z0 at one end to z1 at the
    other, rising toward rise ('+x', '-x', '+y', '-y'), each step `step`
    high (16, or 8 for dense stairs) and the run shared out evenly. Each
    step is a solid block from z0 to its tread."""
    n = (z1 - z0) // step
    if n < 1 or n * step != z1 - z0:
        raise KitError(f"stairs {z0}..{z1} are not whole {step} steps")
    along_x = rise[1] == "x"
    a0, a1 = (x0, x1) if along_x else (y0, y1)
    out = []
    for i in range(n):
        lo = a0 + (a1 - a0) * i // n
        hi = a0 + (a1 - a0) * (i + 1) // n
        if rise[0] == "-":
            k = n - 1 - i
        else:
            k = i
        zt = z0 + step * (k + 1)
        if along_x:
            out.append(box((lo, y0, z0 - 16), (hi, y1, zt), tex))
        else:
            out.append(box((x0, lo, z0 - 16), (x1, hi, zt), tex))
    return out


def ramp(x0, y0, x1, y1, z0, z1, rise, tex):
    """A solid ramp wedge over the footprint, z0 at its low end to z1 at the
    high end (rise '+x', '-x', '+y', '-y'), on a base 16 below z0. Refuses
    anything steeper than 1:2."""
    length = (x1 - x0) if rise[1] == "x" else (y1 - y0)
    if (z1 - z0) * 2 > length:
        raise KitError(f"ramp steeper than 1:2 ({z1 - z0} over {length})")
    zb = z0 - 16
    if rise == "+x":
        pts = [(x0, y, z) for y in (y0, y1) for z in (zb, z0)] + \
              [(x1, y, z) for y in (y0, y1) for z in (zb, z1)]
    elif rise == "-x":
        pts = [(x1, y, z) for y in (y0, y1) for z in (zb, z0)] + \
              [(x0, y, z) for y in (y0, y1) for z in (zb, z1)]
    elif rise == "+y":
        pts = [(x, y0, z) for x in (x0, x1) for z in (zb, z0)] + \
              [(x, y1, z) for x in (x0, x1) for z in (zb, z1)]
    else:
        pts = [(x, y1, z) for x in (x0, x1) for z in (zb, z0)] + \
              [(x, y0, z) for x in (x0, x1) for z in (zb, z1)]
    return [hull(pts, tex)]


def beam(a, b, w, h, tex):
    """A straight beam of width w and height h from point a to point b
    (any angle in plan, any slope): one brush."""
    dx, dy = b[0] - a[0], b[1] - a[1]
    L = math.hypot(dx, dy) or 1.0
    px, py = -dy / L * w / 2, dx / L * w / 2
    pts = []
    for (x, y, z) in (a, b):
        for s in (-1, 1):
            for zz in (z - h / 2, z + h / 2):
                pts.append((x + s * px, y + s * py, zz))
    return [hull(pts, tex)]


def buttress(x, y, face, width, z0, z1, depth, tex):
    """An angled buttress against a wall facing `face` ('+x', '-x', '+y',
    '-y': the way out of the wall), its foot depth out from the wall at z0,
    its top meeting the wall at z1."""
    hw = width // 2
    sgn = 1 if face[0] == "+" else -1
    if face[1] == "x":
        pts = [(x, y + s, z) for s in (-hw, hw) for z in (z0, z1)] + \
              [(x + sgn * depth, y + s, z0) for s in (-hw, hw)] + \
              [(x + sgn * depth, y + s, z0 + 16) for s in (-hw, hw)]
    else:
        pts = [(x + s, y, z) for s in (-hw, hw) for z in (z0, z1)] + \
              [(x + s, y + sgn * depth, z0) for s in (-hw, hw)] + \
              [(x + s, y + sgn * depth, z0 + 16) for s in (-hw, hw)]
    return [hull(pts, tex)]


def rocks(seed, cx, cy, z0, radius, count, size, tex):
    """A seeded cluster of count irregular convex rocks round (cx, cy) on
    z0, each about size across: the same seed gives the same rocks."""
    rng = random.Random(seed)
    out = []
    for _ in range(count):
        a = rng.uniform(0, 2 * math.pi)
        d = rng.uniform(0, radius)
        rx, ry = cx + math.cos(a) * d, cy + math.sin(a) * d
        s = size * rng.uniform(0.6, 1.2)
        pts = []
        for k in range(10):
            t = rng.uniform(0, 2 * math.pi)
            u = rng.uniform(-0.2, 1.0)
            r = s / 2 * rng.uniform(0.6, 1.0) * math.sqrt(max(0.05, 1 - u * u))
            pts.append((rx + math.cos(t) * r, ry + math.sin(t) * r, z0 - 8 + (u + 0.2) * s * 0.8))
        try:
            out.append(hull(pts, tex))
        except KitError:
            continue
    return out


def trim(axis, a, b, at, face, z0, z1, depth, tex):
    """A strip from a to b along axis on the wall plane `at`, standing out
    depth toward `face` ('+' or '-'), from z0 to z1."""
    o0, o1 = (at, at + depth) if face == "+" else (at - depth, at)
    if axis == "x":
        return [box((a, o0, z0), (b, o1, z1), tex)]
    return [box((o0, a, z0), (o1, b, z1), tex)]


def frame(axis, lo, hi, at, z0, z1, depth, border, tex, glass=None):
    """A window frame on the wall plane `at` (axis: the wall runs along it):
    four border bars round the opening [lo, hi] x [z0, z1], depth deep; with
    glass, a pane of that texture 4 thick in the middle."""
    d0, d1 = at - depth // 2, at + depth // 2

    def b(u0, u1, za, zb, t=tex, dd=(d0, d1)):
        if axis == "x":
            return box((u0, dd[0], za), (u1, dd[1], zb), t)
        return box((dd[0], u0, za), (dd[1], u1, zb), t)

    out = [b(lo - border, lo, z0 - border, z1 + border), b(hi, hi + border, z0 - border, z1 + border),
           b(lo, hi, z0 - border, z0), b(lo, hi, z1, z1 + border)]
    if glass:
        out.append(b(lo, hi, z0, z1, glass, (at - 2, at + 2)))
    return out


# The arenas' warm fire (generate_arena.py LIGHT_* and _Builder._fire): a
# yellow core on the torch itself and a red-orange glow at the same spot.
TORCH = ("light_torch_small_walltorch", 340, 2.6, 170)
FLAME = ("light_flame_large_yellow", 460, 2.2, 230)
GLOW_WAIT = 0.55
FIRE_COLOR = "1 0.78 0.4"
GLOW_COLOR = "1 0.42 0.12"


def fire_ents(x, y, z, kind=TORCH, k=1.0):
    cls, core, wait, glow = kind
    return [({"classname": cls, "origin": f"{x} {y} {z}", "light": str(round(core * k)),
              "wait": f"{wait:g}", "_color": FIRE_COLOR}, []),
            ({"classname": "light", "origin": f"{x} {y} {z}", "light": str(round(glow * k)),
              "wait": f"{GLOW_WAIT:g}", "_color": GLOW_COLOR}, [])]


def alcove_torch(axis, u, at, face, z, tex, width=64, depth=16, height=96):
    """A torch alcove on a wall plane `at` (the wall runs along axis, the
    room is toward face '+' or '-'): jambs, a sill and a lintel framing the
    torch, centred on u, from z; returns (brushes, entities): the torch 16
    off the wall, its two lights. The frame is thin, so it shadows little of
    the wall round it."""
    s = 1 if face == "+" else -1
    hw = width // 2
    # The frame stands 2 off the wall, so the wall's face behind the torch
    # stays whole (no brush touches it): the torch lights it and its
    # falloff reads on it (validate_bsp firelight).
    o0, o1 = sorted((at + s * 2, at + s * 6))
    if axis == "x":
        br = [box((u - hw - 8, o0, z - 8), (u + hw + 8, o1, z), tex),
              box((u - hw - 8, o0, z + height), (u + hw + 8, o1, z + height + 8), tex),
              box((u - hw - 8, o0, z), (u - hw, o1, z + height), tex),
              box((u + hw, o0, z), (u + hw + 8, o1, z + height), tex)]
        tx, ty = u, at + s * 16
    else:
        br = [box((o0, u - hw - 8, z - 8), (o1, u + hw + 8, z), tex),
              box((o0, u - hw - 8, z + height), (o1, u + hw + 8, z + height + 8), tex),
              box((o0, u - hw - 8, z), (o1, u - hw, z + height), tex),
              box((o0, u + hw, z), (o1, u + hw + 8, z + height), tex)]
        tx, ty = at + s * 16, u
    return br, fire_ents(tx, ty, z + height // 2 + 8)


# --- carving ---------------------------------------------------------------


class Space:
    """Solid inside bounds (x0, y0, z0, x1, y1, z1), on a cell-sized plan
    grid, until carve() opens boxes of it. Each carve names its textures:
    the floor under it, the ceiling over it and the walls round it, and
    "sky" True makes the solid over it sky (a skylight)."""

    def __init__(self, bounds, cell=16):
        self.x0, self.y0, self.z0, self.x1, self.y1, self.z1 = bounds
        self.c = cell
        for v in (self.x0, self.y0, self.x1, self.y1):
            if v % cell:
                raise KitError(f"bounds {bounds} off the {cell} grid")
        self.nx = (self.x1 - self.x0) // cell
        self.ny = (self.y1 - self.y0) // cell
        self.cols = {}     # (i, j) -> [(z0, z1, style)], open spans

    def carve(self, x0, y0, z0, x1, y1, z1, style):
        c = self.c
        if (x0 - self.x0) % c or (x1 - self.x0) % c or (y0 - self.y0) % c or (y1 - self.y0) % c:
            raise KitError(f"carve {(x0, y0, x1, y1)} off the {c} grid")
        if not (self.x0 < x0 < x1 < self.x1 and self.y0 < y0 < y1 < self.y1
                and self.z0 < z0 < z1 < self.z1):
            raise KitError(f"carve {(x0, y0, z0, x1, y1, z1)} not inside {self.bounds()}")
        for i in range((x0 - self.x0) // c, (x1 - self.x0) // c):
            for j in range((y0 - self.y0) // c, (y1 - self.y0) // c):
                self.cols.setdefault((i, j), []).append((z0, z1, style))

    def bounds(self):
        return (self.x0, self.y0, self.z0, self.x1, self.y1, self.z1)

    def open_at(self, x, y, z):
        """True if (x, y, z) is in carved space."""
        i, j = int((x - self.x0) // self.c), int((y - self.y0) // self.c)
        return any(a <= z < b for a, b, _ in self.cols.get((i, j), ()))

    def _solid_spans(self, spans):
        """The solid spans of one column: (z0, z1, style under, style over)."""
        ss = sorted(spans, key=lambda s: (s[0], -s[1]))
        merged = []
        for a, b, st in ss:
            if merged and a <= merged[-1][1]:
                pa, pb, pst, top = merged[-1]
                if b > pb:
                    merged[-1] = (pa, b, pst, st)
            else:
                merged.append((a, b, st, st))
        out = []
        z, below = self.z0, None
        for a, b, st_bottom, st_top in merged:
            if a > z:
                out.append((z, a, below, st_bottom))
            z, below = b, st_top
        if z < self.z1:
            out.append((z, self.z1, below, None))
        return out

    def _open_spans(self, i, j):
        """The open spans of column (i, j), merged: [(z0, z1, style)]."""
        out = []
        for a, b, st in sorted(self.cols.get((i, j), ()), key=lambda s: (s[0], -s[1])):
            if out and a <= out[-1][1]:
                if b > out[-1][1]:
                    out[-1] = (out[-1][0], b, out[-1][2])
            else:
                out.append((a, b, st))
        return out

    def _beside(self, i, j, a, b):
        """The open span beside solid (i, j) from a to b that it overlaps
        most: (z0, z1, style), or None (solid all round)."""
        best, most = None, 0
        for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            for z0, z1, st in self._open_spans(i + di, j + dj):
                o = min(b, z1) - max(a, z0)
                if o > most:
                    best, most = (z0, z1, st), o
        return best

    def emit(self, styles, default):
        """The solid as merged boxes. styles: {name: {"floor", "ceil",
        "wall"[, "sky", "sky_tex", "base", "frieze"]}}, the names
        carve() was given; default: the style name for solid with no carve
        beside it. A span's top takes the floor of the space over it, its
        bottom the ceiling of the space under it, and its sides the walls of
        the open space beside it; with "base": (texture, height) the wall's
        foot is banded in it from that space's floor, and with "frieze" its
        top below that space's ceiling. A span over a sky space is sky all
        round."""
        c = self.c
        keyed = {}
        for i in range(self.nx):
            for j in range(self.ny):
                spans = self.cols.get((i, j))
                solids = [(self.z0, self.z1, None, None)] if not spans else self._solid_spans(spans)
                for a, b, under, over in solids:
                    bes = self._beside(i, j, a, b)
                    side = bes[2] if bes else None
                    cuts = [a, b]
                    if bes and side in styles:
                        st = styles[side]
                        if "base" in st:
                            cuts.append(bes[0] + st["base"][1])
                        if "frieze" in st:
                            cuts.append(bes[1] - st["frieze"][1])
                    cuts = sorted({z for z in cuts if a <= z <= b})
                    for za, zb in zip(cuts, cuts[1:]):
                        band = None
                        if bes and side in styles:
                            st = styles[side]
                            if "base" in st and zb <= bes[0] + st["base"][1] and za >= bes[0] - 64:
                                band = "base"
                            elif "frieze" in st and za >= bes[1] - st["frieze"][1] and zb <= bes[1] + 64:
                                band = "frieze"
                        keyed.setdefault((za, zb, under if za == a else None,
                                          over if zb == b else None, side, band,
                                          za == a, zb == b), set()).add((i, j))
        out = []
        for key, cells in sorted(keyed.items(), key=lambda kv: tuple(str(k) for k in kv[0])):
            a, b, under, over, side, band, is_bottom, is_top = key
            under, over, dflt = styles.get(under), styles.get(over), styles[default]
            sst = styles.get(side) or over or under or dflt
            if under and under.get("sky") and is_bottom:
                tex = under["sky_tex"]
            else:
                wall = sst[band][0] if band else sst["wall"]
                tex = {"side": wall,
                       "top": (over or dflt)["floor"],
                       "bottom": (under or dflt)["ceil"]}
            for (i0, j0, i1, j1) in _rects(cells):
                out.append(box((self.x0 + i0 * c, self.y0 + j0 * c, a),
                               (self.x0 + i1 * c, self.y0 + j1 * c, b), tex))
        return out


def _rects(cells):
    """Greedy cover of a set of (i, j) cells by rectangles (i0, j0, i1, j1),
    exclusive ends: rows first, then grown along j."""
    left = set(cells)
    out = []
    for (i, j) in sorted(cells, key=lambda p: (p[1], p[0])):
        if (i, j) not in left:
            continue
        i1 = i
        while (i1 + 1, j) in left:
            i1 += 1
        j1 = j
        while all((k, j1 + 1) in left for k in range(i, i1 + 1)):
            j1 += 1
        for k in range(i, i1 + 1):
            for m in range(j, j1 + 1):
                left.discard((k, m))
        out.append((i, j, i1 + 1, j1 + 1))
    return out
