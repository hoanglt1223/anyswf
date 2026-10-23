"""Shared case loading for the render-test scripts."""

import os
import tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def load_cases(max_phase=None):
    with open(os.path.join(HERE, "cases.toml"), "rb") as f:
        cases = tomllib.load(f)["case"]
    if max_phase is not None:
        cases = [c for c in cases if c["phase"] <= max_phase]
    return cases


def swf_path(case, gen_dir):
    swf = case["swf"]
    if swf.startswith("gen:"):
        return os.path.join(gen_dir, swf[4:] + ".swf")
    return os.path.join(ROOT, swf)
