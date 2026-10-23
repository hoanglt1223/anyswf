//! `assets.bin` writer. The format is specified in docs/dev/assets-bin-format.md and
//! loaded by runtime/src/assets.c; keep the three in step and bump `VERSION` on any
//! layout change.

pub const MAGIC: &[u8; 4] = b"ASWF";
pub const VERSION: u16 = 1;
pub const HEADER_SIZE: usize = 64;
pub const NONE: u32 = u32::MAX;

pub mod kind {
    pub const NONE: u8 = 0;
    pub const SHAPE: u8 = 1;
    pub const MORPH: u8 = 2;
    pub const SPRITE: u8 = 3;
    pub const BUTTON: u8 = 4;
    pub const TEXT: u8 = 5;
    pub const EDIT_TEXT: u8 = 6;
    pub const FONT: u8 = 7;
    pub const BITMAP: u8 = 8;
    pub const SOUND: u8 = 9;
}

pub mod cmd {
    pub const PLACE: u8 = 1;
    pub const REMOVE: u8 = 2;
    pub const ACTION: u8 = 3;
    pub const START_SOUND: u8 = 4;
    pub const STREAM_BLOCK: u8 = 5;
}

/// PLACE command field flags.
pub mod place {
    pub const MOVE: u16 = 1 << 0;
    pub const CHARACTER: u16 = 1 << 1;
    pub const MATRIX: u16 = 1 << 2;
    pub const CXFORM: u16 = 1 << 3;
    pub const RATIO: u16 = 1 << 4;
    pub const NAME: u16 = 1 << 5;
    pub const CLIP_DEPTH: u16 = 1 << 6;
    pub const CLIP_ACTIONS: u16 = 1 << 7;
    pub const BLEND: u16 = 1 << 9;
    pub const CACHE: u16 = 1 << 10;
    pub const VISIBLE: u16 = 1 << 13;
}

pub mod draw_flag {
    pub const STROKE: u8 = 1 << 0;
    pub const SMOOTH: u8 = 1 << 1;
}

pub mod tex_flag {
    pub const SWIZZLED: u8 = 1 << 0;
    pub const REPEAT: u8 = 1 << 1;
}

pub mod sound_format {
    pub const PCM16: u8 = 1;
    pub const IMA4: u8 = 2;
    pub const MP3: u8 = 3;
}

pub const SOLID_VERTEX_SIZE: usize = 12;
pub const TEX_VERTEX_SIZE: usize = 16;

#[derive(Clone, Copy, Default)]
pub struct CharEntry {
    pub kind: u8,
    pub index: u32,
}

#[derive(Clone, Copy)]
pub struct ShapeRec {
    pub bounds: [i32; 4],
    pub first_draw: u32,
    pub draw_count: u32,
}

#[derive(Clone, Copy)]
pub struct DrawRec {
    pub textured: bool,
    pub flags: u8,
    pub shift: u8,
    pub texture: u32,
    pub vtx_off: u32,
    pub vtx_count: u32,
    pub idx_off: u32,
    pub idx_count: u32,
}

#[derive(Clone, Copy)]
pub struct TexRec {
    pub format: u8,
    pub flags: u8,
    pub width: u16,
    pub height: u16,
    pub used_w: u16,
    pub used_h: u16,
    pub clut_count: u16,
    pub data_off: u32,
    pub data_size: u32,
    pub clut_off: u32,
}

#[derive(Clone, Copy)]
pub struct SpriteRec {
    pub frame_count: u32,
    pub frame_table: u32,
    pub label_first: u32,
    pub label_count: u32,
    pub stream: u32,
}

#[derive(Clone, Copy)]
pub struct ButtonRec {
    pub first_record: u32,
    pub record_count: u16,
    pub flags: u16,
    pub first_action: u32,
    pub action_count: u32,
}

#[derive(Clone, Copy)]
pub struct ButtonRecord {
    pub states: u8,
    pub blend: u8,
    pub char_id: u16,
    pub depth: u16,
    pub matrix: [f32; 6],
    pub cxform: [i16; 8],
}

#[derive(Clone, Copy)]
pub struct EditRec {
    pub bounds: [i32; 4],
    pub font_id: u16,
    pub height: u16,
    pub color: u32,
    pub max_length: u16,
    pub flags: u16,
    pub align: u8,
    pub left_margin: u16,
    pub right_margin: u16,
    pub indent: u16,
    pub leading: i16,
    pub var_name: u32,
    pub initial_text: u32,
}

#[derive(Clone, Copy)]
pub struct FontRec {
    pub name: u32,
    pub flags: u16,
    pub glyph_count: u16,
    pub first_glyph: u32,
    pub ascent: u16,
    pub descent: u16,
    pub leading: i16,
    pub em: u16,
}

