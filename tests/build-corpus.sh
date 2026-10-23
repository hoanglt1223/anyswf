#!/usr/bin/env bash
# Compiles every self-authored AS2 test in tests/swf-src/ into a SWF 8 movie
# (480x272, 30 fps) with MTASC, next to its expected trace output.
#
#   tests/build-corpus.sh [out-dir]     default: out/corpus
#
# MTASC comes from $MTASC, else tools/mtasc (tests/fetch-mtasc.sh), else PATH.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="$root/tests/swf-src"
out="${1:-$root/out/corpus}"

mtasc="${MTASC:-}"
if [ -z "$mtasc" ]; then
    for candidate in "$root/tools/mtasc/mtasc.exe" "$root/tools/mtasc/mtasc"; do
        if [ -x "$candidate" ]; then mtasc="$candidate"; break; fi
    done
fi
if [ -z "$mtasc" ]; then
    mtasc="$(command -v mtasc || true)"
fi
if [ -z "$mtasc" ]; then
    echo "build-corpus: MTASC not found; run tests/fetch-mtasc.sh or set MTASC" >&2
    exit 1
fi

std_args=()
std_dir="$(dirname "$mtasc")"
if [ -d "$std_dir/std" ]; then
    std_args+=(-cp "$std_dir/std")
    if [ -d "$std_dir/std8" ]; then std_args+=(-cp "$std_dir/std8"); fi
fi

mkdir -p "$out"
count=0
for as in "$src"/*.as; do
    name="$(basename "$as" .as)"
    if [ ! -f "$src/$name.expected" ]; then
        echo "build-corpus: $name.as has no $name.expected" >&2
        exit 1
    fi
    # MTASC resolves the main class through the classpath, so pass a path relative to it.
    (cd "$src" && "$mtasc" "${std_args[@]}" -cp . -version 8 -header 480:272:30 -main \
        -swf "$out/$name.swf" "$name.as")
    cp "$src/$name.expected" "$out/$name.expected"
    count=$((count + 1))
done
echo "build-corpus: built $count SWF(s) into $out"
