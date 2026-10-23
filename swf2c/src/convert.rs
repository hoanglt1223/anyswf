//! SWF to `Assets`: runs every converter and lays the results out in assets.bin records.

use crate::assets_bin::{
    self as ab, Assets, ButtonRec, ButtonRecord, CharEntry, DrawRec, EditRec, FontRec, ScriptRec,
    ShapeRec, SoundRec, SpriteRec, TexRec, kind,
};
use crate::bitmaps::{self, Converted, Texture};
use crate::characters::{BitmapTag, Character, Characters, FontTag, decode_str};
use crate::gradients::{self, GradientKey, Wrap};
use crate::scripts::Scripts;
use crate::tessellate::{self, DrawPath, Paint, RawDraw, TessOptions, TessStats};
use crate::{Error, Result, morph, sounds, text};
use lyon::tessellation::FillRule;
use std::collections::{BTreeMap, BTreeSet, HashMap};
use swf::{FillStyle, Tag};

pub const SCREEN_W: f32 = 480.0;
pub const SCREEN_H: f32 = 272.0;
/// Glyph meshes for runtime text are tessellated for roughly this text height in pixels.
const GLYPH_REF_PX: f64 = 24.0;

pub struct Options {
    /// Maximum curve flattening error in screen pixels at the stage scale.
    pub tolerance_px: f32,
}

impl Default for Options {
    fn default() -> Self {
        Options { tolerance_px: 0.35 }
    }
}

#[derive(Default, Debug)]
pub struct Stats {
    pub triangles: usize,
    pub draws: usize,
    pub texture_bytes: usize,
    pub sound_bytes: usize,
}

pub struct Output {
    pub assets: Assets,
    pub warnings: Vec<String>,
    pub stats: Stats,
}

struct BitmapEntry {
    conv: Converted,
    first_texture: u32,
}

struct Conv<'a> {
    chars: &'a Characters<'a>,
    scripts: &'a Scripts<'a>,
    a: Assets,
    tol_twips: f32,
    min_stroke: f32,
    bitmaps: HashMap<(u16, bool), BitmapEntry>,
    decoded: HashMap<u16, Option<bitmaps::Image>>,
    gradient_cache: HashMap<(GradientKey, usize), (u32, f32)>,
    sprite_index: HashMap<u16, u32>,
    scales: HashMap<u16, (f32, f32)>,
    warnings: Vec<String>,
    warned: BTreeSet<String>,
    stats: Stats,
}

pub fn target_scale(stage: &swf::Rectangle<swf::Twips>) -> f32 {
    let w = (stage.x_max - stage.x_min).to_pixels() as f32;
    let h = (stage.y_max - stage.y_min).to_pixels() as f32;
    if w <= 0.0 || h <= 0.0 {
        return 1.0;
    }
    (SCREEN_W / w).min(SCREEN_H / h)
}

