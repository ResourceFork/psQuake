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


def arch(axis, at, lo, hi, z0, spring, top, depth, tex, kind="octagonal", jamb=32, z1=None,
         round_jambs=False):
    """An arch frame across an opening [lo, hi] along axis ('x': the opening
    runs along x, the frame's depth along y, centred on y = at; 'y' the
    other way). Jambs jamb wide outside the opening from z0; the soffit
    springs at `spring` and closes at `top`: "octagonal" cuts both upper
    corners at 45 degrees, "pointed" rises in two straight halves to a point
    at the top. Above `top` a lintel block runs to z1 (default top + 32).
    round_jambs: each jamb an octagonal pier up to top, under a lintel the
    frame's whole width (a free-standing pier is never square)."""
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

    if round_jambs:
        r = min(jamb, depth) // 2
        out = [blk(lo - jamb, hi + jamb, top, z1)]
        for u in (lo - jamb // 2, hi + jamb // 2):
            x, y = xy(u, at)
            out.append(prism(octagon(x, y, r), z0, top, tex))
    else:
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


def stairs(x0, y0, x1, y1, z0, z1, rise, tex, step=16, under=None):
    """A stair run over the footprint, from z0 at one end to z1 at the
    other, rising toward rise ('+x', '-x', '+y', '-y'), each step `step`
    high (16, or 8 for dense stairs) and the run shared out evenly. Each
    step is a solid block from z0 to its tread. under (a0, a1, zb): the
    steps wholly between a0 and a1 along the rise stand on zb instead, a
    tunnel under them; each must be at least 16 thick there."""
    n = (z1 - z0) // step
    if n < 1 or n * step != z1 - z0:
        raise KitError(f"stairs {z0}..{z1} are not whole {step} steps")
    if under is not None and under[2] - z0 < 64:
        raise KitError(f"a tunnel {under[2] - z0} high under stairs: the player needs 56, "
                       "and 8 over")
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
        zb = z0 - 16
        if under is not None and under[0] <= lo and hi <= under[1]:
            zb = under[2]
            if zt - zb < 16:
                raise KitError(f"stairs over a tunnel: a step {zt - zb} thick at {lo}")
        elif under is not None and lo < under[1] and under[0] < hi:
            raise KitError(f"stairs over a tunnel: the step {lo}..{hi} is half over "
                           f"{under[0]}..{under[1]}")
        if along_x:
            out.append(box((lo, y0, zb), (hi, y1, zt), tex))
        else:
            out.append(box((x0, lo, zb), (x1, hi, zt), tex))
    return out


def shaft(cx, cy, r, z0, z1, tex):
    """An octagonal shaft round (cx, cy), half-width r: a broken pillar, a
    trunk, a post."""
    return [prism(octagon(cx, cy, r), z0, z1, tex)]


def rail(axis, a0, a1, at, face, z, tex, top=32, thick=6, bar=10, every=64):
    """A thin railing along a perch's edge: on the plane `at` (the edge),
    running along axis from a0 to a1, toward face ('+' or '-', the perch's
    side) thick deep; posts every `every` from z to z + top and a bar from
    z + top - bar, so the player looks over it (top under the eye, 46) and
    under its bar, and cannot walk off (the bar is over a step, 18)."""
    if top - bar <= 18 or top >= 46:
        raise KitError(f"a rail {top} high with a bar from {top - bar}: over a step "
                       "and under the eye")
    s = 1 if face == "+" else -1
    o0, o1 = sorted((at, at + s * thick))

    def b(u0, u1, za, zb):
        if axis == "x":
            return box((u0, o0, za), (u1, o1, zb), tex)
        return box((o0, u0, za), (o1, u1, zb), tex)
    n = max(1, round((a1 - a0) / every))
    out = [b(a0, a1, z + top - bar, z + top)]
    for i in range(n + 1):
        u = a0 + (a1 - a0) * i // n
        u0 = min(max(u - thick // 2, a0), a1 - thick)
        out.append(b(u0, u0 + thick, z, z + top - bar))
    return out


def lava_chute(x, y, face, z, surface, depth, slope, sections, tex):
    """A lava channel's pour down a chute cut in a pit's funnel wall, from
    the channel's mouth at (x, y) on the pit's edge, falling toward face
    ('+y', '-y', '+x', '-x'): lava surface under the wall's slope (which
    falls slope for each 1 in from z) and depth deep, on the chute's bed,
    in tapered trapezoid sections between sections [(in, half-width), ...]:
    wide at the mouth, narrowing, widening again as it lands (molten metal
    poured). (brushes, entities): a yellow-white light over its mouth and an
    orange one where it lands."""
    dx, dy = {"+x": (1, 0), "-x": (-1, 0), "+y": (0, 1), "-y": (0, -1)}[face]

    def edge(d, hw):
        cx, cy = x + d * dx, y + d * dy
        top, bed = z - surface - slope * d, z - depth - slope * d
        return [(cx + u * dy, cy + u * dx, zz) for u in (-hw, hw) for zz in (top, bed)]
    out = [hull(edge(*a) + edge(*b), tex) for a, b in zip(sections, sections[1:])]
    dl = sections[-1][0]
    ents = [({"classname": "light", "origin": f"{x + 24 * dx} {y + 24 * dy} {z + 8}",
              "light": "220", "_color": "1 0.9 0.55", "wait": "1.4"}, []),
            ({"classname": "light", "origin": f"{x + (dl + 16) * dx} {y + (dl + 16) * dy} "
              f"{z - slope * dl + 16}", "light": "300", "_color": "1 0.45 0.1", "wait": "1.2"}, [])]
    return out, ents


def lava_pour(x, y, face, z, zb, tex, lip_tex=None, feed=None, slope=None):
    """A lava channel's pour into a pit at (x, y) on the pit's edge, falling
    toward face ('+y', '-y', '+x', '-x') over the funnel: a thick, rounded
    stream down to zb, swelling as it falls (molten metal: viscous).
    lip_tex: a stone lip under its start (a spout); feed (half-width, top):
    instead, the channel's lava runs on over the edge into the stream, one
    flow. slope: the funnel wall's fall for each 1 in (from z at the edge):
    the stream then lies on the wall, each ring resting on it, instead of
    standing off it. (brushes, entities): its warm lights, a yellow-white
    one at its start and an orange one where it lands."""
    dx, dy = {"+x": (1, 0), "-x": (-1, 0), "+y": (0, 1), "-y": (0, -1)}[face]
    lip = []
    if lip_tex is not None:
        a, b = ((x - 12) if dy else x, (y - 12) if dx else y)
        c, d = ((x + 12) if dy else x + 20 * dx, (y + 12) if dx else y + 20 * dy)
        lip.append(box((min(a, c), min(b, d), z - 12), (max(a, c), max(b, d), z), lip_tex))
    # the stream: off the lip's end and down over the funnel's slope (which
    # falls 2 for each 1 in), swelling as it goes: three rounded rings
    def ring(d, zz, r):
        return [(px, py, zz) for px, py in octagon(x + d * dx, y + d * dy, r, max(2, r * 2 // 5))]
    # the three rings (in, z, radius): the same stream either way, standing
    # off the wall (20 over a slope of 2) or, with slope, resting on it
    if slope is None:
        mid = (z + zb) // 2
        rings = [(14, z - 8, 6), (14 + (z - 8 - mid) // 2, mid, 8), (14 + (z - 8 - zb) // 2, zb, 10)]
    else:
        # each ring's centre one radius over the wall at its own distance in;
        # the last where that meets the pit's lava (zb)
        d2 = round((z + 10 - zb) / slope)
        dm = (14 + d2) // 2
        rings = [(14, z - slope * 14 + 6, 6), (dm, z - slope * dm + 8, 8), (d2, zb, 10)]
    if feed is not None:
        # the channel's lava, its full width at the edge, turning down onto
        # the wall into the stream's first ring
        hw, top = feed
        edge = [(x + u * dy, y + u * dx) for u in (-hw, hw)]
        lip.append(hull([(px, py, zz) for px, py in edge for zz in (top, top - 12)]
                        + ring(*rings[0]), tex))
    stream = [hull(ring(*rings[0]) + ring(*rings[1]), tex),
              hull(ring(*rings[1]) + ring(*rings[2]), tex)]
    bot_c = (x + rings[2][0] * dx, y + rings[2][0] * dy)
    ents = [({"classname": "light", "origin": f"{x + 28 * dx} {y + 28 * dy} {z + 8}",
              "light": "220", "_color": "1 0.9 0.55", "wait": "1.4"}, []),
            ({"classname": "light", "origin": f"{bot_c[0] + 8 * dx} {bot_c[1] + 8 * dy} {zb + 24}",
              "light": "300", "_color": "1 0.45 0.1", "wait": "1.2"}, [])]
    return lip + stream, ents



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


SMALL_FLAME = ("light_flame_small_yellow", 300, 2.4, 160)


def _on_wall(axis, u, at, face):
    """A mapper (along, out, z) -> (x, y, z) for a wall plane: along the wall
    from u, out from it into the room toward face ('+' or '-')."""
    s = 1 if face == "+" else -1
    if axis == "x":   # the wall runs along x at y = at
        return lambda a, o, z: (u + a, at + s * o, z)
    return lambda a, o, z: (at + s * o, u + a, z)


def sconce_bracket(axis, u, at, face, z, tex):
    """A traditional torch on a bracket: a plate flush on the wall, an arm
    angled out and up, and a cup holding the torch. (brushes, entities)"""
    w = _on_wall(axis, u, at, face)
    plate = hull([w(a, o, zz) for a in (-8, 8) for o in (0, 4) for zz in (z, z + 40)], tex)
    arm = hull([w(a, o, zz) for a in (-3, 3) for o, zz in ((4, z + 10), (4, z + 18),
                                                        (14, z + 26), (14, z + 32))], tex)
    cup = hull([w(a, o, zz) for a in (-6, 6) for o in (8, 20) for zz in (z + 30, z + 36)]
               + [w(a, o, z + 26) for a in (-3, 3) for o in (11, 17)], tex)
    # the torch stands in the cup, nearer the wall than the cup's rim is
    # under it, so its light reads on the wall (validate_bsp firelight)
    x, y, zz = w(0, 14, z + 52)
    return [plate, arm, cup], fire_ents(x, y, zz, TORCH, 1.4)


def sconce_cup(axis, u, at, face, z, tex):
    """A cup on the wall: a bowl, narrow at its foot and wide at its lip,
    flush on the wall, a flame in it. (brushes, entities)"""
    w = _on_wall(axis, u, at, face)
    bowl = hull([w(a, 0, z) for a in (-8, 8)] + [w(a, 10, z) for a in (-6, 6)]
                + [w(a, 0, z + 20) for a in (-16, 16)] + [w(a, 22, z + 20) for a in (-12, 12)]
                + [w(a, 12, z - 6) for a in (-4, 4)], tex)
    x, y, zz = w(0, 11, z + 30)
    return [bowl], fire_ents(x, y, zz, SMALL_FLAME)


def sconce_trapezoid(axis, u, at, face, z, tex):
    """An inverted-trapezoid brazier on the wall: 16 wide at its foot, 48 at
    its rim, flush on the wall, a flame on top. (brushes, entities)"""
    w = _on_wall(axis, u, at, face)
    body = hull([w(a, o, z) for a in (-8, 8) for o in (0, 12)]
                + [w(a, o, z + 28) for a in (-24, 24) for o in (0, 24)], tex)
    rim = hull([w(a, o, zz) for a in (-26, 26) for o in (0, 26) for zz in (z + 28, z + 32)], tex)
    x, y, zz = w(0, 13, z + 42)
    return [body, rim], fire_ents(x, y, zz, SMALL_FLAME)


SCONCES = {"bracket": sconce_bracket, "cup": sconce_cup, "trapezoid": sconce_trapezoid}


def _ray_out(cx, cy, t, box):
    """How far the ray from (cx, cy) at angle t (radians) runs to leave box
    (x0, y0, x1, y1), which holds (cx, cy)."""
    x0, y0, x1, y1 = box
    c, s = math.cos(t), math.sin(t)
    ds = [(x1 - cx) / c if c > 1e-9 else (x0 - cx) / c if c < -1e-9 else math.inf,
          (y1 - cy) / s if s > 1e-9 else (y0 - cy) / s if s < -1e-9 else math.inf]
    return min(ds)


def spiral_stairs(cx, cy, r0, r1, a0, a1, z0, z1, steps, tex, fill=None):
    """Stairs that sweep a smooth arc round (cx, cy) between radii r0 and r1,
    from angle a0 (degrees: the foot, at z0) to a1 (the top, z1), in steps
    wedge-shaped steps, each a solid block from 16 under z0 to its tread.
    fill (x0, y0, x1, y1), a box holding (cx, cy) and the sweep: each step
    runs on out past r1 to the box's edge, so nothing is left to stand in
    between the stair and the walls the box meets."""
    if fill is not None:
        x0, y0, x1, y1 = fill
        if not (x0 <= cx <= x1 and y0 <= cy <= y1):
            raise KitError(f"spiral fill {fill} does not hold its centre")
    if (z1 - z0) % steps:
        raise KitError(f"spiral {z0}..{z1} does not split into {steps} steps")
    rise = (z1 - z0) // steps
    if rise > 18:
        raise KitError(f"spiral steps of {rise}, over the player's step")
    out = []
    for i in range(steps):
        t0 = math.radians(a0 + (a1 - a0) * i / steps)
        t1 = math.radians(a0 + (a1 - a0) * (i + 1) / steps)
        top = z0 + rise * (i + 1)
        if fill is None:
            ring = [(cx + r * math.cos(t), cy + r * math.sin(t))
                    for r in (r0, r1) for t in (t0, t1)]
        else:
            lo, hi = min(t0, t1), max(t0, t1)
            ring = [(cx + r0 * math.cos(t), cy + r0 * math.sin(t)) for t in (t0, t1)]
            for t in (t0, t1):
                d = _ray_out(cx, cy, t, fill)
                if d < r1:
                    raise KitError(f"spiral fill {fill} cuts the stair inside r1")
                ring.append((cx + d * math.cos(t), cy + d * math.sin(t)))
            # each edge's nearest point to the hub, if inside the span
            for d, tn in ((fill[2] - cx, 0.0), (cy - fill[1], -math.pi / 2),
                          (cx - fill[0], math.pi), (fill[3] - cy, math.pi / 2)):
                if any(lo < tn + k * 2 * math.pi < hi for k in (-1, 0, 1)) and d < r1:
                    raise KitError(f"spiral fill {fill} cuts the stair inside r1")
            for x, y in ((fill[0], fill[1]), (fill[2], fill[1]), (fill[0], fill[3]),
                         (fill[2], fill[3])):
                ta = math.atan2(y - cy, x - cx)
                for k in (-1, 0, 1):   # the corner's angle, wound into the step's span
                    if lo + 1e-9 < ta + k * 2 * math.pi < hi - 1e-9:
                        ring.append((x, y))
        pts = [(x, y, z) for x, y in ring for z in (z0 - 16, top)]
        out.append(hull(pts, tex))
    return out


def lean_wall(axis, a0, a1, at, face, z0, z1, tex, lean=10):
    """A cliff leaning back as it rises, lean degrees off vertical: a wedge
    filling the foot of a wall carved back by (z1 - z0) * tan(lean) from
    the plane at (the room toward face); its slope runs from at on the
    floor to the carved-back plane at z1."""
    s = 1 if face == "+" else -1
    d = lean_depth(z0, z1, lean)
    back = at - s * d
    if axis == "x":
        pts = [(a, y, z) for a in (a0, a1) for y, z in ((back, z0), (at, z0), (back, z1))]
    else:
        pts = [(x, a, z) for a in (a0, a1) for x, z in ((back, z0), (at, z0), (back, z1))]
    return [hull(pts, tex)]


def lean_depth(z0, z1, lean=10):
    """How far a leaning cliff's top stands back from its foot (16 grid)."""
    return int(math.ceil((z1 - z0) * math.tan(math.radians(lean)) / 16)) * 16


def strata(axis, a0, a1, at, face, z, seed, tex, depth=(10, 26), h=(12, 28)):
    """Rock strata along a wall plane: a band of irregular ledges sticking out
    of it at about z, seeded."""
    rng = random.Random(seed)
    s = 1 if face == "+" else -1
    out, a = [], a0
    while a < a1:
        b = min(a1, a + rng.choice((48, 64, 96)))
        d0, d1 = rng.randint(*depth), rng.randint(*depth)
        t = rng.randint(*h)
        zz = z + rng.randint(-8, 8)
        if axis == "x":
            pts = [(aa, at - s * 8, q) for aa in (a, b) for q in (zz, zz + t)] + \
                  [(a, at + s * d0, zz + t), (b, at + s * d1, zz + t), (a, at + s * d0 / 2, zz),
                   (b, at + s * d1 / 2, zz)]
        else:
            pts = [(at - s * 8, aa, q) for aa in (a, b) for q in (zz, zz + t)] + \
                  [(at + s * d0, a, zz + t), (at + s * d1, b, zz + t), (at + s * d0 / 2, a, zz),
                   (at + s * d1 / 2, b, zz)]
        try:
            out.append(hull(pts, tex))
        except KitError:
            pass
        a = b
    return out


def pit_funnel(x0, y0, x1, y1, ztop, zbot, inset, tex, chutes=()):
    """Steep slopes round a pit (x0..x1, y0..y1) from its rim at ztop down
    and in by inset to zbot, too steep to stand on (its normal's z under 0.65,
    where Quake stands from 0.7), so anything falling in slides on down.
    chutes: (side 'x0' 'x1' 'y0' 'y1', a, b, depth): that side's wall cut
    from a to b along it, depth under its slope, a lava pour's bed (the
    channel's groove carried on down the wall)."""
    if inset / math.hypot(inset, ztop - zbot) >= 0.65:
        raise KitError("a funnel slope must be too steep to stand on (normal z under 0.65; "
                       "Quake stands on 0.7 and more)")
    if chutes:
        for side, a, b, depth in chutes:
            if side not in ("x0", "x1", "y0", "y1"):
                raise KitError(f"a chute on no wall: {side!r}")
            lo_, hi_ = (x0, x1) if side[0] == "y" else (y0, y1)
            if not (lo_ + inset <= a < b <= hi_ - inset):
                raise KitError(f"a chute {a}..{b} on {side} is within {inset} of a corner, where "
                               "the next wall fills it")
            if not 0 < depth < ztop - zbot:
                raise KitError(f"a chute {depth} deep in a wall {ztop - zbot} high")
        for i, (s1, a1_, b1_, _) in enumerate(chutes):
            for s2, a2_, b2_, _ in chutes[i + 1:]:
                if s1 == s2 and a1_ < b2_ and a2_ < b1_:
                    raise KitError(f"chutes {a1_}..{b1_} and {a2_}..{b2_} overlap on {s1}")
        out = []
        fall = (ztop - zbot) / inset
        for side, x_lo, x_hi, y_lo, y_hi in (("x0", x0, x0, y0, y1), ("x1", x1, x1, y0, y1),
                                            ("y0", x0, x1, y0, y0), ("y1", x0, x1, y1, y1)):
            along_x = side[0] == "y"
            a0, a1 = (x_lo, x_hi) if along_x else (y_lo, y_hi)
            cuts = sorted((a, b, depth) for s, a, b, depth in chutes if s == side)
            spans, u = [], a0
            for a, b, depth in cuts:
                if not a0 < a < b < a1:
                    raise KitError(f"a chute {a}..{b} off the {side} wall {a0}..{a1}")
                spans += [(u, a, 0), (a, b, depth)]
                u = b
            spans.append((u, a1, 0))
            at = {"x0": x0, "x1": x1, "y0": y0, "y1": y1}[side]
            s = 1 if side[1] == "0" else -1      # inward
            for ua, ub, depth in spans:
                if ub <= ua:
                    continue
                reach = inset - depth / fall     # where the cut slope meets zbot
                prof = [(0, ztop - depth), (0, zbot), (reach, zbot)]
                pts = []
                for u in (ua, ub):
                    for d, zz in prof:
                        pts.append((u, at + s * d, zz) if along_x else (at + s * d, u, zz))
                out.append(hull(pts, tex))
        return out
    sides = [[(x0, y, ztop) for y in (y0, y1)] + [(x0, y, zbot) for y in (y0, y1)]
             + [(x0 + inset, y, zbot) for y in (y0, y1)],
             [(x1, y, ztop) for y in (y0, y1)] + [(x1, y, zbot) for y in (y0, y1)]
             + [(x1 - inset, y, zbot) for y in (y0, y1)],
             [(x, y0, ztop) for x in (x0, x1)] + [(x, y0, zbot) for x in (x0, x1)]
             + [(x, y0 + inset, zbot) for x in (x0, x1)],
             [(x, y1, ztop) for x in (x0, x1)] + [(x, y1, zbot) for x in (x0, x1)]
             + [(x, y1 - inset, zbot) for x in (x0, x1)]]
    return [hull(p, tex) for p in sides]


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
