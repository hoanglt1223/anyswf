# assets.bin format (version 1)

`swf2c assets game.swf -o assets.bin` writes everything the runtime needs except
compiled scripts: meshes, textures, timelines, sounds, fonts and text records.
Writer: `swf2c/src/assets_bin.rs` (layout) and `swf2c/src/convert.rs` (content).
Loader: `runtime/src/assets.c`, which validates every offset, count and index
before use; a file that fails validation is rejected with a logged reason.
Bump `VERSION` in both places on any layout change; the loader refuses other versions.

All integers are little-endian. Offsets inside record tables are byte offsets into
the `DATA`, `STRS` or `CMDS` blocks (not file offsets). `NONE` is `0xFFFFFFFF`.

## File layout

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `ASWF` |
| 4 | 2 | version (1) |
| 6 | 2 | header size (64) |
| 8 | 4 | file size; must equal the actual size |
| 12 | 4 | block count |
| 16 | 16 | stage rect in twips: i32 x_min, y_min, x_max, y_max |
| 32 | 2 | frame rate, 8.8 fixed point |
| 34 | 2 | SWF version |
| 36 | 4 | background colour, bytes R, G, B, A |
| 40 | 4 | f32 target scale (stage pixels to screen pixels, "show all") |
| 44 | 4 | uncompressed SWF length (MovieClip.getBytesTotal) |
| 48 | 2 | u16 logical screen width; 0 together with the height means 480 |
| 50 | 2 | u16 logical screen height; 0 together with the width means 272 |
| 52 | 12 | reserved, zero |

The screen is the PSP's 480x272 for `--target psp` (the default) and the stage size for
`--target pc` and `android` (see `swf2c/src/targets.rs`). The runtime refuses a screen
above 4096 pixels on either side, and the PSP build refuses anything but 480x272.

The block directory follows at offset 64: one 16-byte entry per block, `char tag[4]`,
`u32 offset`, `u32 size`, `u32 count`. Every block starts on a 64-byte boundary of the
file, and the runtime loads the file into 64-byte aligned memory, so the GE can use
vertex, index, texture and palette data in place (all of which are additionally
16-byte aligned inside `DATA`). A missing record block means zero records.

## Record blocks

Sizes are exact: `size == count * record size`.

| Tag | Bytes | Record |
|---|---|---|
| `CHAR` | 8 | indexed by character id: u8 kind, u8 0, u16 0, u32 index into the kind's table |
| `SHAP` | 24 | i32 bounds[4] (twips), u32 first_draw, u32 draw_count |
| `DRAW` | 32 | u8 textured, u8 flags, u8 shift, u8 0, u32 texture, u32 vtx_off, u32 vtx_count, u32 idx_off, u32 idx_count, u32 0, u32 0 |
| `TEXR` | 32 | u8 format, u8 flags, u16 width, u16 height, u16 used_w, u16 used_h, u16 clut_count, u32 data_off, u32 data_size, u32 clut_off, u32 0, u32 0 |
| `MRPH` | 8 | u32 first_frame, u32 frame_count (into `MRPF`) |
| `MRPF` | 8 | u16 ratio, u16 0, u32 shape |
| `SPRT` | 24 | u32 frame_count, u32 frame_table (index into `FRMS`), u32 label_first, u32 label_count, u32 stream sound or NONE, u32 0 |
| `FRMS` | 4 | u32 offsets into `CMDS`; a sprite uses frame_count + 1 entries, frame *i* spans [off[i], off[i+1]) |
| `LABL` | 8 | u32 name (string), u32 frame (0-based) |
| `BTTN` | 16 | u32 first_record, u16 record_count, u16 flags (bit 0 track as menu), u32 first_action, u32 action_count |
| `BTRC` | 48 | u8 states, u8 blend, u16 char_id, u16 depth, u16 0, f32 matrix[6], i16 cxform[8] |
| `BTAC` | 8 | u16 conditions (SWF button condition bits 0-8), u16 key code, u32 script |
| `CLAC` | 12 | u32 clip event flags (as in the SWF), u8 key code, 3 bytes 0, u32 script |
| `EDIT` | 48 | i32 bounds[4], u16 font_id, u16 height (twips), u32 colour RGBA, u16 max_length, u16 flags (SWF EditText flags), u8 align, u8 0, u16 left_margin, u16 right_margin, u16 indent, i16 leading, u16 0, u32 variable name, u32 initial text |
| `FONT` | 24 | u32 name, u16 flags (bit 0 bold, bit 1 italic), u16 glyph_count, u32 first_glyph, u16 ascent, u16 descent, i16 leading, u16 em (1024 or 20480), u32 0 |
| `GLYF` | 12 | u16 code, i16 advance (font units), u32 shape or NONE, u32 0 |
| `SOND` | 24 | u8 format, u8 channels, u16 0, u32 rate, u32 frames, u32 data_off, u32 data_size, u32 0 |
| `EXPT` | 8 | u16 char_id, u16 0, u32 name |
| `INIT` | 8 | u16 sprite_id, u16 0, u32 script |
| `SCRP` | 8 | u8 kind (1 frame, 2 init, 3 clip event, 4 button), u8 0, u16 owner, u32 bytecode size |

