"""Renders every render-test case with Ruffle's exporter into tests/render/ref/.

    python tests/render/make-render-refs.py --exporter <path/to/exporter> --gen-dir out/gen

The exporter is built from Ruffle (MIT/Apache-2.0) outside this repository:
    cd <ruffle checkout> && CARGO_TARGET_DIR=<scratch> cargo build --release --locked -p exporter
It renders frame 1 at 480x272 ("show all" scaling), the size AnySWF targets.
"""

import argparse
import os
import subprocess
import sys

from render_cases import HERE, load_cases, swf_path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exporter", required=True)
    ap.add_argument("--gen-dir", required=True)
    ap.add_argument("--corpus", help="tests/build-corpus.sh output (for corpus: cases)")
    ap.add_argument("--only", nargs="*")
    args = ap.parse_args()
    args.exporter = os.path.abspath(args.exporter)
    ref_dir = os.path.join(HERE, "ref")
    os.makedirs(ref_dir, exist_ok=True)
    failed = 0
    for case in load_cases():
        if args.only and case["name"] not in args.only:
            continue
        out = os.path.join(ref_dir, case["name"] + ".png")
        cmd = [args.exporter, "-s", "--width", "480", "--height", "272", swf_path(case, args.gen_dir, args.corpus), out]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"FAIL {case['name']}: {r.stderr.strip()}")
            failed += 1
        else:
            print(f"ref  {case['name']}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
