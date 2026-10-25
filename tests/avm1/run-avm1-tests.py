"""AVM1 conformance runner: compiles each test SWF to C with swf2c, builds one player
executable per test with CMake, runs it headless for the test's frame count and
compares the trace output with the expected output (Flash Player's, for Ruffle tests).

    python tests/avm1/run-avm1-tests.py --swf2c <swf2c> --out out/avm1
        [--corpus out/corpus] [--ruffle DIR (default tests/ruffle-avm1)] [--only NAME...]
        [--generator "Visual Studio 17 2022"] [--list-selection]

Self-authored tests may have <name>.toml next to the SWF: num_frames (default 1), input
(the player's --input script), runs (default 1: later runs reuse the case's save
directory, for SharedObject; the expected output is all runs' output in order) and
audio = true (the run must mix non-silent sound, checked through --wav). Runs use TZ=UTC
(`utc = true` marks tests that rely on it; the PSP runner skips those).

Ruffle tests are selected statically (before running): the movie must be AVM1, need no
injected input (input.json) or network, and name none of the features in UNSUPPORTED.
The selection is printed with reasons.
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tomllib
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

UNSUPPORTED = [
    # display features the runtime does not have
    "beginBitmapFill", "attachBitmap", "setMask", "loadMovie", "unloadMovie", "loadVariables", "useHandCursor",
    "_url", "_quality", "_highquality", "_focusrect", "_soundbuftime", "toggleHighQuality", "tabIndex",
    "tabEnabled", "filters", "blendMode", "cacheAsBitmap", "scrollRect", "scale9Grid", "transform",
    "opaqueBackground", "_lockroot", "menu", "focusEnabled", "trackAsMenu", "getTextSnapshot", "hitArea",
    "lineGradientStyle",
    # events that need focus, loading or text input
    "onData", "onSetFocus", "onKillFocus", "onChanged", "onScroller", "onMouseWheel",
    # classes not in the runtime
    "XML", "XMLNode", "LoadVars", "LocalConnection", "NetConnection", "NetStream", "Camera", "Microphone",
    "Video", "Selection", "System", "Accessibility", "ContextMenu", "ContextMenuItem", "PrintJob",
    "ExternalInterface", "BitmapData", "BlurFilter", "GlowFilter", "DropShadowFilter", "BevelFilter",
    "ColorMatrixFilter", "ConvolutionFilter", "DisplacementMapFilter", "GradientBevelFilter",
    "GradientGlowFilter", "Transform", "ColorTransform", "Matrix", "Point", "Rectangle", "MovieClipLoader",
    "TextSnapshot", "FileReference", "FileReferenceList", "CustomActions", "TextRenderer", "StyleSheet", "IME",
    "capabilities", "security", "flash", "XMLSocket", "loadSound", "id3",
    # natives and host
    "ASnative", "ASconstructor", "fscommand", "getURL", "getVersion", "$version", "_level1", "_level2",
]
UNSUPPORTED_SET = set(UNSUPPORTED)


def block(data, tag):
    n = struct.unpack_from("<I", data, 12)[0]
    for i in range(n):
        t, off, size, count = struct.unpack_from("<4sIII", data, 64 + i * 16)
        if t == tag:
            return data[off:off + size], count
    return b"", 0


def script_kinds(assets_bin):
    with open(assets_bin, "rb") as f:
        data = f.read()
    raw, count = block(data, b"SCRP")
    return [raw[i * 8] for i in range(count)]


def edit_text_count(assets_bin):
    with open(assets_bin, "rb") as f:
        data = f.read()
    return block(data, b"EDIT")[1]


ATOM_RE = re.compile(r'^    "((?:[^"\\]|\\.)*)",$', re.M)


def atoms_of(game_c):
    with open(game_c, encoding="utf-8") as f:
        src = f.read()
    start = src.find("ATOM_UTF8[")
    end = src.find("};", start)
    return {m.group(1) for m in ATOM_RE.finditer(src[start:end])}


def normalize(text):
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    while lines and lines[-1] == "":
        lines.pop()
    return lines


def wav_has_sound(path):
    try:
        with wave.open(path, "rb") as w:
            data = w.readframes(w.getnframes())
    except (OSError, wave.Error, EOFError):
        return False
    peak = max((abs(int.from_bytes(data[i:i + 2], "little", signed=True)) for i in range(0, len(data) - 1, 2)),
               default=0)
    return peak > 1000


def collect_cases(args):
    cases = []
    if args.corpus and os.path.isdir(args.corpus):
        for f in sorted(os.listdir(args.corpus)):
            if f.endswith(".swf"):
                name = f[:-4]
                exp = os.path.join(args.corpus, name + ".expected")
                meta = {}
                toml_path = os.path.join(args.corpus, name + ".toml")
                if os.path.isfile(toml_path):
                    with open(toml_path, "rb") as tf:
                        meta = tomllib.load(tf)
                cases.append({"name": "self_" + name, "swf": os.path.join(args.corpus, f), "expected": exp,
                              "frames": int(meta.get("num_frames", 1)), "kind": "self",
                              "input": meta.get("input"), "runs": int(meta.get("runs", 1)),
                              "audio": bool(meta.get("audio", False))})
    if args.ruffle and os.path.isdir(args.ruffle):
        for d in sorted(os.listdir(args.ruffle)):
            p = os.path.join(args.ruffle, d)
            swf = os.path.join(p, "test.swf")
            if not os.path.isfile(swf) or not os.path.isfile(os.path.join(p, "output.txt")):
                continue
            frames = 1
            known = False
            toml_path = os.path.join(p, "test.toml")
            if os.path.isfile(toml_path):
                with open(toml_path, "rb") as f:
                    raw = f.read()
                try:
                    meta = tomllib.loads(raw.decode("utf-8"))
                except (tomllib.TOMLDecodeError, UnicodeDecodeError):
                    # Some Ruffle files use TOML 1.1 syntax; read the two keys we need.
                    text = raw.decode("utf-8", "replace")
                    m = re.search(r"^num_frames\s*=\s*(\d+)", text, re.M)
                    meta = {"num_frames": int(m.group(1)) if m else 1,
                            "known_failure": bool(re.search(r"^known_failure\s*=\s*true", text, re.M)),
                            "ignore": bool(re.search(r"^ignore\s*=\s*true", text, re.M))}
                frames = int(meta.get("num_frames", 1))
                known = bool(meta.get("known_failure", False)) or bool(meta.get("ignore", False))
            cases.append({"name": "ruffle_" + d, "swf": swf, "expected": os.path.join(p, "output.txt"),
                          "frames": frames, "kind": "ruffle", "known_failure": known})
    return cases


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", **kw)


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--swf2c", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--corpus")
    ap.add_argument("--ruffle", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ruffle-avm1"))
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--generator")
    ap.add_argument("--config", default="Release")
    ap.add_argument("--list-selection", action="store_true")
    ap.add_argument("--timeout", type=int, default=60)
    ap.add_argument("--min-ruffle", type=float, default=80.0,
                    help="pass rate (percent) the Ruffle tests must reach (default 80)")
    ap.add_argument("--jobs", type=int, help="parallel build jobs (default: the generator's)")
    args = ap.parse_args()
    swf2c = os.path.abspath(args.swf2c)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)

    cases = [c for c in collect_cases(args) if not args.only or c["name"] in args.only]
    selected, excluded = [], []
    for c in cases:
        d = os.path.join(out, "cases", c["name"])
        if os.path.isdir(d):
            shutil.rmtree(d)
        r = run([swf2c, "compile", c["swf"], "-o", d])
        if r.returncode != 0:
            reason = "swf2c: " + (r.stderr.strip().splitlines() or ["failed"])[-1]
            if c["kind"] == "ruffle" and ("ActionScript 3" in reason or "not a SWF" in reason):
                excluded.append((c, reason))
                continue
            c["compile_error"] = reason
            selected.append(c)
            continue
        c["dir"] = d
        if c["kind"] == "ruffle":
            if c.get("known_failure"):
                excluded.append((c, "known failure in Ruffle"))
                continue
            kinds = script_kinds(os.path.join(d, "assets.bin"))
            if os.path.exists(os.path.join(os.path.dirname(c["swf"]), "input.json")):
                excluded.append((c, "scripted Ruffle input (input.json)"))
                continue
            if not kinds:
                excluded.append((c, "no ActionScript"))
                continue
            with open(os.path.join(d, "game.c"), encoding="utf-8") as f:
                src = f.read()
            if "swf_act_get_url" in src:
                excluded.append((c, "getURL/loadMovie/loadVariables (network is out of scope)"))
                continue
            hits = sorted(atoms_of(os.path.join(d, "game.c")) & UNSUPPORTED_SET)
            if hits:
                excluded.append((c, "uses " + ", ".join(hits[:3])))
                continue
        selected.append(c)

    if args.list_selection:
        for c, why in excluded:
            print(f"EXCLUDE {c['name']}: {why}")
    print(f"avm1: {len(selected)} selected, {len(excluded)} excluded")

    buildable = [c for c in selected if "dir" in c]
    proj = os.path.join(out, "project")
    os.makedirs(proj, exist_ok=True)
    root_cmake = ROOT.replace("\\", "/")
    lines = [
        "cmake_minimum_required(VERSION 3.24)",
        "project(anyswf_avm1_tests C)",
        "set(BUILD_TESTING OFF CACHE BOOL \"\" FORCE)",
        f"add_subdirectory(\"{root_cmake}\" anyswf)",
    ]
    for c in buildable:
        game_c = os.path.join(c["dir"], "game.c").replace("\\", "/")
        lines.append(f"anyswf_add_game(t_{c['name']} \"{game_c}\")")
    with open(os.path.join(proj, "CMakeLists.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    bdir = os.path.join(out, "cmake-out")
    cfg = ["cmake", "-S", proj, "-B", bdir]
    if args.generator:
        cfg += ["-G", args.generator]
        if args.generator.startswith("Visual Studio"):
            cfg += ["-A", "x64"]
    r = run(cfg)
    if r.returncode != 0:
        print(r.stdout[-4000:], r.stderr[-4000:])
        sys.exit(2)
    build = ["cmake", "--build", bdir, "--config", args.config, "--parallel"]
    if args.jobs:
        build.append(str(args.jobs))
    r = run(build)
    build_log = r.stdout + r.stderr

    results = []
    for c in selected:
        name = c["name"]
        if "compile_error" in c:
            results.append((c, False, c["compile_error"]))
            continue
        exe = None
        for cand in [os.path.join(bdir, args.config, f"t_{name}.exe"), os.path.join(bdir, f"t_{name}.exe"),
                     os.path.join(bdir, f"t_{name}")]:
            if os.path.isfile(cand):
                exe = cand
                break
        if not exe:
            err = [l for l in build_log.splitlines() if f"t_{name}" in l and "error" in l.lower()]
            results.append((c, False, "C compile failed: " + (err[0][-200:] if err else "see compile.log")))
            continue
        save_dir = os.path.join(c["dir"], "save")
        shutil.rmtree(save_dir, ignore_errors=True)
        wav_path = os.path.join(c["dir"], "out.wav")
        cmd = [exe, os.path.join(c["dir"], "assets.bin"), "--headless", "--frames", str(c["frames"]),
               "--save-dir", save_dir]
        if c.get("input"):
            cmd += ["--input", c["input"]]
        if c.get("audio"):
            cmd += ["--wav", wav_path]
        env = dict(os.environ, TZ="UTC")
        got = []
        try:
            for _ in range(c.get("runs", 1)):
                r = run(cmd, timeout=args.timeout, env=env)
                got += normalize(r.stdout)
        except subprocess.TimeoutExpired:
            results.append((c, False, "timeout"))
            continue
        if c.get("audio") and not wav_has_sound(wav_path):
            results.append((c, False, "no sound in " + wav_path))
            continue
        with open(c["expected"], encoding="utf-8", errors="replace") as f:
            want = normalize(f.read())
        if got == want:
            results.append((c, True, ""))
        else:
            diff_at = next((i for i, (a, b) in enumerate(zip(got, want)) if a != b), min(len(got), len(want)))
            g = got[diff_at] if diff_at < len(got) else "<end>"
            w = want[diff_at] if diff_at < len(want) else "<end>"
            results.append((c, False, f"line {diff_at + 1}: got {g!r}, want {w!r} (exit {r.returncode})"))
            with open(os.path.join(c["dir"], "actual.txt"), "w", encoding="utf-8") as f:
                f.write("\n".join(got) + "\n")
    with open(os.path.join(out, "compile.log"), "w", encoding="utf-8") as f:
        f.write(build_log)

    for kind in ("self", "ruffle"):
        rs = [r for r in results if r[0]["kind"] == kind]
        if not rs:
            continue
        for c, ok, why in rs:
            print(f"{'PASS' if ok else 'FAIL'} {c['name']}{'' if ok else ': ' + why}")
        passed = sum(1 for r in rs if r[1])
        print(f"avm1 {kind}: {passed}/{len(rs)} passed ({100.0 * passed / len(rs):.1f}%)")
    self_ok = all(r[1] for r in results if r[0]["kind"] == "self")
    ruffle = [r for r in results if r[0]["kind"] == "ruffle"]
    ruffle_ok = not ruffle or sum(1 for r in ruffle if r[1]) >= args.min_ruffle / 100.0 * len(ruffle)
    sys.exit(0 if results and self_ok and ruffle_ok else 1)


if __name__ == "__main__":
    main()
