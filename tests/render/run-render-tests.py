"""Converts each render-test SWF with swf2c, renders frame 1 headless with anyswf-player
and compares it with Ruffle's reference render (imgdiff.py thresholds).

    python tests/render/run-render-tests.py --swf2c <swf2c> --player <anyswf-player>
        --gen-dir out/gen --out out/render [--corpus out/corpus] [--generator G]
        [--max-phase N] [--only NAME...]

Cases with `code = true` have ActionScript (tests/render/as, compiled by
tests/build-corpus.sh into <corpus>/render): they are compiled with `swf2c compile`
and built into one player per case through cmake/anyswf-game.cmake.
"""

import argparse
import os
import shutil
import subprocess
import sys

from imgdiff import compare
from render_cases import HERE, ROOT, load_cases, swf_path


def build_code_cases(args, cases):
    """swf2c compile + one CMake build for every scripted case; returns name -> (exe, bin)."""
    out = {}
    proj = os.path.join(args.out, "code")
    os.makedirs(proj, exist_ok=True)
    lines = ["cmake_minimum_required(VERSION 3.24)", "project(anyswf_render_tests C)",
             "set(BUILD_TESTING OFF CACHE BOOL \"\" FORCE)",
             f"add_subdirectory(\"{ROOT.replace(os.sep, '/')}\" anyswf)"]
    built = []
    for case in cases:
        d = os.path.join(proj, "cases", case["name"])
        shutil.rmtree(d, ignore_errors=True)
        r = subprocess.run([args.swf2c, "compile", swf_path(case, args.gen_dir, args.corpus), "-o", d],
                           capture_output=True, text=True)
        if r.returncode != 0:
            out[case["name"]] = ("swf2c: " + r.stderr.strip(), None)
            continue
        lines.append(f"anyswf_add_game(r_{case['name']} \"{os.path.join(d, 'game.c').replace(os.sep, '/')}\")")
        built.append((case["name"], d))
    if not built:
        return out
    with open(os.path.join(proj, "CMakeLists.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    bdir = os.path.join(proj, "cmake-out")
    cfg = ["cmake", "-S", proj, "-B", bdir, "-DCMAKE_BUILD_TYPE=Release"]
    if args.generator:
        cfg += ["-G", args.generator] + (["-A", "x64"] if args.generator.startswith("Visual Studio") else [])
    r = subprocess.run(cfg, capture_output=True, text=True)
    if r.returncode == 0:
        r = subprocess.run(["cmake", "--build", bdir, "--config", "Release", "--parallel", str(args.jobs)],
                           capture_output=True, text=True)
    for name, d in built:
        exe = next((p for p in (os.path.join(bdir, "Release", f"r_{name}.exe"), os.path.join(bdir, f"r_{name}.exe"),
                                os.path.join(bdir, f"r_{name}")) if os.path.isfile(p)), None)
        out[name] = (exe, os.path.join(d, "assets.bin")) if exe else ("build failed: " + r.stderr[-300:], None)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--swf2c", required=True)
    ap.add_argument("--player", required=True)
    ap.add_argument("--gen-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--corpus", help="tests/build-corpus.sh output (default out/corpus)")
    ap.add_argument("--generator")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--max-phase", type=int)
    ap.add_argument("--only", nargs="*")
    args = ap.parse_args()
    args.swf2c = os.path.abspath(args.swf2c)
    args.player = os.path.abspath(args.player)
    args.out = os.path.abspath(args.out)
    os.makedirs(args.out, exist_ok=True)
    results = []
    cases = [c for c in load_cases(args.max_phase) if not args.only or c["name"] in args.only]
    code = build_code_cases(args, [c for c in cases if c.get("code")])
    for case in cases:
        name = case["name"]
        ref = os.path.join(HERE, "ref", name + ".png")
        png = os.path.join(args.out, name + ".png")
        if case.get("code"):
            exe, bin_path = code[name]
            if bin_path is None:
                results.append((name, False, exe))
                continue
            player = exe
        else:
            player = args.player
            bin_path = os.path.join(args.out, name + ".bin")
            r = subprocess.run([args.swf2c, "assets", swf_path(case, args.gen_dir), "-o", bin_path],
                               capture_output=True, text=True)
            if r.returncode != 0:
                results.append((name, False, "swf2c: " + r.stderr.strip()))
                continue
        r = subprocess.run([player, bin_path, "--screenshot", png], capture_output=True, text=True)
        if r.returncode != 0:
            results.append((name, False, "player: " + (r.stderr or r.stdout).strip()))
            continue
        if not os.path.exists(ref):
            results.append((name, False, "no reference image"))
            continue
        d = compare(png, ref)
        detail = d.get("reason") or f"bad {d['bad_fraction'] * 100:.2f}%, mean {d['mean_diff']:.2f}"
        results.append((name, d["ok"], detail))
    xfail = {c["name"] for c in load_cases() if c.get("known_failure")}
    failed = 0
    for name, ok, detail in results:
        if name in xfail:
            tag = "XPASS" if ok else "XFAIL"
        else:
            tag = "PASS" if ok else "FAIL"
            failed += not ok
        print(f"{tag} {name}: {detail}")
    counted = [r for r in results if r[0] not in xfail]
    print(f"render tests: {len(counted) - failed}/{len(counted)} passed, {len(results) - len(counted)} known failure(s)")
    sys.exit(0 if results and failed == 0 else 1)


if __name__ == "__main__":
    main()
