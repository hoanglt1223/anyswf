#!/usr/bin/env bash
# Builds swf2c, writes the generated test movies, and runs the render tests against the
# committed Ruffle references.
#
#   tests/render/run.sh <anyswf-player> [max-phase]
#
# Python comes from $PYTHON, else python3, else python. Cargo output goes to
# $CARGO_TARGET_DIR (default out/cargo).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
player="${1:?usage: tests/render/run.sh <anyswf-player> [max-phase]}"
phase_args=()
if [ -n "${2:-}" ]; then phase_args=(--max-phase "$2"); fi

py="${PYTHON:-}"
if [ -z "$py" ]; then
    for c in python3 python; do
        if "$c" -c 'import sys; sys.exit(sys.version_info < (3, 11))' 2>/dev/null; then py="$c"; break; fi
    done
fi
[ -n "$py" ] || { echo "render tests: Python 3.11+ not found (set PYTHON)" >&2; exit 1; }

export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$root/out/cargo}"
(cd "$root/swf2c" && cargo build --release --locked && cargo run --release --locked --example gen_test_swfs -- "$root/out/gen")
swf2c="$CARGO_TARGET_DIR/release/swf2c"
[ -x "$swf2c" ] || swf2c="$swf2c.exe"
"$py" "$root/tests/render/run-render-tests.py" --swf2c "$swf2c" --player "$player" \
    --gen-dir "$root/out/gen" --out "$root/out/render" "${phase_args[@]}"
