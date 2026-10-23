"""Converts each render-test SWF with swf2c, renders frame 1 headless with anyswf-player
and compares it with Ruffle's reference render (imgdiff.py thresholds).

    python tests/render/run-render-tests.py --swf2c <swf2c> --player <anyswf-player>
        --gen-dir out/gen --out out/render [--max-phase N] [--only NAME...]
"""

import argparse
import os
import subprocess
import sys

from imgdiff import compare
from render_cases import HERE, load_cases, swf_path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--swf2c", required=True)
    ap.add_argument("--player", required=True)
    ap.add_argument("--gen-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-phase", type=int)
    ap.add_argument("--only", nargs="*")
    args = ap.parse_args()
    args.swf2c = os.path.abspath(args.swf2c)
    args.player = os.path.abspath(args.player)
    os.makedirs(args.out, exist_ok=True)
    results = []
    for case in load_cases(args.max_phase):
        name = case["name"]
        if args.only and name not in args.only:
            continue
        ref = os.path.join(HERE, "ref", name + ".png")
        bin_path = os.path.join(args.out, name + ".bin")
        png = os.path.join(args.out, name + ".png")
        r = subprocess.run([args.swf2c, "assets", swf_path(case, args.gen_dir), "-o", bin_path], capture_output=True, text=True)
        if r.returncode != 0:
            results.append((name, False, "swf2c: " + r.stderr.strip()))
            continue
        r = subprocess.run([args.player, bin_path, "--screenshot", png], capture_output=True, text=True)
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