pub fn convert(swf: &swf::Swf, opts: &Options) -> Result<Output> {
    let version = swf.header.version();
    let chars = Characters::collect(version, &swf.tags);
    let scripts = Scripts::collect(&swf.tags);
    let stage = *swf.header.stage_size();
    let scale = target_scale(&stage);
    let mut c = Conv {
        chars: &chars,
        scripts: &scripts,
        a: Assets::default(),
        tol_twips: opts.tolerance_px * 20.0 / scale,
        min_stroke: 20.0 / scale,
        bitmaps: HashMap::new(),
        decoded: HashMap::new(),
        gradient_cache: HashMap::new(),
        sprite_index: HashMap::new(),
        scales: HashMap::new(),
        warnings: chars.warnings.clone(),
        warned: BTreeSet::new(),
        stats: Stats::default(),
    };
    let bg = swf.header.background_color().unwrap_or(swf::Color::WHITE);
    c.a.stage = tessellate::twips_rect(&stage);
    c.a.frame_rate = swf.header.frame_rate().get() as u16;
    c.a.swf_version = version as u16;
    c.a.background = u32::from_le_bytes([bg.r, bg.g, bg.b, 255]);
    c.a.target_scale = scale;
    c.a.swf_size = swf.header.uncompressed_len().max(0) as u32;
    let max_id = chars.map.keys().next_back().copied().unwrap_or(0) as usize;
    c.a.chars = vec![CharEntry::default(); max_id + 1];

    // Sprite 0 is the root timeline; the others follow in character id order.
    let mut n = 1;
    for (id, ch) in &chars.map {
        if matches!(ch, Character::Sprite(_)) {
            c.sprite_index.insert(*id, n);
            n += 1;
        }
    }
    let ratios = c.morph_ratios(&swf.tags);
    c.scales = placement_scales(&chars, &swf.tags);

    for (&id, ch) in &chars.map {
        let entry = match ch {
            Character::Shape(sh) => {
                let paths = tessellate::distill(
                    &sh.shape,
                    &sh.styles.fill_styles,
                    &sh.styles.line_styles,
                    sh.flags.contains(swf::ShapeFlag::NON_ZERO_WINDING_RULE),
                );
                let idx = c.add_mesh(&paths, Some(tessellate::twips_rect(&sh.shape_bounds)), id);
                Some((kind::SHAPE, idx))
            }
            Character::Morph(m) => {
                let first = c.a.morph_frames.len() as u32;
                let list = ratios
                    .get(&id)
                    .cloned()
                    .unwrap_or_else(|| BTreeSet::from([0]));
                for r in &list {
                    let f = morph::frame(m, *r);
                    let paths =
                        tessellate::distill(&f.records, &f.fill_styles, &f.line_styles, false);
                    let s = c.add_mesh(&paths, None, id);
                    c.a.morph_frames.push((*r, s));
                }
                c.a.morphs.push((first, list.len() as u32));
                Some((kind::MORPH, c.a.morphs.len() as u32 - 1))
            }
            Character::Sprite(_) => Some((kind::SPRITE, c.sprite_index[&id])),
            Character::Text(t, _) => {
                let st = text::static_text(&chars, t);
                for f in &st.missing_fonts {
                    c.warn(format!("text {id} uses missing font {f}"));
                }
                let idx = c.add_mesh(&st.paths(), Some(tessellate::twips_rect(&t.bounds)), id);
                Some((kind::TEXT, idx))
            }
            Character::Font(f) => c.add_font(id, f).map(|i| (kind::FONT, i)),
            Character::EditText(e) => Some((kind::EDIT_TEXT, c.add_edit(e))),
            Character::Button(b, v) => Some((kind::BUTTON, c.add_button(b, *v))),
            Character::Sound(s) => c.add_event_sound(s).map(|i| (kind::SOUND, i)),
            // Assigned after all meshes, so the texture variant fills already used is reused.
            Character::Bitmap(_) => None,
            Character::BinaryData => None,
            Character::Video(_) => {
                c.warn(format!("video {id} ignored: video is not supported"));
                None
            }
        };
        if let Some((k, index)) = entry {
            c.a.chars[id as usize] = CharEntry { kind: k, index };
        }
    }

    let bitmap_ids: Vec<u16> = chars
        .map
        .iter()
        .filter(|(_, ch)| matches!(ch, Character::Bitmap(_)))
        .map(|(id, _)| *id)
        .collect();
    for id in bitmap_ids {
        let existing = [false, true]
            .iter()
            .find_map(|r| c.bitmaps.get(&(id, *r)).map(|b| b.first_texture));
        let tex = match existing {
            Some(t) => Some(t),
            None => c.bitmap(id, false).map(|b| b.first_texture),
        };
        if let Some(index) = tex {
            c.a.chars[id as usize] = CharEntry {
                kind: kind::BITMAP,
                index,
            };
        }
    }

    // Timelines: root first, then sprites in index order.
    c.a.sprites = vec![
        SpriteRec {
            frame_count: 0,
            frame_table: 0,
            label_first: 0,
            label_count: 0,
            stream: ab::NONE
        };
        n as usize
    ];
    c.add_timeline(0, &swf.tags, swf.header.num_frames());
    for ch in chars.map.values() {
        if let Character::Sprite(sp) = ch {
            c.add_timeline(c.sprite_index[&sp.id], &sp.tags, sp.num_frames);
        }
    }

    for (id, name) in &chars.exports {
        let s = c.a.strings.intern(name);
        c.a.exports.push((*id, s));
    }
    for (id, bytes) in &chars.init_actions {
        let s = scripts.index_of(bytes).unwrap_or(ab::NONE);
        c.a.inits.push((*id, s));
    }
    for s in &scripts.list {
        c.a.scripts.push(ScriptRec {
            kind: s.kind,
            owner: s.owner,
            size: s.bytes.len() as u32,
        });
    }
    c.add_device_font();

    Ok(Output {
        assets: c.a,
        warnings: c.warnings,
        stats: c.stats,
    })
}

fn head(textured: bool, flags: u8, shift: u8, texture: u32) -> DrawRec {
    DrawRec {
        textured,
        flags,
        shift,
        texture,
        vtx_off: 0,
        vtx_count: 0,
        idx_off: 0,
        idx_count: 0,
    }
}

fn matrix_scale(m: &swf::Matrix) -> f32 {
    let sx = (m.a.to_f32().powi(2) + m.b.to_f32().powi(2)).sqrt();
    let sy = (m.c.to_f32().powi(2) + m.d.to_f32().powi(2)).sqrt();
    sx.max(sy)
}

