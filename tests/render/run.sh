#!/usr/bin/env bash
# Builds swf2c, writes the generated test movies, builds the AS2 corpus (scripted cases
# need MTASC: tests/fetch-mtasc.sh), and runs the render tests against the committed
# Ruffle references.
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
# Scripted cases come from the AS2 corpus (MTASC; tests/fetch-mtasc.sh).
bash "$root/tests/build-corpus.sh" "$root/out/corpus" >/dev/null
gen_args=()
if command -v ninja >/dev/null; then gen_args=(--generator Ninja); fi
"$py" "$root/tests/render/run-render-tests.py" --swf2c "$swf2c" --player "$player" \
    --gen-dir "$root/out/gen" --corpus "$root/out/corpus" --out "$root/out/render" \
    "${gen_args[@]}" "${phase_args[@]}"
