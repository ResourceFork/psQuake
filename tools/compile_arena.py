#!/usr/bin/env python3
"""Generate and compile the psQuake arena: generate_arena.py, qbsp, vis, light.

    compile_arena.py --tools data/ericw-tools/bin --pak data/librequake/id1/pak0.pak \\
        --out build/arena/psqarena [--arena NAME] [--break KIND] [--keep-going]

Writes <out>/psqarena.map, psqarena.wad, psqarena.bsp and its coloured
lightmap psqarena.lit (whichever map --arena names: generate_arena.py
ARENAS, or one of OTHER_GENERATORS; the test hall psqtesthall without it), plus psqarena.plan.svg and
psqarena.shots, and each compiler's
output in <out>/<tool>.out. A previous psqarena.bsp, its .lit and leak files in <out> are
removed first, so nothing stale survives a failed step. --keep-going runs
every step even after one fails (the validator's tests compile broken maps
that leak, so vis cannot run). Exit status is 1 when any step failed.

The compilers are ericw-tools (tools/setup-build.sh builds them). They run
with an argument list, never through a shell.

Python 3 standard library only.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Maps another generator writes (the rest: generate_arena.py --arena).
OTHER_GENERATORS = {"psqtesthazards": "generate_testhazards.py",
                    "psqtemple": "generate_map.py", "psqruins": "generate_map.py"}
STEMS = ("psqarena.bsp", "psqarena.pts", "psqarena.lin", "psqarena.prt",
         "psqarena.log", "psqarena.content.json", "psqarena.texinfo.json",
         "psqarena.bsp.tmp", "psqarena.lit", "psqarena.plan.svg", "psqarena.shots")


def run(step, cmd, out_dir):
    log = os.path.join(out_dir, f"{step}.out")
    with open(log, "wb") as f:
        rc = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, cwd=out_dir).returncode
    print(f"compile_arena: {step} exit {rc} ({log})")
    if rc != 0:
        with open(log, "rb") as f:
            tail = f.read()[-2000:].decode("utf-8", "replace")
        print(tail, file=sys.stderr)
    return rc == 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--tools", required=True, help="folder with qbsp, vis and light")
    ap.add_argument("--pak", required=True, help="LibreQuake id1/pak0.pak")
    ap.add_argument("--out", required=True, help="output folder")
    ap.add_argument("--break", dest="break_kind")
    ap.add_argument("--arena", default="psqtesthall",
                    help="generate_arena.py --arena (default the test hall)")
    ap.add_argument("--keep-going", action="store_true")
    args = ap.parse_args(argv)

    tools = os.path.abspath(args.tools)
    out = os.path.abspath(args.out)
    for t in ("qbsp", "vis", "light"):
        if not os.access(os.path.join(tools, t), os.X_OK):
            print(f"compile_arena: no {t} in {tools} (run tools/setup-build.sh)", file=sys.stderr)
            return 1
    os.makedirs(out, exist_ok=True)
    for stem in STEMS:
        p = os.path.join(out, stem)
        if os.path.lexists(p):
            os.remove(p)

    mapf = os.path.join(out, "psqarena.map")
    bspf = os.path.join(out, "psqarena.bsp")
    script = OTHER_GENERATORS.get(args.arena, "generate_arena.py")
    gen = [sys.executable, "-I", "-B", os.path.join(HERE, script),
           "--pak", os.path.abspath(args.pak), "--map", mapf,
           "--wad", os.path.join(out, "psqarena.wad")]
    if args.break_kind:
        if script != "generate_arena.py":
            print(f"compile_arena: --break is the test hall's, not {args.arena}'s", file=sys.stderr)
            return 1
        gen += ["--break", args.break_kind]
    if script != "generate_testhazards.py":
        gen += ["--arena", args.arena]
    if script != "generate_testhazards.py":
        gen += ["--plan", os.path.join(out, "psqarena.plan.svg"),
                "--shots", os.path.join(out, "psqarena.shots")]
    steps = [
        ("generate", gen),
        ("qbsp", [os.path.join(tools, "qbsp"), "-nopercent", "-wadpath", out, mapf, bspf]),
        ("vis", [os.path.join(tools, "vis"), "-nopercent", bspf]),
        ("light", [os.path.join(tools, "light"), "-nopercent", "-extra4", "-lit", bspf]),
    ]
    ok = True
    for step, cmd in steps:
        if not run(step, cmd, out):
            ok = False
            if not args.keep_going or step == "generate":
                return 1
    # The coloured lightmap (light -lit): psqarena.lit in every arena's work
    # folder, which arena.cmake copies to <arena>.lit and the app bundles
    # beside the BSP (the torches' and flames' warm light; the engine renders
    # the map white without it).
    if ok and not os.path.exists(os.path.join(out, "psqarena.lit")):
        print("compile_arena: light wrote no psqarena.lit", file=sys.stderr)
        return 1
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
