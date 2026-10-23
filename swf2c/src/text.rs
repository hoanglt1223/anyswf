//! Fonts and text. Static text (DefineText) is baked into one mesh at its placed size;
//! embedded fonts keep one mesh per glyph for runtime-laid-out dynamic text; device
//! text uses a bundled bitmap font.

use crate::bitmaps::Image;
use crate::characters::{Character, Characters, FontTag, decode_str};
use crate::device_font;
use crate::tessellate::{Cmd, DrawPath, Pt, distill};
use lyon::tessellation::FillRule;
use swf::{Color, FillStyle, ShapeRecord};

pub struct FontData<'a> {
    pub name: String,
    pub bold: bool,
    pub italic: bool,
    /// Font units per em: 1024, or 20480 for DefineFont3.
    pub em: f64,
    pub glyphs: Vec<GlyphData<'a>>,
    pub ascent: u16,
    pub descent: u16,
    pub leading: i16,
}

pub struct GlyphData<'a> {
    pub code: u16,
    pub advance: i16,
    pub records: &'a [ShapeRecord],
}

pub fn font_data<'a>(chars: &Characters<'a>, id: u16) -> Option<FontData<'a>> {
    match chars.get(id)? {
        Character::Font(FontTag::V1(f)) => {
            let info = chars.font_info.get(&id);
            Some(FontData {
                name: info
                    .map(|i| decode_str(i.name, chars.swf_version))
                    .unwrap_or_default(),
                bold: info.is_some_and(|i| i.flags.contains(swf::FontInfoFlag::IS_BOLD)),
                italic: info.is_some_and(|i| i.flags.contains(swf::FontInfoFlag::IS_ITALIC)),
                em: 1024.0,
                glyphs: f
                    .glyphs
                    .iter()
                    .enumerate()
                    .map(|(i, g)| GlyphData {
                        code: info
                            .and_then(|inf| inf.code_table.get(i).copied())
                            .unwrap_or(0),
                        advance: 0,
                        records: g,
                    })
                    .collect(),
                ascent: 0,
                descent: 0,
                leading: 0,
            })
        }
        Character::Font(FontTag::V2(f)) => Some(FontData {
            name: decode_str(f.name, chars.swf_version),
            bold: f.flags.contains(swf::FontFlag::IS_BOLD),
            italic: f.flags.contains(swf::FontFlag::IS_ITALIC),
            em: if f.version >= 3 { 20480.0 } else { 1024.0 },
            glyphs: f
                .glyphs
                .iter()
                .map(|g| GlyphData {
                    code: g.code,
                    advance: g.advance as i16,
                    records: &g.shape_records,
                })
                .collect(),
            ascent: f.layout.as_ref().map_or(0, |l| l.ascent),
            descent: f.layout.as_ref().map_or(0, |l| l.descent),
            leading: f.layout.as_ref().map_or(0, |l| l.leading),
        }),
        _ => None,
    }
}

/// A glyph outline in font units as fill commands.
pub fn glyph_cmds(records: &[ShapeRecord]) -> Vec<Cmd> {
    let style = [FillStyle::Color(Color::WHITE)];
    let mut out = Vec::new();
    for p in distill(records, &style, &[], false) {
        if let DrawPath::Fill { cmds, .. } = p {
            out.extend(cmds);
        }
    }
    out
}

fn map_cmds(cmds: &[Cmd], f: &dyn Fn(Pt) -> Pt) -> Vec<Cmd> {
    cmds.iter()
        .map(|c| match c {
            Cmd::MoveTo(p) => Cmd::MoveTo(f(*p)),
            Cmd::LineTo(p) => Cmd::LineTo(f(*p)),
            Cmd::QuadTo(c1, p) => Cmd::QuadTo(f(*c1), f(*p)),
        })
        .collect()
}

pub struct StaticText {
    pub styles: Vec<FillStyle>,
    /// (index into `styles`, outline of one glyph in character space)
    pub glyphs: Vec<(usize, Vec<Cmd>)>,
    pub missing_fonts: Vec<u16>,
}

impl StaticText {
    pub fn paths(&self) -> Vec<DrawPath<'_>> {
        self.glyphs
            .iter()
            .map(|(s, cmds)| DrawPath::Fill {
                style: &self.styles[*s],
                cmds: cmds.clone(),
                rule: FillRule::EvenOdd,
            })
            .collect()
    }
}