#[derive(Clone, Copy)]
pub struct SoundRec {
    pub format: u8,
    pub channels: u8,
    pub rate: u32,
    pub frames: u32,
    pub data_off: u32,
    pub data_size: u32,
}

/// (texture, first char, count, cell, glyph height, atlas columns, advances)
pub type DeviceFontRec = (u32, u16, u16, u16, u16, u16, Vec<u8>);

#[derive(Clone, Copy)]
pub struct ScriptRec {
    pub kind: u8,
    pub owner: u16,
    pub size: u32,
}

#[derive(Default)]
pub struct Assets {
    pub stage: [i32; 4],
    pub frame_rate: u16,
    pub swf_version: u16,
    pub background: u32,
    pub target_scale: f32,
    /// Uncompressed SWF length (MovieClip.getBytesTotal on the root).
    pub swf_size: u32,
    pub chars: Vec<CharEntry>,
    pub shapes: Vec<ShapeRec>,
    pub draws: Vec<DrawRec>,
    pub textures: Vec<TexRec>,
    pub morphs: Vec<(u32, u32)>,
    pub morph_frames: Vec<(u16, u32)>,
    pub sprites: Vec<SpriteRec>,
    pub frames: Vec<u32>,
    pub cmds: Vec<u8>,
    pub labels: Vec<(u32, u32)>,
    pub buttons: Vec<ButtonRec>,
    pub button_records: Vec<ButtonRecord>,
    pub button_actions: Vec<(u16, u16, u32)>,
    pub clip_actions: Vec<(u32, u8, u32)>,
    pub edits: Vec<EditRec>,
    pub fonts: Vec<FontRec>,
    pub glyphs: Vec<(u16, i16, u32)>,
    pub device_font: Option<DeviceFontRec>,
    pub sounds: Vec<SoundRec>,
    pub exports: Vec<(u16, u32)>,
    pub inits: Vec<(u16, u32)>,
    pub scripts: Vec<ScriptRec>,
    pub strings: Strings,
    pub data: Vec<u8>,
}

/// NUL-terminated UTF-8 strings; offset 0 is the empty string.
pub struct Strings {
    pub blob: Vec<u8>,
    index: std::collections::HashMap<String, u32>,
}

impl Default for Strings {
    fn default() -> Self {
        Strings {
            blob: vec![0],
            index: Default::default(),
        }
    }
}

impl Strings {
    pub fn intern(&mut self, s: &str) -> u32 {
        if s.is_empty() {
            return 0;
        }
        if let Some(&o) = self.index.get(s) {
            return o;
        }
        let o = self.blob.len() as u32;
        // Interior NULs would truncate the string in C; Flash strings cannot hold them anyway.
        self.blob.extend(s.bytes().filter(|&b| b != 0));
        self.blob.push(0);
        self.index.insert(s.to_owned(), o);
        o
    }
}

impl Assets {
    /// Appends 16-byte-aligned data and returns its offset in the DATA block.
    pub fn push_data(&mut self, bytes: &[u8]) -> u32 {
        while !self.data.len().is_multiple_of(16) {
            self.data.push(0);
        }
        let o = self.data.len() as u32;
        self.data.extend_from_slice(bytes);
        o
    }
}

struct Out {
    buf: Vec<u8>,
}

impl Out {
    fn u8(&mut self, v: u8) {
        self.buf.push(v);
    }
    fn u16(&mut self, v: u16) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }
    fn i16(&mut self, v: i16) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }
    fn u32(&mut self, v: u32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }
    fn i32(&mut self, v: i32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }
    fn f32(&mut self, v: f32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }
}

fn block<F: FnOnce(&mut Out)>(tag: &[u8; 4], count: usize, f: F) -> ([u8; 4], u32, Vec<u8>) {
    let mut o = Out { buf: Vec::new() };
    f(&mut o);
    (*tag, count as u32, o.buf)
}

