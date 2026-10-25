"""Runs the self-authored AS2 corpus as PSP EBOOTs under PPSSPPHeadless and compares each
movie's trace output with its .expected file.

    python tests/psp/run-psp-corpus.py --anyswf <anyswf> [--corpus out/corpus] [--out out/psp-corpus]
        [--image ppsspp-headless:local] [--seconds 3] [--only NAME...]

Each movie is built with `anyswf build --target psp` (one shared work folder, so only the
game code recompiles) and run from a scratch memory stick in Docker. PPSSPP prints the
player's log on stdout; lines starting with "plat:" and the final TIMEOUT line are not trace.
"""

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def normalize(text):
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    lines = [l for l in lines if not l.startswith("plat:") and not l.startswith("TIMEOUT")]
    while lines and lines[-1] == "":
        lines.pop()
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--anyswf", required=True)
    ap.add_argument("--corpus", default=os.path.join(ROOT, "out", "corpus"))
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "psp-corpus"))
    ap.add_argument("--image", default="ppsspp-headless:local")
    ap.add_argument("--seconds", type=int, default=3)
    ap.add_argument("--only", nargs="*")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    names = sorted(f[:-4] for f in os.listdir(args.corpus) if f.endswith(".swf"))
    if args.only:
        names = [n for n in names if n in args.only]
    if not names:
        print(f"psp corpus: no SWF files in {args.corpus}", file=sys.stderr)
        return 1
    stick = os.path.join(out, "ms")
    user = [] if os.name == "nt" else ["--user", f"{os.getuid()}:{os.getgid()}"]
    passed = 0
    for name in names:
        game = os.path.join(out, "games", name)
        r = subprocess.run([args.anyswf, "build", os.path.join(args.corpus, name + ".swf"), "--target", "psp",
                            "-o", game, "--work", os.path.join(out, "work"), "--title", name],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"FAIL {name}: anyswf build failed\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
            continue
        folder = os.path.join(stick, "PSP", "GAME", name.upper())
        shutil.rmtree(folder, ignore_errors=True)
        shutil.copytree(game, folder)
        r = subprocess.run(["docker", "run", "--rm", *user, "-v", f"{out}:/w", "-w", "/w", args.image,
                            "PPSSPPHeadless", "--memstick=/w/ms", f"--timeout-emulated={args.seconds}",
                            f"/w/ms/PSP/GAME/{name.upper()}/EBOOT.PBP"],
                           capture_output=True, text=True, errors="replace")
        got = normalize(r.stdout)
        with open(os.path.join(args.corpus, name + ".expected"), encoding="utf-8", errors="replace") as f:
            want = normalize(f.read())
        if got == want:
            passed += 1
            print(f"PASS {name}")
        else:
            at = next((i for i, (a, b) in enumerate(zip(got, want)) if a != b), min(len(got), len(want)))
            g = got[at] if at < len(got) else "<end>"
            w = want[at] if at < len(want) else "<end>"
            print(f"FAIL {name}: line {at + 1}: got {g!r}, want {w!r}")
    print(f"psp corpus: {passed}/{len(names)} passed")
    return 0 if passed == len(names) else 1


if __name__ == "__main__":
    sys.exit(main())
