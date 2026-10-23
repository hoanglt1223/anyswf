#!/usr/bin/env bash
# Installs MTASC 1.14 (the open-source AS2 compiler) into tools/mtasc/ with a
# pinned SHA-256. Layout: tools/mtasc/mtasc[.exe], std/, std8/.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="$root/tools/mtasc"

# mtasc.org is gone; the Windows build comes from the Internet Archive's copy of
# the official zip, the Linux build from Ubuntu's archive.
win_url="https://web.archive.org/web/2015id_/http://www.mtasc.org/zip/mtasc-1.14.zip"
win_sha="99730a81bdc9ad38d2c9e4da0bff2e4cbe98543d93d438bb77327d1323742e4a"
linux_url="https://mirrors.mit.edu/ubuntu/pool/universe/m/mtasc/mtasc_1.14-3build5_amd64.deb"
linux_sha="c563e8670078bb508ae25e5cfe826e01f9c6fdd6d4587f0502e2f97d05448714"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) kind=windows; exe=mtasc.exe ;;
    Linux) kind=linux; exe=mtasc ;;
    *) echo "fetch-mtasc: unsupported host $(uname -s)" >&2; exit 1 ;;
esac
if [ "$kind" = linux ] && [ "$(uname -m)" != x86_64 ]; then
    echo "fetch-mtasc: only x86_64 Linux is supported" >&2
    exit 1
fi

if [ -x "$dest/$exe" ] && [ -d "$dest/std" ] && [ -d "$dest/std8" ]; then
    echo "fetch-mtasc: already installed at $dest"
    exit 0
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

verify() {
    local file="$1" want="$2" got
    got="$(sha256sum "$file" | cut -d' ' -f1)"
    if [ "$got" != "$want" ]; then
        echo "fetch-mtasc: checksum mismatch for $file (got $got)" >&2
        exit 1
    fi
}

rm -rf "$dest"
mkdir -p "$dest"
if [ "$kind" = windows ]; then
    curl -fsSL --retry 3 -o "$work/mtasc.zip" "$win_url"
    verify "$work/mtasc.zip" "$win_sha"
    unzip -q "$work/mtasc.zip" -d "$work/x"
    cp -R "$work/x/mtasc-1.14/mtasc.exe" "$work/x/mtasc-1.14/std" "$work/x/mtasc-1.14/std8" "$dest/"
else
    curl -fsSL --retry 3 -o "$work/mtasc.deb" "$linux_url"
    verify "$work/mtasc.deb" "$linux_sha"
    if command -v dpkg-deb >/dev/null; then
        dpkg-deb -x "$work/mtasc.deb" "$work/x"
    else
        (cd "$work" && ar x mtasc.deb && mkdir x && tar -xf data.tar.* -C x)
    fi
    cp "$work/x/usr/bin/mtasc" "$dest/"
    cp -R "$work/x/usr/share/mtasc/std" "$work/x/usr/share/mtasc/std8" "$dest/"
    chmod +x "$dest/mtasc"
fi
echo "fetch-mtasc: installed $("$dest/$exe" 2>&1 | head -n 1 || true)"
