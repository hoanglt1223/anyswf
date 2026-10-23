//! Character table: every defined character by id, plus the side tables the runtime
//! needs (exports, init actions, button extras, font info).

use std::collections::BTreeMap;
use std::fmt::Write as _;
use swf::{CharacterId, Tag};

#[derive(Clone, Copy, Debug)]
pub enum BitmapTag<'a> {
    Jpeg1(&'a [u8]),
    Jpeg2(&'a [u8]),
    Jpeg3(&'a swf::DefineBitsJpeg3<'a>),
    Lossless(&'a swf::DefineBitsLossless<'a>),
}

#[derive(Clone, Copy, Debug)]
pub enum FontTag<'a> {
    V1(&'a swf::FontV1),
    V2(&'a swf::Font<'a>),
    V4(&'a swf::Font4<'a>),
}

#[derive(Clone, Copy, Debug)]
pub enum Character<'a> {
    Shape(&'a swf::Shape),
    Morph(&'a swf::DefineMorphShape),
    Sprite(&'a swf::Sprite<'a>),
    Button(&'a swf::Button<'a>, u8),
    Text(&'a swf::Text, u8),
    EditText(&'a swf::EditText<'a>),
    Font(FontTag<'a>),
    Bitmap(BitmapTag<'a>),
    Sound(&'a swf::Sound<'a>),
    BinaryData,
    Video(&'a swf::DefineVideoStream),
}

impl Character<'_> {
    pub fn kind_name(&self) -> &'static str {
        match self {
            Character::Shape(_) => "shape",
            Character::Morph(_) => "morph",
            Character::Sprite(_) => "sprite",
            Character::Button(..) => "button",
            Character::Text(..) => "text",
            Character::EditText(_) => "edittext",
            Character::Font(_) => "font",
            Character::Bitmap(_) => "bitmap",
            Character::Sound(_) => "sound",
            Character::BinaryData => "binary",
            Character::Video(_) => "video",
        }
    }
}

pub struct Characters<'a> {
    pub swf_version: u8,
    pub map: BTreeMap<CharacterId, Character<'a>>,
    pub jpeg_tables: Option<&'a [u8]>,
    pub font_info: BTreeMap<CharacterId, &'a swf::FontInfo<'a>>,
    pub button_cxforms: BTreeMap<CharacterId, &'a swf::ButtonColorTransform>,
    pub exports: Vec<(CharacterId, String)>,
    pub init_actions: Vec<(CharacterId, &'a [u8])>,
    pub warnings: Vec<String>,
}

pub fn decode_str(s: &swf::SwfStr, version: u8) -> String {
    s.to_string_lossy(swf::SwfStr::encoding_for_version(version))
}

impl<'a> Characters<'a> {
    pub fn collect(swf_version: u8, tags: &'a [Tag<'a>]) -> Self {
        let mut c = Characters {
            swf_version,
            map: BTreeMap::new(),
            jpeg_tables: None,
            font_info: BTreeMap::new(),
            button_cxforms: BTreeMap::new(),
            exports: Vec::new(),
            init_actions: Vec::new(),
            warnings: Vec::new(),
        };
        for tag in tags {
            let def = match tag {
                Tag::DefineShape(s) => Some((s.id, Character::Shape(s))),
                Tag::DefineMorphShape(m) => Some((m.id, Character::Morph(m))),
                Tag::DefineSprite(s) => Some((s.id, Character::Sprite(s))),
                Tag::DefineButton(b) => Some((b.id, Character::Button(b, 1))),
                Tag::DefineButton2(b) => Some((b.id, Character::Button(b, 2))),
                Tag::DefineText(t) => Some((t.id, Character::Text(t, 1))),
                Tag::DefineText2(t) => Some((t.id, Character::Text(t, 2))),
                Tag::DefineEditText(e) => Some((e.id(), Character::EditText(e))),
                Tag::DefineFont(f) => Some((f.id, Character::Font(FontTag::V1(f)))),
                Tag::DefineFont2(f) => Some((f.id, Character::Font(FontTag::V2(f)))),
                Tag::DefineFont4(f) => Some((f.id, Character::Font(FontTag::V4(f)))),
                Tag::DefineBits { id, jpeg_data } => {
                    Some((*id, Character::Bitmap(BitmapTag::Jpeg1(jpeg_data))))
                }
                Tag::DefineBitsJpeg2 { id, jpeg_data } => {
                    Some((*id, Character::Bitmap(BitmapTag::Jpeg2(jpeg_data))))
                }
                Tag::DefineBitsJpeg3(j) => Some((j.id, Character::Bitmap(BitmapTag::Jpeg3(j)))),
                Tag::DefineBitsLossless(l) => {
                    Some((l.id, Character::Bitmap(BitmapTag::Lossless(l))))
                }
                Tag::DefineSound(s) => Some((s.id, Character::Sound(s))),
                Tag::DefineBinaryData(b) => Some((b.id, Character::BinaryData)),
                Tag::DefineVideoStream(v) => Some((v.id, Character::Video(v))),
                Tag::JpegTables(t) => {
                    c.jpeg_tables = Some(t);
                    None
                }
                Tag::DefineFontInfo(fi) => {
                    c.font_info.insert(fi.id, fi);
                    None
                }
                Tag::DefineButtonColorTransform(t) => {
                    c.button_cxforms.insert(t.id, t);
                    None
                }
                Tag::ExportAssets(list) => {
                    for e in list {
                        c.exports.push((e.id, decode_str(e.name, swf_version)));
                    }
                    None
                }
                Tag::DoInitAction { id, action_data } => {
                    c.init_actions.push((*id, action_data));
                    None
                }
                Tag::ImportAssets { url, .. } => {
                    c.warnings.push(format!(
                        "ImportAssets from '{}' ignored: imported characters are missing",
                        decode_str(url, swf_version)
                    ));
                    None
                }
                _ => None,
            };
            if let Some((id, ch)) = def {
                // Flash keeps the first definition of an id.
                if let std::collections::btree_map::Entry::Vacant(e) = c.map.entry(id) {
                    e.insert(ch);
                } else {
                    c.warnings
                        .push(format!("character {id} defined twice; keeping the first"));
                }
            }
        }
        c
    }

    pub fn get(&self, id: CharacterId) -> Option<&Character<'a>> {
        self.map.get(&id)
    }
}

fn frames_of(tags: &[Tag]) -> usize {
    tags.iter().filter(|t| matches!(t, Tag::ShowFrame)).count()
}

/// Human-readable character table, for `swf2c inspect`.
pub fn report(swf: &swf::Swf, chars: &Characters) -> String {
    let h = &swf.header;
    let st = h.stage_size();
    let mut s = String::new();
    let _ = writeln!(
        s,
        "SWF version {}, {:?} compression, stage {}x{} px, {} fps, {} frames (header), {} frames (tags)",
        h.version(),
        h.compression(),
        (st.x_max - st.x_min).to_pixels(),
        (st.y_max - st.y_min).to_pixels(),
        h.frame_rate().to_f32(),
        h.num_frames(),
        frames_of(&swf.tags)
    );
    if let Some(bg) = h.background_color() {
        let _ = writeln!(s, "background #{:02x}{:02x}{:02x}", bg.r, bg.g, bg.b);
    }
    let mut actions = 0;
    count_actions(&swf.tags, &mut actions);
    let _ = writeln!(
        s,
        "{} characters, {} action blocks, {} exports, {} init actions",
        chars.map.len(),
        actions,
        chars.exports.len(),
        chars.init_actions.len()
    );
    for (id, ch) in &chars.map {
        let detail = match ch {
            Character::Shape(sh) => format!(
                "v{} {} fills, {} lines, {} records, bounds {}",
                sh.version,
                sh.styles.fill_styles.len(),
                sh.styles.line_styles.len(),
                sh.shape.len(),
                rect(&sh.shape_bounds)
            ),
            Character::Morph(m) => format!(
                "v{} {} fills, {} records",
                m.version,
                m.start.fill_styles.len(),
                m.start.shape.len()
            ),
            Character::Sprite(sp) => format!(
                "{} frames, {} tags, {} actions",
                sp.num_frames,
                sp.tags.len(),
                {
                    let mut n = 0;
                    count_actions(&sp.tags, &mut n);
                    n
                }
            ),
            Character::Button(b, v) => format!(
                "v{v} {} records, {} actions",
                b.records.len(),
                b.actions.len()
            ),
            Character::Text(t, v) => format!(
                "v{v} {} records, bounds {}",
                t.records.len(),
                rect(&t.bounds)
            ),
            Character::EditText(e) => format!(
                "font {:?}, var '{}', text '{}'",
                e.font_id(),
                decode_str(e.variable_name(), chars.swf_version),
                e.initial_text()
                    .map(|t| decode_str(t, chars.swf_version))
                    .unwrap_or_default()
                    .chars()
                    .take(40)
                    .collect::<String>()
            ),
            Character::Font(f) => match f {
                FontTag::V1(f) => format!("DefineFont, {} glyphs", f.glyphs.len()),
                FontTag::V2(f) => format!(
                    "DefineFont{} '{}', {} glyphs",
                    f.version,
                    decode_str(f.name, chars.swf_version),
                    f.glyphs.len()
                ),
                FontTag::V4(f) => format!(
                    "DefineFont4 '{}' (CFF, unsupported)",
                    decode_str(f.name, chars.swf_version)
                ),
            },
            Character::Bitmap(b) => match b {
                BitmapTag::Jpeg1(d) => format!("DefineBits JPEG, {} bytes", d.len()),
                BitmapTag::Jpeg2(d) => format!("DefineBitsJPEG2, {} bytes", d.len()),
                BitmapTag::Jpeg3(j) => format!(
                    "DefineBitsJPEG3, {} bytes + {} alpha",
                    j.data.len(),
                    j.alpha_data.len()
                ),
                BitmapTag::Lossless(l) => format!(
                    "DefineBitsLossless{} {:?} {}x{}",
                    if l.version == 2 { "2" } else { "" },
                    l.format,
                    l.width,
                    l.height
                ),
            },
            Character::Sound(snd) => format!(
                "{:?} {} Hz {}, {} samples",
                snd.format.compression,
                snd.format.sample_rate,
                if snd.format.is_stereo {
                    "stereo"
                } else {
                    "mono"
                },
                snd.num_samples
            ),
            Character::BinaryData => String::new(),
            Character::Video(v) => format!("{:?} {}x{}", v.codec, v.width, v.height),
        };
        let _ = writeln!(s, "  {id:5} {:9} {detail}", ch.kind_name());
    }
    for (id, name) in &chars.exports {
        let _ = writeln!(s, "  export {id} as '{name}'");
    }
    for w in &chars.warnings {
        let _ = writeln!(s, "warning: {w}");
    }
    s
}

fn rect(r: &swf::Rectangle<swf::Twips>) -> String {
    format!(
        "({},{})-({},{})",
        r.x_min.to_pixels(),
        r.y_min.to_pixels(),
        r.x_max.to_pixels(),
        r.y_max.to_pixels()
    )
}

fn count_actions(tags: &[Tag], n: &mut usize) {
    for t in tags {
        match t {
            Tag::DoAction(_) | Tag::DoInitAction { .. } => *n += 1,
            Tag::DefineSprite(s) => count_actions(&s.tags, n),
            Tag::PlaceObject(p) => {
                if let Some(ca) = &p.clip_actions {
                    *n += ca.records.len();
                }
            }
            Tag::DefineButton(b) | Tag::DefineButton2(b) => *n += b.actions.len(),
            _ => {}
        }
    }
}