/// Lays out DefineText glyph records into outlines in the text character's space
/// (the text matrix applied).
pub fn static_text(chars: &Characters, text: &swf::Text) -> StaticText {
    let mut out = StaticText {
        styles: Vec::new(),
        glyphs: Vec::new(),
        missing_fonts: Vec::new(),
    };
    let m = &text.matrix;
    let (ma, mb, mc, md) = (m.a.to_f64(), m.b.to_f64(), m.c.to_f64(), m.d.to_f64());
    let (mtx, mty) = (m.tx.get() as f64, m.ty.get() as f64);
    let mut font: Option<FontData> = None;
    let mut font_id = None;
    let mut color = Color::BLACK;
    let mut height = 0f64;
    let (mut x, mut y) = (0f64, 0f64);
    for rec in &text.records {
        if let Some(id) = rec.font_id
            && font_id != Some(id)
        {
            font = font_data(chars, id);
            font_id = Some(id);
            if font.is_none() && !out.missing_fonts.contains(&id) {
                out.missing_fonts.push(id);
            }
        }
        if let Some(c) = rec.color {
            color = c;
        }
        if let Some(h) = rec.height {
            height = h.get() as f64;
        }
        if let Some(dx) = rec.x_offset {
            x = dx.get() as f64;
        }
        if let Some(dy) = rec.y_offset {
            y = dy.get() as f64;
        }
        let style = out
            .styles
            .iter()
            .position(|s| *s == FillStyle::Color(color))
            .unwrap_or_else(|| {
                out.styles.push(FillStyle::Color(color));
                out.styles.len() - 1
            });
        for g in &rec.glyphs {
            if let Some(f) = &font
                && let Some(gd) = f.glyphs.get(g.index as usize)
            {
                let k = height / f.em;
                let (gx, gy) = (x, y);
                let cmds = map_cmds(&glyph_cmds(gd.records), &|p: Pt| {
                    let tx = gx + p.x as f64 * k;
                    let ty = gy + p.y as f64 * k;
                    Pt {
                        x: (ma * tx + mc * ty + mtx).round() as i32,
                        y: (mb * tx + md * ty + mty).round() as i32,
                    }
                });
                if !cmds.is_empty() {
                    out.glyphs.push((style, cmds));
                }
            }
            x += g.advance as f64;
        }
    }
    out
}

pub struct DeviceFont {
    pub atlas: Image,
    pub cols: u32,
    pub advances: Vec<u8>,
}

/// Atlas of the bundled bitmap font: printable ASCII in 8x8 cells, white on transparent.
pub fn device_font() -> DeviceFont {
    let count = device_font::ROWS.len() as u32;
    let cols = 16;
    let rows = count.div_ceil(cols);
    let (w, h) = (cols * device_font::CELL, rows * device_font::CELL);
    let mut px = vec![[0u8; 4]; (w * h) as usize];
    let mut advances = Vec::with_capacity(count as usize);
    for (i, glyph) in device_font::ROWS.iter().enumerate() {
        let (cx, cy) = (
            (i as u32 % cols) * device_font::CELL,
            (i as u32 / cols) * device_font::CELL,
        );
        let mut right = 0;
        for (r, bits) in glyph.iter().enumerate() {
            for c in 0..8u32 {
                if bits >> c & 1 != 0 {
                    px[((cy + r as u32) * w + cx + c) as usize] = [255; 4];
                    right = right.max(c + 1);
                }
            }
        }
        // One pixel of spacing; the space glyph is blank and gets a fixed width.
        advances.push(if right == 0 { 4 } else { right as u8 + 1 });
    }
    DeviceFont {
        atlas: Image::from_pixels(w, h, &px),
        cols,
        advances,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn device_font_atlas_has_glyph_pixels() {
        let f = device_font();
        assert_eq!((f.atlas.width, f.atlas.height), (128, 48));
        assert_eq!(f.advances.len(), 95);
        // 'A' is glyph 33: row 0 is .###.... in its cell.
        let i = (b'A' - device_font::FIRST) as u32;
        let (cx, cy) = ((i % 16) * 8, (i / 16) * 8);
        assert_eq!(f.atlas.pixel(cx, cy), [0; 4]);
        assert_eq!(f.atlas.pixel(cx + 1, cy), [255; 4]);
        assert_eq!(f.advances[i as usize], 6);
    }
}