`STRS` holds NUL-terminated UTF-8 strings; offset 0 is the empty string. `DATA` holds
vertex, index, texture, palette and sound data. `CMDS` holds timeline commands.
`DFNT` (optional) describes the device-text bitmap font: u32 texture, u16 first char,
u16 count, u16 cell size, u16 glyph height, u16 atlas columns, u16 0, then `count`
u8 advances in pixels.

### Kinds (`CHAR`)

0 none, 1 shape, 2 morph, 3 sprite, 4 button, 5 static text (index is a shape),
6 edit text, 7 font, 8 bitmap (index is its first texture), 9 sound.

### Draws and vertices

A shape is a list of draws, each one GE draw call: `sceGuDrawArray(GU_TRIANGLES, ...)`
with 16-bit indices. `shift` gives the position unit: a stored position *p* means
*p* x 2^shift twips (the GE reads 16-bit positions as p / 32768, so the renderer folds
32768 x 2^shift into the model matrix). Flags: bit 0 stroke (skipped when the shape
is drawn as a mask), bit 1 smooth (bilinear texture filtering).

- Solid draw (12 bytes per vertex): u8 R, G, B, A; s16 x, y, z (0), pad. Matches
  `GU_COLOR_8888 | GU_VERTEX_16BIT`.
- Textured draw (16 bytes): f32 u, v; s16 x, y, z (0), pad. Matches
  `GU_TEXTURE_32BITF | GU_VERTEX_16BIT`. u and v are in [0,1] over the stored
  (power-of-two) texture; the colour comes from the colour transform.

A draw has at most 65535 vertices and 65535 indices; swf2c splits larger ones.

### Textures

Formats use the GU_PSM values: 0 RGB565, 1 RGBA5551, 2 RGBA4444, 3 RGBA8888,
5 CLUT8 (palette of `clut_count` RGBA8888 entries, a multiple of 16). Channels are
stored R in the low bits, as the GE expects. Colours are straight (not premultiplied).
Width and height are powers of two up to 512, rows are at least 16 bytes, and
`used_w` x `used_h` is the image inside; padding repeats the edge texels. Flags: bit 0
swizzled (GE 16-byte x 8-row blocks; set whenever the height is at least 8), bit 1
repeat (wrap instead of clamp).

Gradients are baked: linear ones to a 256x1 ramp (512x1 for reflect), radial and
focal ones to a square image of the gradient sized to its on-screen diameter (32 to
256). Bitmaps are scaled by the target scale, quantised to CLUT8 when the median-cut
palette reaches 40 dB PSNR (otherwise 565, 5551 or 4444 by alpha content), and split
into 512x512 tiles when larger.

### Sounds

Formats: 1 PCM s16, 2 IMA ADPCM, 3 MP3. IMA data is a sequence of blocks of 1024
frames; per channel a block holds a 4-byte header (i16 predictor, u8 step index, u8 0)
and 512 bytes of 4-bit codes, low nibble first; channels follow each other inside the
block. MP3 is the raw frame stream of a timeline's sound stream, for the Media Engine.
Event sounds are always IMA; streams are MP3 when the SWF stream is MP3, IMA otherwise.

## Timeline commands (`CMDS`)

Each command is one opcode byte followed by its fields, packed (read with byte
loads). Fields marked optional are present only when their flag is set.

| Op | Name | Fields |
|---|---|---|
| 1 | PLACE | u16 flags, u16 depth; optional u16 char_id (flag 0x2), f32 matrix a, b, c, d, tx, ty in twips (0x4), i16 cxform r, g, b, a multiply (8.8) then r, g, b, a add (0x8), u16 ratio (0x10), u32 name string (0x20), u16 clip depth (0x40), u32 first clip action + u16 count (0x80), u8 blend mode (0x200), u8 cache as bitmap (0x400), u8 visible (0x2000) |
| 2 | REMOVE | u16 depth |
| 3 | ACTION | u32 script |
| 4 | START_SOUND | u16 sound char id, u8 event (0 event, 1 start, 2 stop), u8 0, u16 loops, u16 0, u32 in sample or NONE, u32 out sample or NONE |
| 5 | STREAM_BLOCK | u32 position of this frame's block in the timeline's stream, in sample frames |

PLACE flag 0x1 (move) means modify the object at the depth (replacing its character
when one is given); without it, the command places a new object and is ignored if the
depth is occupied, as in Flash. Filters are dropped at build time with a warning.

## Scripts

Script indices number every AVM1 action block in file order (frame actions,
`DoInitAction`, clip event handlers and button actions, sprites inline); see
`swf2c/src/scripts.rs`. Compiled code provides one C function per index.