pub fn serialize(a: &Assets) -> Vec<u8> {
    let mut blocks = vec![
        block(b"CHAR", a.chars.len(), |o| {
            for c in &a.chars {
                o.u8(c.kind);
                o.u8(0);
                o.u16(0);
                o.u32(c.index);
            }
        }),
        block(b"SHAP", a.shapes.len(), |o| {
            for s in &a.shapes {
                s.bounds.iter().for_each(|&v| o.i32(v));
                o.u32(s.first_draw);
                o.u32(s.draw_count);
            }
        }),
        block(b"DRAW", a.draws.len(), |o| {
            for d in &a.draws {
                o.u8(d.textured as u8);
                o.u8(d.flags);
                o.u8(d.shift);
                o.u8(0);
                o.u32(d.texture);
                o.u32(d.vtx_off);
                o.u32(d.vtx_count);
                o.u32(d.idx_off);
                o.u32(d.idx_count);
                o.u32(0);
                o.u32(0);
            }
        }),
        block(b"TEXR", a.textures.len(), |o| {
            for t in &a.textures {
                o.u8(t.format);
                o.u8(t.flags);
                o.u16(t.width);
                o.u16(t.height);
                o.u16(t.used_w);
                o.u16(t.used_h);
                o.u16(t.clut_count);
                o.u32(t.data_off);
                o.u32(t.data_size);
                o.u32(t.clut_off);
                o.u32(0);
                o.u32(0);
            }
        }),
        block(b"MRPH", a.morphs.len(), |o| {
            for &(first, count) in &a.morphs {
                o.u32(first);
                o.u32(count);
            }
        }),
        block(b"MRPF", a.morph_frames.len(), |o| {
            for &(ratio, shape) in &a.morph_frames {
                o.u16(ratio);
                o.u16(0);
                o.u32(shape);
            }
        }),
        block(b"SPRT", a.sprites.len(), |o| {
            for s in &a.sprites {
                o.u32(s.frame_count);
                o.u32(s.frame_table);
                o.u32(s.label_first);
                o.u32(s.label_count);
                o.u32(s.stream);
                o.u32(0);
            }
        }),
        block(b"FRMS", a.frames.len(), |o| {
            a.frames.iter().for_each(|&f| o.u32(f))
        }),
        block(b"CMDS", a.cmds.len(), |o| o.buf.extend_from_slice(&a.cmds)),
        block(b"LABL", a.labels.len(), |o| {
            for &(name, frame) in &a.labels {
                o.u32(name);
                o.u32(frame);
            }
        }),
        block(b"BTTN", a.buttons.len(), |o| {
            for b in &a.buttons {
                o.u32(b.first_record);
                o.u16(b.record_count);
                o.u16(b.flags);
                o.u32(b.first_action);
                o.u32(b.action_count);
            }
        }),
        block(b"BTRC", a.button_records.len(), |o| {
            for r in &a.button_records {
                o.u8(r.states);
                o.u8(r.blend);
                o.u16(r.char_id);
                o.u16(r.depth);
                o.u16(0);
                r.matrix.iter().for_each(|&v| o.f32(v));
                r.cxform.iter().for_each(|&v| o.i16(v));
            }
        }),
        block(b"BTAC", a.button_actions.len(), |o| {
            for &(cond, key, script) in &a.button_actions {
                o.u16(cond);
                o.u16(key);
                o.u32(script);
            }
        }),
        block(b"CLAC", a.clip_actions.len(), |o| {
            for &(events, key, script) in &a.clip_actions {
                o.u32(events);
                o.u8(key);
                o.u8(0);
                o.u16(0);
                o.u32(script);
            }
        }),
        block(b"EDIT", a.edits.len(), |o| {
            for e in &a.edits {
                e.bounds.iter().for_each(|&v| o.i32(v));
                o.u16(e.font_id);
                o.u16(e.height);
                o.u32(e.color);
                o.u16(e.max_length);
                o.u16(e.flags);
                o.u8(e.align);
                o.u8(0);
                o.u16(e.left_margin);
                o.u16(e.right_margin);
                o.u16(e.indent);
                o.i16(e.leading);
                o.u16(0);
                o.u32(e.var_name);
                o.u32(e.initial_text);
            }
        }),
        block(b"FONT", a.fonts.len(), |o| {
            for f in &a.fonts {
                o.u32(f.name);
                o.u16(f.flags);
                o.u16(f.glyph_count);
                o.u32(f.first_glyph);
                o.u16(f.ascent);
                o.u16(f.descent);
                o.i16(f.leading);
                o.u16(f.em);
                o.u32(0);
            }
        }),
        block(b"GLYF", a.glyphs.len(), |o| {
            for &(code, adv, shape) in &a.glyphs {
                o.u16(code);
                o.i16(adv);
                o.u32(shape);
                o.u32(0);
            }
        }),
        block(b"SOND", a.sounds.len(), |o| {
            for s in &a.sounds {
                o.u8(s.format);
                o.u8(s.channels);
                o.u16(0);
                o.u32(s.rate);
                o.u32(s.frames);
                o.u32(s.data_off);
                o.u32(s.data_size);
                o.u32(0);
            }
        }),
        block(b"EXPT", a.exports.len(), |o| {
            for &(id, name) in &a.exports {
                o.u16(id);
                o.u16(0);
                o.u32(name);
            }
        }),
        block(b"INIT", a.inits.len(), |o| {
            for &(id, script) in &a.inits {
                o.u16(id);
                o.u16(0);
                o.u32(script);
            }
        }),
        block(b"SCRP", a.scripts.len(), |o| {
            for s in &a.scripts {
                o.u8(s.kind);
                o.u8(0);
                o.u16(s.owner);
                o.u32(s.size);
            }
        }),
        block(b"STRS", a.strings.blob.len(), |o| {
            o.buf.extend_from_slice(&a.strings.blob)
        }),
        block(b"DATA", a.data.len(), |o| o.buf.extend_from_slice(&a.data)),
    ];
    if let Some((tex, first, count, cell, glyph_h, cols, adv)) = &a.device_font {
        blocks.push(block(b"DFNT", 1, |o| {
            o.u32(*tex);
            o.u16(*first);
            o.u16(*count);
            o.u16(*cell);
            o.u16(*glyph_h);
            o.u16(*cols);
            o.u16(0);
            o.buf.extend_from_slice(adv);
        }));
    }

    let dir_size = blocks.len() * 16;
    let mut off = align(HEADER_SIZE + dir_size, 64);
    let mut dir = Vec::new();
    for (tag, count, data) in &blocks {
        dir.push((*tag, off as u32, data.len() as u32, *count));
        off = align(off + data.len(), 64);
    }
    let total = off;

    let mut o = Out {
        buf: Vec::with_capacity(total),
    };
    o.buf.extend_from_slice(MAGIC);
    o.u16(VERSION);
    o.u16(HEADER_SIZE as u16);
    o.u32(total as u32);
    o.u32(blocks.len() as u32);
    a.stage.iter().for_each(|&v| o.i32(v));
    o.u16(a.frame_rate);
    o.u16(a.swf_version);
    o.u32(a.background);
    o.f32(a.target_scale);
    o.u32(a.swf_size);
    for _ in 0..4 {
        o.u32(0);
    }
    debug_assert_eq!(o.buf.len(), HEADER_SIZE);
    for (tag, offset, size, count) in &dir {
        o.buf.extend_from_slice(tag);
        o.u32(*offset);
        o.u32(*size);
        o.u32(*count);
    }
    for ((_, _, data), (_, offset, _, _)) in blocks.iter().zip(&dir) {
        o.buf.resize(*offset as usize, 0);
        o.buf.extend_from_slice(data);
    }
    o.buf.resize(total, 0);
    o.buf
}

