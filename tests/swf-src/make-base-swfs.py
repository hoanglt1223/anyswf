"""Writes the self-made base movies that some corpus tests compile their classes into
(MTASC -swf <base> -out <test>.swf). A test opts in with a first line `// base: NAME`.

    python tests/swf-src/make-base-swfs.py <out-dir>

library.swf (SWF 8, 480x272, 30 fps, one frame):
  - shape 1: red 40x40 square from (0,0) to (40,40)
  - sprite 2 "box": the square; exported as "box"
  - sound 3 "beep": 0.1 s of a 440 Hz tone, 22050 Hz mono PCM16; exported as "beep"
  - button 4: the square in every state, placed on the root as "btn" at (200,100)
    at depth 1; its actions trace "btn release" on release and "btn key" on Space
"""

import math
import os
import struct
import sys


class Bits:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, value, bits):
        for i in range(bits - 1, -1, -1):
            self.acc = (self.acc << 1) | ((value >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                self.acc = 0
                self.n = 0

    def put_signed(self, value, bits):
        self.put(value & ((1 << bits) - 1), bits)

    def flush(self):
        if self.n:
            self.put(0, 8 - self.n)
        return bytes(self.out)


def sbits(*values):
    need = 1
    for v in values:
        while not (-(1 << (need - 1)) <= v < (1 << (need - 1))):
            need += 1
    return need


def rect(xmin, xmax, ymin, ymax):
    b = Bits()
    n = sbits(xmin, xmax, ymin, ymax)
    b.put(n, 5)
    for v in (xmin, xmax, ymin, ymax):
        b.put_signed(v, n)
    return b.flush()


def matrix(tx=0, ty=0):
    b = Bits()
    b.put(0, 1)  # no scale
    b.put(0, 1)  # no rotate
    n = sbits(tx, ty) if (tx or ty) else 0
    b.put(n, 5)
    if n:
        b.put_signed(tx, n)
        b.put_signed(ty, n)
    return b.flush()


def cxform_alpha_identity():
    b = Bits()
    b.put(0, 1)  # no add terms
    b.put(0, 1)  # no mult terms
    b.put(0, 4)
    return b.flush()


def tag(code, body):
    if len(body) < 63:
        return struct.pack("<H", (code << 6) | len(body)) + body
    return struct.pack("<HI", (code << 6) | 0x3F, len(body)) + body


def square_shape(shape_id, size_twips, rgb):
    body = struct.pack("<H", shape_id) + rect(0, size_twips, 0, size_twips)
    body += bytes([1, 0x00]) + bytes(rgb)  # one solid fill
    body += bytes([0])                     # no line styles
    b = Bits()
    b.put(1, 4)  # fill bits
    b.put(0, 4)  # line bits
    # Style change: move to (0,0), fill style 1.
    b.put(0, 1)
    b.put(0, 1)  # new styles
    b.put(0, 1)  # line style
    b.put(1, 1)  # fill style 1
    b.put(0, 1)  # fill style 0
    b.put(1, 1)  # move to
    b.put(1, 5)
    b.put_signed(0, 1)
    b.put_signed(0, 1)
    b.put(1, 1)  # fill style 1 index
    n = sbits(size_twips, -size_twips)
    for dx, dy in ((size_twips, 0), (0, size_twips), (-size_twips, 0), (0, -size_twips)):
        b.put(1, 1)  # edge
        b.put(1, 1)  # straight
        b.put(n - 2, 4)
        b.put(0, 1)  # not a general line
        if dx:
            b.put(0, 1)
            b.put_signed(dx, n)
        else:
            b.put(1, 1)
            b.put_signed(dy, n)
    b.put(0, 6)  # end of shape
    return tag(2, body + b.flush())


def place(depth, char_id, name=None, tx=0, ty=0):
    flags = 0x02 | 0x04 | (0x20 if name else 0)
    body = struct.pack("<BHH", flags, depth, char_id) + matrix(tx, ty)
    if name:
        body += name.encode() + b"\0"
    return tag(26, body)


def push_string_trace(text):
    data = b"\0" + text.encode() + b"\0"
    return bytes([0x96]) + struct.pack("<H", len(data)) + data + bytes([0x26])


def button(button_id, char_id):
    records = bytes([0x0F]) + struct.pack("<HH", char_id, 1) + matrix() + cxform_alpha_identity()
    records += b"\0"
    release = push_string_trace("btn release") + b"\0"
    key = push_string_trace("btn key") + b"\0"
    # Conditions: byte 0 bit 3 = OverDownToOverUp; byte 1 bits 7..1 = key code.
    cond_release = struct.pack("<H", 4 + len(release)) + bytes([0x08, 0x00]) + release
    cond_key = struct.pack("<H", 0) + bytes([0x00, 32 << 1]) + key
    body = struct.pack("<HB", button_id, 0)
    body += struct.pack("<H", 2 + len(records)) + records + cond_release + cond_key
    return tag(34, body)


def beep_sound(sound_id):
    rate = 22050
    frames = rate // 10
    pcm = b"".join(struct.pack("<h", int(12000 * math.sin(2 * math.pi * 440 * i / rate))) for i in range(frames))
    flags = (3 << 4) | (2 << 2) | (1 << 1) | 0  # uncompressed LE, 22 kHz, 16-bit, mono
    return tag(14, struct.pack("<HBI", sound_id, flags, frames) + pcm)


def export(pairs):
    body = struct.pack("<H", len(pairs))
    for cid, name in pairs:
        body += struct.pack("<H", cid) + name.encode() + b"\0"
    return tag(56, body)


def library():
    sprite_tags = place(1, 1) + tag(1, b"") + tag(0, b"")
    tags = [
        tag(69, struct.pack("<I", 0)),
        tag(9, bytes([0xFF, 0xFF, 0xFF])),
        square_shape(1, 800, (0xE0, 0x20, 0x20)),
        tag(39, struct.pack("<HH", 2, 1) + sprite_tags),
        beep_sound(3),
        button(4, 1),
        export([(2, "box"), (3, "beep")]),
        place(1, 4, "btn", 200 * 20, 100 * 20),
        tag(1, b""),
        tag(0, b""),
    ]
    body = rect(0, 480 * 20, 0, 272 * 20) + struct.pack("<HH", 30 << 8, 1) + b"".join(tags)
    return b"FWS" + bytes([8]) + struct.pack("<I", 8 + len(body)) + body


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: make-base-swfs.py <out-dir>")
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "library.swf"), "wb") as f:
        f.write(library())


if __name__ == "__main__":
    main()