/// Smallest and largest on-stage scale of every character, through nested sprites and
/// buttons, from the placement matrices in the timelines. Unplaced characters (attached
/// from script) are absent and default to 1.
fn placement_scales(chars: &Characters, root: &[Tag]) -> HashMap<u16, (f32, f32)> {
    // (parent, child, scale); parent 0 is the root timeline.
    let mut edges: Vec<(u16, u16, f32)> = Vec::new();
    fn walk(edges: &mut Vec<(u16, u16, f32)>, parent: u16, tags: &[Tag]) {
        let mut depth: HashMap<u16, u16> = HashMap::new();
        for t in tags {
            if let Tag::PlaceObject(p) = t {
                let id = match p.action {
                    swf::PlaceObjectAction::Place(id) | swf::PlaceObjectAction::Replace(id) => {
                        depth.insert(p.depth, id);
                        Some(id)
                    }
                    swf::PlaceObjectAction::Modify => depth.get(&p.depth).copied(),
                };
                if let Some(id) = id {
                    let s = p.matrix.as_ref().map_or(1.0, matrix_scale);
                    edges.push((parent, id, s));
                }
            }
        }
    }
    walk(&mut edges, 0, root);
    for (id, ch) in &chars.map {
        match ch {
            Character::Sprite(sp) => walk(&mut edges, *id, &sp.tags),
            Character::Button(b, _) => {
                for r in &b.records {
                    edges.push((*id, r.id, matrix_scale(&r.matrix)));
                }
            }
            _ => {}
        }
    }
    let mut out: HashMap<u16, (f32, f32)> = HashMap::new();
    for _ in 0..16 {
        let mut changed = false;
        for &(parent, child, s) in &edges {
            let (plo, phi) = if parent == 0 {
                (1.0, 1.0)
            } else {
                out.get(&parent).copied().unwrap_or((1.0, 1.0))
            };
            let (lo, hi) = (
                (plo * s).clamp(1.0 / 32.0, 64.0),
                (phi * s).clamp(1.0 / 32.0, 64.0),
            );
            let e = out.entry(child).or_insert((lo, hi));
            let next = (e.0.min(lo), e.1.max(hi));
            if next != *e {
                *e = next;
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    out
}

fn shift_for(max_abs: f32) -> u8 {
    let mut s = 0u8;
    while max_abs / (1u32 << s) as f32 > 32767.0 && s < 16 {
        s += 1;
    }
    s
}

fn q16(v: f32, shift: u8) -> i16 {
    (v / (1u32 << shift) as f32)
        .round()
        .clamp(-32768.0, 32767.0) as i16
}

/// Splits a triangle list so that each part has at most 65535 vertices and indices.
fn split_u16(vertex_count: usize, idx: &[u32]) -> Vec<(Vec<u32>, Vec<u16>)> {
    if vertex_count <= 65535 && idx.len() <= 65535 {
        return vec![(
            (0..vertex_count as u32).collect(),
            idx.iter().map(|&i| i as u16).collect(),
        )];
    }
    let mut out = Vec::new();
    let mut map: HashMap<u32, u16> = HashMap::new();
    let mut verts = Vec::new();
    let mut ind: Vec<u16> = Vec::new();
    for tri in idx.chunks_exact(3) {
        let new = tri.iter().filter(|v| !map.contains_key(v)).count();
        if verts.len() + new > 65535 || ind.len() + 3 > 65535 {
            out.push((std::mem::take(&mut verts), std::mem::take(&mut ind)));
            map.clear();
        }
        for &v in tri {
            let i = *map.entry(v).or_insert_with(|| {
                verts.push(v);
                (verts.len() - 1) as u16
            });
            ind.push(i);
        }
    }
    if !ind.is_empty() {
        out.push((verts, ind));
    }
    out
}

/// Clips triangles (positions with affine UVs in image pixels) to an axis-aligned UV rect.
fn clip_uv(
    pos: &[[f32; 2]],
    uv: &[[f32; 2]],
    idx: &[u32],
    r: [f32; 4],
) -> (Vec<[f32; 2]>, Vec<[f32; 2]>, Vec<u32>) {
    let (mut op, mut ou, mut oi) = (Vec::new(), Vec::new(), Vec::new());
    for tri in idx.chunks_exact(3) {
        let mut poly: Vec<([f32; 2], [f32; 2])> = tri
            .iter()
            .map(|&i| (pos[i as usize], uv[i as usize]))
            .collect();
        for (axis, bound, keep_greater) in [
            (0, r[0], true),
            (0, r[2], false),
            (1, r[1], true),
            (1, r[3], false),
        ] {
            let inside = |v: &([f32; 2], [f32; 2])| {
                if keep_greater {
                    v.1[axis] >= bound
                } else {
                    v.1[axis] <= bound
                }
            };
            let mut next = Vec::new();
            for k in 0..poly.len() {
                let a = poly[k];
                let b = poly[(k + 1) % poly.len()];
                let (ia, ib) = (inside(&a), inside(&b));
                if ia {
                    next.push(a);
                }
                if ia != ib {
                    let t = (bound - a.1[axis]) / (b.1[axis] - a.1[axis]);
                    let lerp = |x: [f32; 2], y: [f32; 2]| {
                        [x[0] + (y[0] - x[0]) * t, x[1] + (y[1] - x[1]) * t]
                    };
                    next.push((lerp(a.0, b.0), lerp(a.1, b.1)));
                }
            }
            poly = next;
            if poly.len() < 3 {
                break;
            }
        }
        if poly.len() < 3 {
            continue;
        }
        let base = op.len() as u32;
        for (p, u) in &poly {
            op.push(*p);
            ou.push(*u);
        }
        for k in 1..poly.len() as u32 - 1 {
            oi.extend_from_slice(&[base, base + k, base + k + 1]);
        }
    }
    (op, ou, oi)
}

impl<'a> Conv<'a> {
    fn warn(&mut self, msg: String) {
        if self.warned.insert(msg.clone()) {
            self.warnings.push(msg);
        }
    }

    fn add_texture(&mut self, t: &Texture) -> u32 {
        let data_off = self.a.push_data(&t.data);
        let clut_off = if t.clut.is_empty() {
            0
        } else {
            self.a.push_data(&t.clut)
        };
        let mut flags = 0;
        if t.swizzled {
            flags |= ab::tex_flag::SWIZZLED;
        }
        if t.wrap == Wrap::Repeat {
            flags |= ab::tex_flag::REPEAT;
        }
        self.a.textures.push(TexRec {
            format: t.format as u8,
            flags,
            width: t.width as u16,
            height: t.height as u16,
            used_w: t.used_w as u16,
            used_h: t.used_h as u16,
            clut_count: (t.clut.len() / 4) as u16,
            data_off,
            data_size: t.data.len() as u32,
            clut_off,
        });
        self.stats.texture_bytes += t.data.len() + t.clut.len();
        self.a.textures.len() as u32 - 1
    }

    fn decode_bitmap(&mut self, id: u16) -> Option<bitmaps::Image> {
        if !self.decoded.contains_key(&id) {
            let decoded = match self.chars.get(id) {
                Some(Character::Bitmap(b)) => match b {
                    BitmapTag::Jpeg1(d) => bitmaps::decode_jpeg1(d, self.chars.jpeg_tables),
                    BitmapTag::Jpeg2(d) => bitmaps::decode_jpeg_tag(d, None),
                    BitmapTag::Jpeg3(j) => bitmaps::decode_jpeg_tag(j.data, Some(j.alpha_data)),
                    BitmapTag::Lossless(l) => bitmaps::decode_lossless(l),
                },
                _ => Err(Error::new("no such bitmap character")),
            };
            let img = match decoded {
                Ok(img) => Some(img),
                Err(e) => {
                    self.warn(format!("bitmap {id}: {e}; fills using it are dropped"));
                    None
                }
            };
            self.decoded.insert(id, img);
        }
        self.decoded.get(&id).cloned().flatten()
    }

    /// The texture set for a bitmap: padded and clamped, or (`repeat`) resampled to a
    /// power of two so the GE can wrap it. Each variant is converted once, on first use.
    fn bitmap(&mut self, id: u16, repeat: bool) -> Option<&BitmapEntry> {
        if !self.bitmaps.contains_key(&(id, repeat)) {
            let img = self.decode_bitmap(id)?;
            let conv = bitmaps::convert(&img, self.a.target_scale, repeat);
            let first_texture = self.a.textures.len() as u32;
            for t in &conv.tiles {
                self.add_texture(&t.texture);
            }
            self.bitmaps.insert(
                (id, repeat),
                BitmapEntry {
                    conv,
                    first_texture,
                },
            );
        }
        self.bitmaps.get(&(id, repeat))
    }

    fn gradient_texture(&mut self, style: &FillStyle, owner: u16) -> Option<(u32, f32)> {
        let (key, g) = gradients::key(style)?;
        let size = if matches!(style, FillStyle::LinearGradient(_)) {
            0
        } else {
            let hi = self.scales.get(&owner).map_or(1.0, |s| s.1);
            let screen = 32768.0 / 20.0 * matrix_scale(&g.matrix) * self.a.target_scale * hi;
            gradients::radial_size(screen)
        };
        if let Some(v) = self.gradient_cache.get(&(key.clone(), size)) {
            return Some(*v);
        }
        let baked = gradients::bake(&key, size);
        let tex = bitmaps::encode(&baked.image, false, baked.wrap, None);
        let idx = self.add_texture(&tex);
        self.gradient_cache
            .insert((key, size), (idx, baked.u_scale));
        Some((idx, baked.u_scale))
    }

    /// Tolerance follows the largest scale the character is shown at, the hairline width
    /// the smallest, so curves stay smooth and thin strokes stay visible.
    fn add_mesh(&mut self, paths: &[DrawPath], bounds: Option<[i32; 4]>, owner: u16) -> u32 {
        let (lo, hi) = self.scales.get(&owner).copied().unwrap_or((1.0, 1.0));
        self.add_mesh_tol(
            paths,
            bounds,
            owner,
            self.tol_twips / hi,
            self.min_stroke / lo,
        )
    }

    fn add_mesh_tol(
        &mut self,
        paths: &[DrawPath],
        bounds: Option<[i32; 4]>,
        owner: u16,
        tol: f32,
        min_stroke: f32,
    ) -> u32 {
        let mut st = TessStats::default();
        let raw = tessellate::tessellate(
            paths,
            &TessOptions {
                tolerance: tol,
                min_stroke,
            },
            &mut st,
        );
        if st.failures > 0 {
            self.warn(format!(
                "character {owner}: {} path(s) failed to tessellate",
                st.failures
            ));
        }
        let vb = tessellate::bounds(&raw);
        let bounds = bounds
            .or_else(|| {
                vb.map(|b| {
                    [
                        b[0].floor() as i32,
                        b[1].floor() as i32,
                        b[2].ceil() as i32,
                        b[3].ceil() as i32,
                    ]
                })
            })
            .unwrap_or([0; 4]);
        let max_abs = vb.map_or(0.0, |b| b.iter().fold(0f32, |m, v| m.max(v.abs())));
        let shift = shift_for(max_abs);
        let first_draw = self.a.draws.len() as u32;
        for d in &raw {
            self.emit_draw(d, shift, owner);
        }
        self.a.shapes.push(ShapeRec {
            bounds,
            first_draw,
            draw_count: self.a.draws.len() as u32 - first_draw,
        });
        self.a.shapes.len() as u32 - 1
    }

    /// `head` supplies kind, flags, shift and texture; offsets and counts are filled in.
    fn push_draw(
        &mut self,
        head: DrawRec,
        pos: &[[f32; 2]],
        extra: &dyn Fn(usize, &mut Vec<u8>),
        idx: &[u32],
    ) {
        for (verts, ind) in split_u16(pos.len(), idx) {
            let mut vb = Vec::with_capacity(verts.len() * 16);
            for &v in &verts {
                let v = v as usize;
                extra(v, &mut vb);
                vb.extend_from_slice(&q16(pos[v][0], head.shift).to_le_bytes());
                vb.extend_from_slice(&q16(pos[v][1], head.shift).to_le_bytes());
                vb.extend_from_slice(&[0; 4]);
            }
            let ib: Vec<u8> = ind.iter().flat_map(|i| i.to_le_bytes()).collect();
            let vtx_off = self.a.push_data(&vb);
            let idx_off = self.a.push_data(&ib);
            self.stats.triangles += ind.len() / 3;
            self.stats.draws += 1;
            self.a.draws.push(DrawRec {
                vtx_off,
                vtx_count: verts.len() as u32,
                idx_off,
                idx_count: ind.len() as u32,
                ..head
            });
        }
    }

    fn emit_draw(&mut self, d: &RawDraw, shift: u8, owner: u16) {
        let stroke = if d.stroke { ab::draw_flag::STROKE } else { 0 };
        match &d.paint {
            Paint::Solid => {
                let colors = &d.color;
                self.push_draw(
                    head(false, stroke, shift, ab::NONE),
                    &d.pos,
                    &|v, out| out.extend_from_slice(&colors[v]),
                    &d.idx,
                );
            }
            Paint::Gradient(style) => {
                let Some((tex, u_scale)) = self.gradient_texture(style, owner) else {
                    return;
                };
                let radial = !matches!(style, FillStyle::LinearGradient(_));
                let g = match style {
                    FillStyle::LinearGradient(g) | FillStyle::RadialGradient(g) => g,
                    FillStyle::FocalGradient { gradient, .. } => gradient,
                    _ => return,
                };
                let uvs: Vec<[f32; 2]> = d
                    .pos
                    .iter()
                    .map(|p| {
                        let uv = gradients::uv(g, *p);
                        if radial { uv } else { [uv[0] * u_scale, 0.5] }
                    })
                    .collect();
                self.push_draw(
                    head(true, stroke | ab::draw_flag::SMOOTH, shift, tex),
                    &d.pos,
                    &|v, out| {
                        out.extend_from_slice(&uvs[v][0].to_le_bytes());
                        out.extend_from_slice(&uvs[v][1].to_le_bytes());
                    },
                    &d.idx,
                );
            }
            Paint::Bitmap {
                id,
                matrix,
                smooth,
                repeat,
            } => {
                let id = *id;
                let Some(img) = self.decode_bitmap(id) else {
                    self.warn(format!("character {owner}: fill uses missing bitmap {id}"));
                    return;
                };
                let inv = gradients::invert(matrix);
                let orig: Vec<[f64; 2]> =
                    d.pos.iter().map(|p| gradients::apply(&inv, *p)).collect();
                // Flash marks most bitmap fills repeating; only wrap when the fill really
                // reaches past the bitmap, so pixel art keeps its exact pixels.
                let (w, h) = (img.width as f64, img.height as f64);
                let wraps = *repeat
                    && orig
                        .iter()
                        .any(|q| q[0] < -0.5 || q[1] < -0.5 || q[0] > w + 0.5 || q[1] > h + 0.5);
                let Some(entry) = self.bitmap(id, wraps) else {
                    return;
                };
                let conv = entry.conv.clone();
                let first = entry.first_texture;
                let (sx, sy) = (
                    conv.img_w as f64 / conv.orig_w as f64,
                    conv.img_h as f64 / conv.orig_h as f64,
                );
                let img_uv: Vec<[f32; 2]> = orig
                    .iter()
                    .map(|q| [(q[0] * sx) as f32, (q[1] * sy) as f32])
                    .collect();
                let flags = stroke | if *smooth { ab::draw_flag::SMOOTH } else { 0 };
                for (ti, tile) in conv.tiles.iter().enumerate() {
                    let tex = first + ti as u32;
                    let (tw, th) = (tile.texture.width as f32, tile.texture.height as f32);
                    let (pos, uv, idx) = if conv.tiles.len() == 1 {
                        (d.pos.clone(), img_uv.clone(), d.idx.clone())
                    } else {
                        clip_uv(
                            &d.pos,
                            &img_uv,
                            &d.idx,
                            [
                                tile.x as f32,
                                tile.y as f32,
                                (tile.x + tile.w) as f32,
                                (tile.y + tile.h) as f32,
                            ],
                        )
                    };
                    if idx.is_empty() {
                        continue;
                    }
                    let tuv: Vec<[f32; 2]> = if conv.stretched {
                        uv.iter()
                            .map(|u| [u[0] / conv.img_w as f32, u[1] / conv.img_h as f32])
                            .collect()
                    } else {
                        uv.iter()
                            .map(|u| [(u[0] - tile.x as f32) / tw, (u[1] - tile.y as f32) / th])
                            .collect()
                    };
                    self.push_draw(
                        head(true, flags, shift, tex),
                        &pos,
                        &|v, out| {
                            out.extend_from_slice(&tuv[v][0].to_le_bytes());
                            out.extend_from_slice(&tuv[v][1].to_le_bytes());
                        },
                        &idx,
                    );
                }
            }
        }
    }

    fn add_font(&mut self, id: u16, f: &FontTag) -> Option<u32> {
        if let FontTag::V4(_) = f {
            self.warn(format!("font {id}: DefineFont4 (CFF) is not supported"));
            return None;
        }
        let fd = text::font_data(self.chars, id)?;
        let first_glyph = self.a.glyphs.len() as u32;
        // Glyphs are meshed in font units, sized as if drawn GLYPH_REF_PX tall.
        let units_per_twip = fd.em / (GLYPH_REF_PX * 20.0);
        let tol = (self.tol_twips as f64 * units_per_twip) as f32;
        let style = [FillStyle::Color(swf::Color::WHITE)];
        for g in &fd.glyphs {
            let cmds = text::glyph_cmds(g.records);
            let shape = if cmds.is_empty() {
                ab::NONE
            } else {
                let paths = [DrawPath::Fill {
                    style: &style[0],
                    cmds,
                    rule: FillRule::EvenOdd,
                }];
                self.add_mesh_tol(&paths, None, id, tol, 0.0)
            };
            self.a.glyphs.push((g.code, g.advance, shape));
        }
        let name = self.a.strings.intern(&fd.name);
        self.a.fonts.push(FontRec {
            name,
            flags: (fd.bold as u16) | ((fd.italic as u16) << 1),
            glyph_count: fd.glyphs.len() as u16,
            first_glyph,
            ascent: fd.ascent,
            descent: fd.descent,
            leading: fd.leading,
            em: fd.em as u16,
        });
        Some(self.a.fonts.len() as u32 - 1)
    }

    fn add_edit(&mut self, e: &swf::EditText) -> u32 {
        let v = self.chars.swf_version;
        let var = self.a.strings.intern(&decode_str(e.variable_name(), v));
        let init = e
            .initial_text()
            .map(|t| decode_str(t, v))
            .unwrap_or_default();
        let initial_text = self.a.strings.intern(&init);
        let layout = e.layout().cloned().unwrap_or_default();
        let col = e.color().unwrap_or(swf::Color::BLACK);
        if e.font_class().is_some() {
            self.warn(format!(
                "edit text {}: font class names are not supported",
                e.id()
            ));
        }
        self.a.edits.push(EditRec {
            bounds: tessellate::twips_rect(e.bounds()),
            font_id: e.font_id().unwrap_or(0),
            height: e.height().map_or(240, |h| h.get().clamp(0, 65535) as u16),
            color: u32::from_le_bytes([col.r, col.g, col.b, col.a]),
            max_length: e.max_length().unwrap_or(0),
            flags: e.flags().bits(),
            align: layout.align as u8,
            left_margin: layout.left_margin.get().clamp(0, 65535) as u16,
            right_margin: layout.right_margin.get().clamp(0, 65535) as u16,
            indent: layout.indent.get().clamp(0, 65535) as u16,
            leading: layout.leading.get().clamp(-32768, 32767) as i16,
            var_name: var,
            initial_text,
        });
        self.a.edits.len() as u32 - 1
    }

    fn add_button(&mut self, b: &swf::Button, version: u8) -> u32 {
        let first_record = self.a.button_records.len() as u32;
        let cx_v1 = self
            .chars
            .button_cxforms
            .get(&b.id)
            .and_then(|t| t.color_transforms.first().cloned());
        for r in &b.records {
            let cx = if version == 1 {
                cx_v1.unwrap_or(r.color_transform)
            } else {
                r.color_transform
            };
            if !r.filters.is_empty() {
                self.warn(format!("button {}: filters ignored", b.id));
            }
            self.a.button_records.push(ButtonRecord {
                states: r.states.bits(),
                blend: r.blend_mode as u8,
                char_id: r.id,
                depth: r.depth,
                matrix: matrix6(&r.matrix),
                cxform: cxform8(&cx),
            });
        }
        let first_action = self.a.button_actions.len() as u32;
        for act in &b.actions {
            let script = self.scripts.index_of(act.action_data).unwrap_or(ab::NONE);
            let cond = if version == 1 {
                swf::ButtonActionCondition::OVER_DOWN_TO_OVER_UP.bits()
            } else {
                act.conditions.bits()
            };
            self.a.button_actions.push((
                cond & 0x1FF,
                act.key_press().map_or(0, |k| k.get() as u16),
                script,
            ));
        }
        self.a.buttons.push(ButtonRec {
            first_record,
            record_count: b.records.len() as u16,
            flags: b.is_track_as_menu as u16,
            first_action,
            action_count: b.actions.len() as u32,
        });
        self.a.buttons.len() as u32 - 1
    }

    fn push_sound(&mut self, format: u8, channels: u8, rate: u32, frames: u32, data: &[u8]) -> u32 {
        let data_off = self.a.push_data(data);
        self.stats.sound_bytes += data.len();
        self.a.sounds.push(SoundRec {
            format,
            channels,
            rate,
            frames,
            data_off,
            data_size: data.len() as u32,
        });
        self.a.sounds.len() as u32 - 1
    }

    fn add_event_sound(&mut self, s: &swf::Sound) -> Option<u32> {
        match sounds::decode(&s.format, s.data, s.num_samples as usize) {
            Ok(pcm) if pcm.frames() > 0 => {
                let enc = sounds::encode_ima(&pcm);
                Some(self.push_sound(
                    ab::sound_format::IMA4,
                    pcm.channels,
                    pcm.rate,
                    pcm.frames() as u32,
                    &enc,
                ))
            }
            Ok(_) => {
                self.warn(format!("sound {}: no audio data", s.id));
                None
            }
            Err(e) => {
                self.warn(format!("sound {}: {e}; dropped", s.id));
                None
            }
        }
    }

    fn morph_ratios(&self, root: &[Tag]) -> BTreeMap<u16, BTreeSet<u16>> {
        let mut out: BTreeMap<u16, BTreeSet<u16>> = BTreeMap::new();
        let is_morph = |id: u16| matches!(self.chars.get(id), Some(Character::Morph(_)));
        let mut walk = |tags: &[Tag]| {
            let mut depth: HashMap<u16, u16> = HashMap::new();
            for t in tags {
                match t {
                    Tag::PlaceObject(p) => {
                        let id = match p.action {
                            swf::PlaceObjectAction::Place(id)
                            | swf::PlaceObjectAction::Replace(id) => {
                                depth.insert(p.depth, id);
                                Some(id)
                            }
                            swf::PlaceObjectAction::Modify => depth.get(&p.depth).copied(),
                        };
                        if let Some(id) = id.filter(|&id| is_morph(id)) {
                            let placed = !matches!(p.action, swf::PlaceObjectAction::Modify);
                            if let Some(r) = p.ratio {
                                out.entry(id).or_default().insert(r);
                            } else if placed {
                                out.entry(id).or_default().insert(0);
                            }
                        }
                    }
                    Tag::RemoveObject(r) => {
                        depth.remove(&r.depth);
                    }
                    _ => {}
                }
            }
        };
        walk(root);
        let mut button_morphs = Vec::new();
        for ch in self.chars.map.values() {
            match ch {
                Character::Sprite(sp) => walk(&sp.tags),
                Character::Button(b, _) => {
                    button_morphs.extend(b.records.iter().map(|r| r.id).filter(|&id| is_morph(id)))
                }
                _ => {}
            }
        }
        for id in button_morphs {
            out.entry(id).or_default().insert(0);
        }
        out
    }

    fn add_timeline(&mut self, index: u32, tags: &[Tag], declared_frames: u16) {
        let frame_table = self.a.frames.len() as u32;
        let label_first = self.a.labels.len() as u32;
        let mut frame = 0u32;
        self.a.frames.push(self.a.cmds.len() as u32);
        let mut stream_head: Option<swf::SoundStreamHead> = None;
        let mut mp3 = Vec::new();
        let mut pcm: Vec<i16> = Vec::new();
        let mut stream_pos: u32 = 0;
        let mut pending = false;
        for t in tags {
            pending = true;
            match t {
                Tag::ShowFrame => {
                    frame += 1;
                    self.a.frames.push(self.a.cmds.len() as u32);
                    pending = false;
                }
                Tag::PlaceObject(p) => self.place(p),
                Tag::RemoveObject(r) => {
                    self.a.cmds.push(ab::cmd::REMOVE);
                    self.a.cmds.extend_from_slice(&r.depth.to_le_bytes());
                }
                Tag::DoAction(d) => {
                    if let Some(s) = self.scripts.index_of(d) {
                        self.a.cmds.push(ab::cmd::ACTION);
                        self.a.cmds.extend_from_slice(&s.to_le_bytes());
                    }
                }
                Tag::FrameLabel(l) => {
                    let name = self
                        .a
                        .strings
                        .intern(&decode_str(l.label, self.chars.swf_version));
                    self.a.labels.push((name, frame));
                }
                Tag::StartSound(s) => {
                    let info = &s.sound_info;
                    if info.envelope.is_some() {
                        self.warn(format!("sound {}: envelopes ignored", s.id));
                    }
                    let c = &mut self.a.cmds;
                    c.push(ab::cmd::START_SOUND);
                    c.extend_from_slice(&s.id.to_le_bytes());
                    c.push(info.event as u8);
                    c.push(0);
                    c.extend_from_slice(&info.num_loops.to_le_bytes());
                    c.extend_from_slice(&[0, 0]);
                    c.extend_from_slice(&info.in_sample.unwrap_or(ab::NONE).to_le_bytes());
                    c.extend_from_slice(&info.out_sample.unwrap_or(ab::NONE).to_le_bytes());
                }
                Tag::SoundStreamHead(h) | Tag::SoundStreamHead2(h) => stream_head = Some(h.clone()),
                Tag::SoundStreamBlock(b) => {
                    let Some(h) = &stream_head else { continue };
                    let fmt = &h.stream_format;
                    self.a.cmds.push(ab::cmd::STREAM_BLOCK);
                    self.a.cmds.extend_from_slice(&stream_pos.to_le_bytes());
                    if fmt.compression == swf::AudioCompression::Mp3 {
                        let (n, payload) = sounds::mp3_block_payload(b);
                        mp3.extend_from_slice(payload);
                        stream_pos += n as u32;
                    } else {
                        match sounds::decode(fmt, b, h.num_samples_per_block as usize) {
                            Ok(p) => {
                                stream_pos += p.frames() as u32;
                                pcm.extend_from_slice(&p.samples);
                            }
                            Err(e) => self.warn(format!("sound stream: {e}")),
                        }
                    }
                }
                Tag::DefineSceneAndFrameLabelData(_) | Tag::End => {}
                _ => {}
            }
        }
        if pending && self.a.cmds.len() as u32 > *self.a.frames.last().unwrap_or(&0) {
            frame += 1;
            self.a.frames.push(self.a.cmds.len() as u32);
        }
        let frame_count = frame.max(declared_frames as u32).max(1);
        while (self.a.frames.len() as u32 - frame_table) < frame_count + 1 {
            let end = self.a.cmds.len() as u32;
            self.a.frames.push(end);
        }
        let mut stream = ab::NONE;
        if let Some(h) = &stream_head {
            let f = &h.stream_format;
            let ch = if f.is_stereo { 2 } else { 1 };
            if !mp3.is_empty() {
                stream = self.push_sound(
                    ab::sound_format::MP3,
                    ch,
                    f.sample_rate as u32,
                    stream_pos,
                    &mp3,
                );
            } else if !pcm.is_empty() {
                let p = sounds::Pcm {
                    rate: f.sample_rate as u32,
                    channels: ch,
                    samples: pcm,
                };
                let enc = sounds::encode_ima(&p);
                stream =
                    self.push_sound(ab::sound_format::IMA4, ch, p.rate, p.frames() as u32, &enc);
            }
        }
        self.a.sprites[index as usize] = SpriteRec {
            frame_count,
            frame_table,
            label_first,
            label_count: self.a.labels.len() as u32 - label_first,
            stream,
        };
    }

    fn place(&mut self, p: &swf::PlaceObject) {
        use ab::place as pf;
        let mut flags = 0u16;
        let ch = match p.action {
            swf::PlaceObjectAction::Place(id) => Some(id),
            swf::PlaceObjectAction::Replace(id) => {
                flags |= pf::MOVE;
                Some(id)
            }
            swf::PlaceObjectAction::Modify => {
                flags |= pf::MOVE;
                None
            }
        };
        if ch.is_some() {
            flags |= pf::CHARACTER;
        }
        if p.matrix.is_some() {
            flags |= pf::MATRIX;
        }
        if p.color_transform.is_some() {
            flags |= pf::CXFORM;
        }
        if p.ratio.is_some() {
            flags |= pf::RATIO;
        }
        if p.name.is_some() {
            flags |= pf::NAME;
        }
        if p.clip_depth.is_some() {
            flags |= pf::CLIP_DEPTH;
        }
        if p.clip_actions.is_some() {
            flags |= pf::CLIP_ACTIONS;
        }
        if p.blend_mode.is_some() {
            flags |= pf::BLEND;
        }
        if p.is_bitmap_cached.is_some() {
            flags |= pf::CACHE;
        }
        if p.is_visible.is_some() {
            flags |= pf::VISIBLE;
        }
        if p.filters.as_ref().is_some_and(|f| !f.is_empty()) {
            self.warn("filters are ignored".to_string());
        }
        let name = p.name.map(|n| {
            self.a
                .strings
                .intern(&decode_str(n, self.chars.swf_version))
        });
        let clip = p.clip_actions.as_ref().map(|ca| {
            let first = self.a.clip_actions.len() as u32;
            for r in &ca.records {
                let s = self.scripts.index_of(r.action_data).unwrap_or(ab::NONE);
                self.a
                    .clip_actions
                    .push((r.events.bits(), r.key_code.unwrap_or(0), s));
            }
            (first, ca.records.len() as u16)
        });
        let c = &mut self.a.cmds;
        c.push(ab::cmd::PLACE);
        c.extend_from_slice(&flags.to_le_bytes());
        c.extend_from_slice(&p.depth.to_le_bytes());
        if let Some(id) = ch {
            c.extend_from_slice(&id.to_le_bytes());
        }
        if let Some(m) = &p.matrix {
            for v in matrix6(m) {
                c.extend_from_slice(&v.to_le_bytes());
            }
        }
        if let Some(cx) = &p.color_transform {
            for v in cxform8(cx) {
                c.extend_from_slice(&v.to_le_bytes());
            }
        }
        if let Some(r) = p.ratio {
            c.extend_from_slice(&r.to_le_bytes());
        }
        if let Some(n) = name {
            c.extend_from_slice(&n.to_le_bytes());
        }
        if let Some(d) = p.clip_depth {
            c.extend_from_slice(&d.to_le_bytes());
        }
        if let Some((first, count)) = clip {
            c.extend_from_slice(&first.to_le_bytes());
            c.extend_from_slice(&count.to_le_bytes());
        }
        if let Some(b) = p.blend_mode {
            c.push(b as u8);
        }
        if let Some(b) = p.is_bitmap_cached {
            c.push(b as u8);
        }
        if let Some(v) = p.is_visible {
            c.push(v as u8);
        }
    }

    fn add_device_font(&mut self) {
        let f = text::device_font();
        let tex = bitmaps::encode(
            &f.atlas,
            false,
            Wrap::Clamp,
            Some(bitmaps::TexFormat::Rgba4444),
        );
        let t = self.add_texture(&tex);
        self.a.device_font = Some((
            t,
            crate::device_font::FIRST as u16,
            f.advances.len() as u16,
            crate::device_font::CELL as u16,
            crate::device_font::GLYPH_H as u16,
            f.cols as u16,
            f.advances,
        ));
    }
}

pub fn matrix6(m: &swf::Matrix) -> [f32; 6] {
    [
        m.a.to_f32(),
        m.b.to_f32(),
        m.c.to_f32(),
        m.d.to_f32(),
        m.tx.get() as f32,
        m.ty.get() as f32,
    ]
}

pub fn cxform8(c: &swf::ColorTransform) -> [i16; 8] {
    [
        c.r_multiply.get(),
        c.g_multiply.get(),
        c.b_multiply.get(),
        c.a_multiply.get(),
        c.r_add,
        c.g_add,
        c.b_add,
        c.a_add,
    ]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn split_keeps_small_meshes_whole_and_splits_big_ones() {
        let idx: Vec<u32> = (0..9).collect();
        assert_eq!(split_u16(9, &idx).len(), 1);
        let n = 70000u32;
        let big: Vec<u32> = (0..n * 3).collect();
        let parts = split_u16((n * 3) as usize, &big);
        assert!(parts.len() >= 4);
        assert!(
            parts
                .iter()
                .all(|(v, i)| v.len() <= 65535 && i.len() <= 65535)
        );
        assert_eq!(parts.iter().map(|p| p.1.len()).sum::<usize>(), big.len());
    }

    #[test]
    fn clip_uv_cuts_a_triangle_at_a_tile_edge() {
        let pos = [[0.0, 0.0], [100.0, 0.0], [0.0, 100.0]];
        let (p, u, i) = clip_uv(&pos, &pos, &[0, 1, 2], [0.0, 0.0, 50.0, 1000.0]);
        assert!(!i.is_empty());
        assert!(u.iter().all(|u| u[0] <= 50.0 + 1e-3));
        assert_eq!(p, u);
    }

    #[test]
    fn shift_fits_coordinates_in_i16() {
        assert_eq!(shift_for(1000.0), 0);
        assert_eq!(shift_for(40000.0), 1);
        assert_eq!(q16(40000.0, 1), 20000);
    }
}