fn align(v: usize, a: usize) -> usize {
    v.div_ceil(a) * a
}

/// Minimal reader used by tests and `swf2c inspect-assets`: block directory lookup.
pub fn find_block<'b>(file: &'b [u8], tag: &[u8; 4]) -> Option<(&'b [u8], u32)> {
    if file.len() < HEADER_SIZE || &file[..4] != MAGIC {
        return None;
    }
    let n = u32::from_le_bytes(file[12..16].try_into().ok()?) as usize;
    for i in 0..n {
        let e = file.get(HEADER_SIZE + i * 16..HEADER_SIZE + i * 16 + 16)?;
        if &e[..4] == tag {
            let off = u32::from_le_bytes(e[4..8].try_into().ok()?) as usize;
            let size = u32::from_le_bytes(e[8..12].try_into().ok()?) as usize;
            let count = u32::from_le_bytes(e[12..16].try_into().ok()?);
            return Some((file.get(off..off + size)?, count));
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_directory_and_alignment() {
        let mut a = Assets {
            stage: [0, 0, 9600, 5440],
            frame_rate: 30 << 8,
            swf_version: 8,
            ..Default::default()
        };
        let o1 = a.push_data(&[1, 2, 3]);
        let o2 = a.push_data(&[4]);
        assert_eq!((o1, o2), (0, 16));
        let s = a.strings.intern("hello");
        assert_eq!(a.strings.intern("hello"), s);
        assert_eq!(a.strings.intern(""), 0);
        let bin = serialize(&a);
        assert_eq!(&bin[..4], MAGIC);
        assert_eq!(
            u32::from_le_bytes(bin[8..12].try_into().unwrap()) as usize,
            bin.len()
        );
        assert_eq!(bin.len() % 64, 0);
        let (data, n) = find_block(&bin, b"DATA").unwrap();
        assert_eq!(n, 17);
        assert_eq!(&data[16..17], &[4]);
        let (strs, _) = find_block(&bin, b"STRS").unwrap();
        assert_eq!(&strs[s as usize..s as usize + 6], b"hello\0");
        for i in 0..u32::from_le_bytes(bin[12..16].try_into().unwrap()) as usize {
            let off = u32::from_le_bytes(bin[64 + i * 16 + 4..64 + i * 16 + 8].try_into().unwrap());
            assert_eq!(off % 64, 0);
        }
    }
}
