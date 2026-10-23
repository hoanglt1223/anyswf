"""Perceptual comparison of two PNG renders (Python standard library only).

Both images are blurred with a 3x3 box filter first, so anti-aliased edges (Ruffle) and
aliased edges (AnySWF, which has no MSAA on the PSP) meet halfway. A pixel counts as
different when any channel differs by more than PIXEL_TOL after blurring.
"""

import struct
import sys
import zlib

PIXEL_TOL = 48
MAX_BAD_FRACTION = 0.01
MAX_MEAN_DIFF = 4.0


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, ihdr = 8, b"", None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos : pos + 4])
        ctype = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + length]
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif ctype == b"IDAT":
            idat += body
        pos += 12 + length
    w, h, depth, ctype, _, _, interlace = ihdr
    if depth != 8 or ctype not in (2, 6) or interlace:
        raise ValueError(f"{path}: only 8-bit RGB/RGBA non-interlaced PNGs are supported")
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    i = 0
    for y in range(h):
        ft = raw[i]
        line = bytearray(raw[i + 1 : i + 1 + stride])
        i += 1 + stride
        if ft == 1:
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 255
        elif ft == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif ft == 3:
            for x in range(stride):
                left = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 255
        elif ft == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        out[y * stride : (y + 1) * stride] = line
        prev = line
    rgb = bytearray(w * h * 3)
    for p in range(w * h):
        rgb[p * 3 : p * 3 + 3] = out[p * bpp : p * bpp + 3]
    return w, h, rgb


def blur(w, h, rgb):
    out = bytearray(len(rgb))
    for y in range(h):
        for x in range(w):
            for c in range(3):
                s = n = 0
                for dy in (-1, 0, 1):
                    yy = y + dy
                    if 0 <= yy < h:
                        for dx in (-1, 0, 1):
                            xx = x + dx
                            if 0 <= xx < w:
                                s += rgb[(yy * w + xx) * 3 + c]
                                n += 1
                out[(y * w + x) * 3 + c] = s // n
    return out


def compare(a_path, b_path):
    wa, ha, a = read_png(a_path)
    wb, hb, b = read_png(b_path)
    if (wa, ha) != (wb, hb):
        return {"ok": False, "reason": f"size {wa}x{ha} vs {wb}x{hb}"}
    a, b = blur(wa, ha, a), blur(wb, hb, b)
    bad = 0
    total = 0
    for p in range(wa * ha):
        d = max(abs(a[p * 3 + c] - b[p * 3 + c]) for c in range(3))
        total += d
        if d > PIXEL_TOL:
            bad += 1
    frac = bad / (wa * ha)
    mean = total / (wa * ha)
    ok = frac <= MAX_BAD_FRACTION and mean <= MAX_MEAN_DIFF
    return {"ok": ok, "bad_fraction": frac, "mean_diff": mean}


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("usage: imgdiff.py <a.png> <b.png>", file=sys.stderr)
        sys.exit(2)
    r = compare(sys.argv[1], sys.argv[2])
    print(r)
    sys.exit(0 if r["ok"] else 1)
