//! Writes the self-authored render-test SWFs (no ActionScript) with the `swf` crate:
//!     cargo run --example gen_test_swfs -- <out-dir>
//! Output is deterministic, so reference renders stay valid. Each movie covers one area
//! of the asset pipeline: fills, curves, strokes, gradients, bitmaps, text, morphs, sprites.

use std::io::Write as _;
use swf::*;

fn tw(px: f64) -> Twips {
    Twips::from_pixels(px)
}

fn rect_px(x0: f64, y0: f64, x1: f64, y1: f64) -> Rectangle<Twips> {
    Rectangle {
        x_min: tw(x0),
        y_min: tw(y0),
        x_max: tw(x1),
        y_max: tw(y1),
    }
}

/// Records for shapes, built with absolute pixel coordinates.
#[derive(Default)]
struct Path {
    recs: Vec<ShapeRecord>,
    pen: (i32, i32),
}

impl Path {
    fn style(&mut self, f0: Option<u32>, f1: Option<u32>, line: Option<u32>) -> &mut Self {
        self.recs
            .push(ShapeRecord::StyleChange(Box::new(StyleChangeData {
                move_to: None,
                fill_style_0: f0,
                fill_style_1: f1,
                line_style: line,
                new_styles: None,
            })));
        self
    }
    fn move_to(&mut self, x: f64, y: f64) -> &mut Self {
        let p = (tw(x).get(), tw(y).get());
        self.recs
            .push(ShapeRecord::StyleChange(Box::new(StyleChangeData {
                move_to: Some(Point::new(Twips::new(p.0), Twips::new(p.1))),
                fill_style_0: None,
                fill_style_1: None,
                line_style: None,
                new_styles: None,
            })));
        self.pen = p;
        self
    }
    fn line_to(&mut self, x: f64, y: f64) -> &mut Self {
        let p = (tw(x).get(), tw(y).get());
        self.recs.push(ShapeRecord::StraightEdge {
            delta: PointDelta::new(Twips::new(p.0 - self.pen.0), Twips::new(p.1 - self.pen.1)),
        });
        self.pen = p;
        self
    }
    fn curve_to(&mut self, cx: f64, cy: f64, x: f64, y: f64) -> &mut Self {
        let c = (tw(cx).get(), tw(cy).get());
        let p = (tw(x).get(), tw(y).get());
        self.recs.push(ShapeRecord::CurvedEdge {
            control_delta: PointDelta::new(
                Twips::new(c.0 - self.pen.0),
                Twips::new(c.1 - self.pen.1),
            ),
            anchor_delta: PointDelta::new(Twips::new(p.0 - c.0), Twips::new(p.1 - c.1)),
        });
        self.pen = p;
        self
    }
    fn rect(&mut self, x: f64, y: f64, w: f64, h: f64) -> &mut Self {
        self.move_to(x, y)
            .line_to(x + w, y)
            .line_to(x + w, y + h)
            .line_to(x, y + h)
            .line_to(x, y)
    }
    /// Clockwise circle from 8 quadratic segments.
    fn circle(&mut self, cx: f64, cy: f64, r: f64) -> &mut Self {
        let k = (std::f64::consts::PI / 8.0).tan() * r;
        let s = std::f64::consts::FRAC_1_SQRT_2 * r;
        self.move_to(cx + r, cy)
            .curve_to(cx + r, cy + k, cx + s, cy + s)
            .curve_to(cx + k, cy + r, cx, cy + r)
            .curve_to(cx - k, cy + r, cx - s, cy + s)
            .curve_to(cx - r, cy + k, cx - r, cy)
            .curve_to(cx - r, cy - k, cx - s, cy - s)
            .curve_to(cx - k, cy - r, cx, cy - r)
            .curve_to(cx + k, cy - r, cx + s, cy - s)
            .curve_to(cx + r, cy - k, cx + r, cy)
    }
}

fn shape(
    id: u16,
    version: u8,
    fills: Vec<FillStyle>,
    lines: Vec<LineStyle>,
    recs: Vec<ShapeRecord>,
    bounds: Rectangle<Twips>,
) -> Tag<'static> {
    Tag::DefineShape(Box::new(Shape {
        version,
        id,
        shape_bounds: bounds,
        edge_bounds: bounds,
        flags: ShapeFlag::HAS_SCALING_STROKES,
        styles: ShapeStyles {
            fill_styles: fills,
            line_styles: lines,
        },
        shape: recs,
    }))
}

fn place(id: u16, depth: u16, matrix: Matrix) -> Tag<'static> {
    place_full(id, depth, matrix, None, None)
}

fn place_full(
    id: u16,
    depth: u16,
    matrix: Matrix,
    cx: Option<ColorTransform>,
    ratio: Option<u16>,
) -> Tag<'static> {
    Tag::PlaceObject(Box::new(PlaceObject {
        version: 2,
        action: PlaceObjectAction::Place(id),
        depth,
        matrix: Some(matrix),
        color_transform: cx,
        ratio,
        name: None,
        clip_depth: None,
        class_name: None,
        filters: None,
        background_color: None,
        blend_mode: None,
        clip_actions: None,
        has_image: false,
        is_bitmap_cached: None,
        is_visible: None,
        amf_data: None,
    }))
}

fn translate(x: f64, y: f64) -> Matrix {
    Matrix::translate(tw(x), tw(y))
}

fn scale_rot(sx: f64, sy: f64, angle: f64, x: f64, y: f64) -> Matrix {
    let (s, c) = angle.sin_cos();
    Matrix {
        a: Fixed16::from_f64(sx * c),
        b: Fixed16::from_f64(sx * s),
        c: Fixed16::from_f64(-sy * s),
        d: Fixed16::from_f64(sy * c),
        tx: tw(x),
        ty: tw(y),
    }
}

fn rgb(v: u32) -> Color {
    Color::from_rgb(v, 255)
}

fn rgba(v: u32, a: u8) -> Color {
    Color::from_rgb(v, a)
}

/// Gradient box matrix like Flash's createGradientBox: maps the 32768-unit gradient
/// square onto a w x h pixel box at (x, y), rotated by `angle`.
fn gradient_box(x: f64, y: f64, w: f64, h: f64, angle: f64) -> Matrix {
    let sx = w * 20.0 / 32768.0;
    let sy = h * 20.0 / 32768.0;
    scale_rot(sx, sy, angle, x + w / 2.0, y + h / 2.0)
}

fn gradient(matrix: Matrix, spread: GradientSpread, stops: &[(u8, Color)]) -> Gradient {
    Gradient {
        matrix,
        spread,
        interpolation: GradientInterpolation::Rgb,
        records: stops
            .iter()
            .map(|(ratio, color)| GradientRecord {
                ratio: *ratio,
                color: *color,
            })
            .collect(),
    }
}

fn write(dir: &std::path::Path, name: &str, stage: (f64, f64), bg: u32, tags: Vec<Tag>) {
    let mut all = vec![Tag::SetBackgroundColor(rgb(bg))];
    all.extend(tags);
    all.push(Tag::ShowFrame);
    let header = Header {
        compression: Compression::Zlib,
        version: 8,
        stage_size: rect_px(0.0, 0.0, stage.0, stage.1),
        frame_rate: Fixed8::from_f32(30.0),
        num_frames: 1,
    };
    let mut out = Vec::new();
    swf::write_swf(&header, &all, &mut out).expect("SWF serialisation failed");
    let path = dir.join(format!("{name}.swf"));
    std::fs::write(&path, out).unwrap_or_else(|e| panic!("cannot write {}: {e}", path.display()));
}

fn zlib(data: &[u8]) -> Vec<u8> {
    let mut e = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::best());
    e.write_all(data).unwrap();
    e.finish().unwrap()
}

/// DefineBitsLossless2 from straight RGBA pixels (stored premultiplied ARGB).
fn lossless2(id: u16, w: u16, h: u16, px: &dyn Fn(u32, u32) -> [u8; 4]) -> Tag<'static> {
    let mut data = Vec::with_capacity(w as usize * h as usize * 4);
    for y in 0..h as u32 {
        for x in 0..w as u32 {
            let [r, g, b, a] = px(x, y);
            let pm = |c: u8| ((c as u32 * a as u32 + 127) / 255) as u8;
            data.extend_from_slice(&[a, pm(r), pm(g), pm(b)]);
        }
    }
    Tag::DefineBitsLossless(DefineBitsLossless {
        version: 2,
        id,
        format: BitmapFormat::Rgb32,
        width: w,
        height: h,
        data: std::borrow::Cow::Owned(zlib(&data)),
    })
}

fn bitmap_fill(id: u16, x: f64, y: f64, scale: f64, smooth: bool, repeat: bool) -> FillStyle {
    FillStyle::Bitmap {
        id,
        matrix: scale_rot(scale * 20.0, scale * 20.0, 0.0, x, y),
        is_smoothed: smooth,
        is_repeating: repeat,
    }
}

fn gen_solid(dir: &std::path::Path) {
    let mut p = Path::default();
    p.style(None, Some(1), None).rect(20.0, 20.0, 140.0, 100.0);
    p.style(None, Some(2), None).circle(250.0, 80.0, 60.0);
    p.style(None, Some(3), None)
        .move_to(330.0, 140.0)
        .line_to(460.0, 140.0)
        .line_to(395.0, 20.0)
        .line_to(330.0, 140.0);
    p.style(None, Some(4), None).rect(100.0, 90.0, 200.0, 120.0);
    p.style(None, Some(5), None).circle(400.0, 210.0, 45.0);
    let fills = vec![
        FillStyle::Color(rgb(0xd03030)),
        FillStyle::Color(rgb(0x30a050)),
        FillStyle::Color(rgb(0x3050d0)),
        FillStyle::Color(rgba(0xf0c020, 160)),
        FillStyle::Color(rgb(0x202020)),
    ];
    write(
        dir,
        "g01_solid_shapes",
        (480.0, 272.0),
        0xf4f4f4,
        vec![
            shape(1, 3, fills, vec![], p.recs, rect_px(0.0, 0.0, 480.0, 272.0)),
            place(1, 1, Matrix::IDENTITY),
        ],
    );
}

fn gen_holes(dir: &std::path::Path) {
    // Even-odd: a ring from two circles sharing one fill, and a five-point star.
    let mut p = Path::default();
    p.style(None, Some(1), None)
        .circle(120.0, 136.0, 90.0)
        .circle(120.0, 136.0, 50.0);
    let (cx, cy, r) = (340.0, 136.0, 100.0);
    p.style(None, Some(2), None);
    for i in 0..=5 {
        let a =
            -std::f64::consts::FRAC_PI_2 + (i * 2 % 5) as f64 * 2.0 * std::f64::consts::PI / 5.0;
        let (x, y) = (cx + r * a.cos(), cy + r * a.sin());
        if i == 0 {
            p.move_to(x, y);
        } else {
            p.line_to(x, y);
        }
    }
    write(
        dir,
        "g02_curves_holes",
        (480.0, 272.0),
        0xffffff,
        vec![
            shape(
                1,
                3,
                vec![
                    FillStyle::Color(rgb(0x8040c0)),
                    FillStyle::Color(rgb(0xe08020)),
                ],
                vec![],
                p.recs,
                rect_px(0.0, 0.0, 480.0, 272.0),
            ),
            place(1, 1, Matrix::IDENTITY),
        ],
    );
}

fn gen_strokes(dir: &std::path::Path) {
    let mut lines = Vec::new();
    let mut p = Path::default();
    let caps = [
        LineCapStyle::Round,
        LineCapStyle::None,
        LineCapStyle::Square,
    ];
    for (i, cap) in caps.iter().enumerate() {
        lines.push(
            LineStyle::new()
                .with_width(tw(14.0))
                .with_color(rgb(0x2050a0))
                .with_start_cap(*cap)
                .with_end_cap(*cap),
        );
        let y = 40.0 + i as f64 * 40.0;
        p.style(None, None, Some(lines.len() as u32))
            .move_to(40.0, y)
            .line_to(200.0, y);
    }
    let joins = [
        LineJoinStyle::Round,
        LineJoinStyle::Bevel,
        LineJoinStyle::Miter(Fixed8::from_f32(3.0)),
    ];
    for (i, join) in joins.iter().enumerate() {
        lines.push(
            LineStyle::new()
                .with_width(tw(12.0))
                .with_color(rgb(0xb03020))
                .with_join_style(*join),
        );
        let x = 250.0 + i as f64 * 75.0;
        p.style(None, None, Some(lines.len() as u32))
            .move_to(x, 140.0)
            .line_to(x + 30.0, 40.0)
            .line_to(x + 60.0, 140.0);
    }
    for (i, w) in [0.0, 1.0, 3.0].iter().enumerate() {
        lines.push(
            LineStyle::new()
                .with_width(tw(*w))
                .with_color(rgb(0x000000)),
        );
        let y = 180.0 + i as f64 * 25.0;
        p.style(None, None, Some(lines.len() as u32))
            .move_to(40.0, y)
            .curve_to(140.0, y - 30.0, 240.0, y);
    }
    lines.push(
        LineStyle::new()
            .with_width(tw(6.0))
            .with_color(rgb(0x208040)),
    );
    p.style(None, Some(1), Some(lines.len() as u32))
        .circle(380.0, 210.0, 45.0);
    write(
        dir,
        "g03_strokes",
        (480.0, 272.0),
        0xffffff,
        vec![
            shape(
                1,
                4,
                vec![FillStyle::Color(rgb(0xc0e0c0))],
                lines,
                p.recs,
                rect_px(0.0, 0.0, 480.0, 272.0),
            ),
            place(1, 1, Matrix::IDENTITY),
        ],
    );
}

fn gen_linear(dir: &std::path::Path) {
    let stops = [
        (0u8, rgb(0xff0000)),
        (128, rgb(0xffff00)),
        (255, rgb(0x0000ff)),
    ];
    let mut fills = Vec::new();
    let mut p = Path::default();
    for (i, spread) in [
        GradientSpread::Pad,
        GradientSpread::Reflect,
        GradientSpread::Repeat,
    ]
    .iter()
    .enumerate()
    {
        let x = 20.0 + i as f64 * 150.0;
        fills.push(FillStyle::LinearGradient(gradient(
            gradient_box(x + 40.0, 20.0, 60.0, 100.0, 0.0),
            *spread,
            &stops,
        )));
        p.style(None, Some(fills.len() as u32), None)
            .rect(x, 20.0, 140.0, 100.0);
    }
    fills.push(FillStyle::LinearGradient(gradient(
        gradient_box(20.0, 140.0, 440.0, 110.0, 0.6),
        GradientSpread::Pad,
        &[
            (0, rgba(0x000000, 255)),
            (100, rgba(0x00a0ff, 200)),
            (255, rgba(0xffffff, 40)),
        ],
    )));
    p.style(None, Some(fills.len() as u32), None)
        .rect(20.0, 140.0, 440.0, 110.0);
    write(
        dir,
        "g04_linear_gradients",
        (480.0, 272.0),
        0x808080,
        vec![
            shape(1, 3, fills, vec![], p.recs, rect_px(0.0, 0.0, 480.0, 272.0)),
            place(1, 1, Matrix::IDENTITY),
        ],
    );
}

fn gen_radial(dir: &std::path::Path) {
    let stops = [
        (0u8, rgb(0xffffff)),
        (90, rgb(0xff8000)),
        (255, rgb(0x400000)),
    ];
    let mut p = Path::default();
    p.style(None, Some(1), None).rect(20.0, 20.0, 200.0, 200.0);
    p.style(None, Some(2), None).circle(350.0, 120.0, 100.0);
    p.style(None, Some(3), None).rect(250.0, 230.0, 210.0, 30.0);
    let fills = vec![
        FillStyle::RadialGradient(gradient(
            gradient_box(20.0, 20.0, 200.0, 200.0, 0.0),
            GradientSpread::Pad,
            &stops,
        )),
        FillStyle::FocalGradient {
            gradient: gradient(
                gradient_box(250.0, 20.0, 200.0, 200.0, 0.4),
                GradientSpread::Pad,
                &[(0, rgb(0xffffff)), (255, rgba(0x0040c0, 255))],
            ),
            focal_point: Fixed8::from_f32(0.7),
        },
        FillStyle::RadialGradient(gradient(
            gradient_box(250.0, 210.0, 210.0, 70.0, 0.0),
            GradientSpread::Pad,
            &[(0, rgba(0x00c000, 255)), (255, rgba(0x00c000, 0))],
        )),
    ];
    write(
        dir,
        "g05_radial_gradients",
        (480.0, 272.0),
        0xe0e0e0,
        vec![
            shape(1, 3, fills, vec![], p.recs, rect_px(0.0, 0.0, 480.0, 272.0)),
            place(1, 1, Matrix::IDENTITY),
        ],
    );
}

fn checker_alpha(x: u32, y: u32) -> [u8; 4] {
    let on = ((x / 4) + (y / 4)).is_multiple_of(2);
    let a = if x < 4 {
        0
    } else {
        255 - (y * 6).min(200) as u8
    };
    if on {
        [255, (x * 8) as u8, 0, a]
    } else {
        [0, 80, (y * 8) as u8, a]
    }
}

fn gen_bitmap_clipped(dir: &std::path::Path) {
    let mut p = Path::default();
    p.style(None, Some(1), None).rect(20.0, 20.0, 128.0, 128.0);
    p.style(None, Some(2), None).rect(200.0, 20.0, 192.0, 192.0);
    write(
        dir,
        "g06_bitmap_clipped",
        (480.0, 272.0),
        0x406040,
        vec![
            lossless2(10, 32, 32, &checker_alpha),
            shape(
                1,
                3,
                vec![
                    bitmap_fill(10, 20.0, 20.0, 4.0, false, false),
                    bitmap_fill(10, 200.0, 20.0, 6.0, true, false),
                ],
                vec![],
                p.recs,
                rect_px(0.0, 0.0, 480.0, 272.0),
            ),
            place(1, 1, Matrix::IDENTITY),
        ],
    );
}

fn gen_bitmap_repeat(dir: &std::path::Path) {
    let mut p = Path::default();
    p.style(None, Some(1), None).rect(10.0, 10.0, 460.0, 120.0);
    p.style(None, Some(2), None).circle(240.0, 200.0, 65.0);
    let tile = |x: u32, y: u32| -> [u8; 4] {
        if x == 0 || y == 0 {
            [40, 40, 40, 255]
        } else if x < y {
            [200, 60, 60, 255]
        } else {
            [240, 220, 180, 255]
        }
    };
    write(
        dir,
        "g07_bitmap_repeat",
        (480.0, 272.0),
        0xffffff,
        vec![
            lossless2(10, 16, 16, &tile),
            lossless2(11, 12, 12, &|x, y| {
                if (x + y) % 3 == 0 {
                    [0, 0, 160, 255]
                } else {
                    [180, 200, 255, 255]
                }
            }),
            shape(
                1,
                3,
                vec![
                    bitmap_fill(10, 10.0, 10.0, 2.0, false, true),
                    bitmap_fill(11, 0.0, 0.0, 3.0, false, true),
                ],
                vec![],
                p.recs,
                rect_px(0.0, 0.0, 480.0, 272.0),
            ),
            place(1, 1, Matrix::IDENTITY),
        ],
    );
}

fn gen_bitmap_large(dir: &std::path::Path) {
    // 800x400 stays above 512 after target scaling, so it is split into GE tiles; the
    // thick grid lines expose seams. Shown smoothed at 0.55x.
    let px = |x: u32, y: u32| -> [u8; 4] {
        if x % 100 < 8 || y % 100 < 8 {
            return [0, 0, 0, 255];
        }
        [(x * 255 / 800) as u8, (y * 255 / 400) as u8, 128, 255]
    };
    let mut p = Path::default();
    p.style(None, Some(1), None).rect(10.0, 26.0, 440.0, 220.0);
    write(
        dir,
        "g08_bitmap_large",
        (480.0, 272.0),
        0xffffff,
        vec![
            lossless2(10, 800, 400, &px),
            shape(
                1,
                3,
                vec![bitmap_fill(10, 10.0, 26.0, 0.55, true, false)],
                vec![],
                p.recs,
                rect_px(10.0, 26.0, 450.0, 246.0),
            ),
            place(1, 1, Matrix::IDENTITY),
        ],
    );
}

/// A tiny block font: each glyph is a set of unit cells on a 5x7 grid (em = 1024).
fn block_glyph(rows: [&str; 7], code: char) -> Glyph {
    let mut p = Path::default();
    let cell = 1024.0 / 8.0;
    for (r, row) in rows.iter().enumerate() {
        for (c, ch) in row.chars().enumerate() {
            if ch == '#' {
                let x = c as f64 * cell / 20.0;
                let y = (r as f64 - 7.0) * cell / 20.0;
                p.style(None, Some(1), None)
                    .rect(x, y, cell / 20.0, cell / 20.0);
            }
        }
    }
    let b = Rectangle {
        x_min: Twips::ZERO,
        y_min: Twips::new(-896),
        x_max: Twips::new(640),
        y_max: Twips::ZERO,
    };
    Glyph {
        shape_records: p.recs,
        code: code as u16,
        advance: (cell * 6.0) as u16,
        bounds: Some(b),
    }
}

fn gen_static_text(dir: &std::path::Path) {
    let glyphs = vec![
        block_glyph(
            [
                "#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#",
            ],
            'H',
        ),
        block_glyph(
            [
                "#####", "#....", "#....", "####.", "#....", "#....", "#####",
            ],
            'E',
        ),
        block_glyph(
            [
                "#....", "#....", "#....", "#....", "#....", "#....", "#####",
            ],
            'L',
        ),
        block_glyph(
            [
                ".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###.",
            ],
            'O',
        ),
        block_glyph(
            [
                "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..",
            ],
            'T',
        ),
    ];
    let font = Tag::DefineFont2(Box::new(Font {
        version: 2,
        id: 2,
        name: SwfStr::from_utf8_str("AnyBlock"),
        language: Language::Latin,
        layout: Some(FontLayout {
            ascent: 900,
            descent: 200,
            leading: 0,
            kerning: vec![],
        }),
        glyphs,
        flags: FontFlag::HAS_LAYOUT | FontFlag::HAS_WIDE_CODES,
    }));
    let entry = |i: u32| GlyphEntry {
        index: i,
        advance: 0,
    };
    let line = |idx: &[u32], h: f64, adv: i32| -> Vec<GlyphEntry> {
        idx.iter()
            .map(|&i| GlyphEntry {
                advance: (h * 20.0 * 6.0 / 8.0) as i32 + adv,
                ..entry(i)
            })
            .collect()
    };
    let text = Tag::DefineText(Box::new(Text {
        id: 3,
        bounds: rect_px(0.0, 0.0, 480.0, 272.0),
        matrix: Matrix::IDENTITY,
        records: vec![
            TextRecord {
                font_id: Some(2),
                color: Some(rgb(0x2040a0)),
                x_offset: Some(tw(30.0)),
                y_offset: Some(tw(80.0)),
                height: Some(tw(48.0)),
                glyphs: line(&[0, 1, 2, 2, 3], 48.0, 0),
            },
            TextRecord {
                font_id: Some(2),
                color: Some(rgb(0xc02020)),
                x_offset: Some(tw(30.0)),
                y_offset: Some(tw(180.0)),
                height: Some(tw(80.0)),
                glyphs: line(&[4, 3, 4, 1], 80.0, 40),
            },
        ],
    }));
    write(
        dir,
        "g09_static_text",
        (480.0, 272.0),
        0xfffff0,
        vec![
            font,
            text,
            place(3, 1, Matrix::IDENTITY),
            place(3, 2, scale_rot(0.5, 0.5, -0.3, 300.0, 250.0)),
        ],
    );
}

fn gen_morph(dir: &std::path::Path) {
    // Morph start records carry the styles in their first StyleChange (with the
    // move-to); end records may only move.
    let mut s = Path::default();
    s.rect(0.0, 0.0, 80.0, 80.0);
    if let ShapeRecord::StyleChange(sc) = &mut s.recs[0] {
        sc.fill_style_1 = Some(1);
        sc.line_style = Some(1);
    }
    let mut e = Path::default();
    e.move_to(40.0, -20.0)
        .line_to(100.0, 40.0)
        .line_to(40.0, 100.0)
        .line_to(-20.0, 40.0)
        .line_to(40.0, -20.0);
    let morph = Tag::DefineMorphShape(Box::new(DefineMorphShape {
        version: 1,
        id: 1,
        flags: DefineMorphShapeFlag::empty(),
        start: MorphShape {
            shape_bounds: rect_px(0.0, 0.0, 80.0, 80.0),
            edge_bounds: rect_px(0.0, 0.0, 80.0, 80.0),
            fill_styles: vec![FillStyle::Color(rgb(0x20a0e0))],
            line_styles: vec![
                LineStyle::new()
                    .with_width(tw(4.0))
                    .with_color(rgb(0x003060)),
            ],
            shape: s.recs,
        },
        end: MorphShape {
            shape_bounds: rect_px(-20.0, -20.0, 100.0, 100.0),
            edge_bounds: rect_px(-20.0, -20.0, 100.0, 100.0),
            fill_styles: vec![FillStyle::Color(rgb(0xe04020))],
            line_styles: vec![
                LineStyle::new()
                    .with_width(tw(12.0))
                    .with_color(rgb(0x600000)),
            ],
            shape: e.recs,
        },
    }));
    write(
        dir,
        "g10_morph",
        (480.0, 272.0),
        0xffffff,
        vec![
            morph,
            place_full(1, 1, translate(40.0, 90.0), None, Some(0)),
            place_full(1, 2, translate(200.0, 90.0), None, Some(32768)),
            place_full(1, 3, translate(360.0, 90.0), None, Some(65535)),
        ],
    );
}

fn gen_sprites(dir: &std::path::Path) {
    let mut p = Path::default();
    p.style(None, Some(1), None).rect(-30.0, -20.0, 60.0, 40.0);
    p.style(None, Some(2), None).circle(0.0, 0.0, 12.0);
    let inner = Tag::DefineSprite(Sprite {
        id: 2,
        num_frames: 1,
        tags: vec![
            place(1, 1, Matrix::IDENTITY),
            place(1, 2, scale_rot(0.4, 0.4, 0.5, 30.0, 20.0)),
            Tag::ShowFrame,
        ],
    });
    let outer = Tag::DefineSprite(Sprite {
        id: 3,
        num_frames: 1,
        tags: vec![
            place(2, 1, translate(-40.0, 0.0)),
            place(2, 2, scale_rot(0.8, 0.8, 0.8, 40.0, 10.0)),
            Tag::ShowFrame,
        ],
    });
    write(
        dir,
        "g11_nested_sprites",
        (480.0, 272.0),
        0xf0f0ff,
        vec![
            shape(
                1,
                3,
                vec![
                    FillStyle::Color(rgb(0x30a030)),
                    FillStyle::Color(rgb(0xffffff)),
                ],
                vec![],
                p.recs,
                rect_px(-30.0, -20.0, 30.0, 20.0),
            ),
            inner,
            outer,
            place(3, 1, translate(120.0, 90.0)),
            place(3, 2, scale_rot(1.5, 1.5, -0.4, 330.0, 100.0)),
            place(2, 3, scale_rot(2.0, 1.0, 0.0, 240.0, 220.0)),
        ],
    );
}

fn gen_scaled_stage(dir: &std::path::Path) {
    let mut p = Path::default();
    p.style(None, Some(1), None).rect(0.0, 0.0, 550.0, 400.0);
    p.style(None, Some(2), None).circle(275.0, 200.0, 150.0);
    p.style(None, Some(3), Some(1))
        .rect(40.0, 40.0, 120.0, 80.0);
    write(
        dir,
        "g12_scaled_stage",
        (550.0, 400.0),
        0x202020,
        vec![
            shape(
                1,
                3,
                vec![
                    FillStyle::Color(rgb(0xffe0a0)),
                    FillStyle::Color(rgb(0x4060c0)),
                    FillStyle::Color(rgb(0xc04060)),
                ],
                vec![
                    LineStyle::new()
                        .with_width(tw(2.0))
                        .with_color(rgb(0x000000)),
                ],
                p.recs,
                rect_px(0.0, 0.0, 550.0, 400.0),
            ),
            place(1, 1, Matrix::IDENTITY),
        ],
    );
}

fn gen_png_in_jpeg2(dir: &std::path::Path) {
    let (w, h) = (40u32, 30u32);
    let mut raw = Vec::new();
    for y in 0..h {
        for x in 0..w {
            raw.extend_from_slice(&[
                (x * 6) as u8,
                (y * 8) as u8,
                200,
                if (x / 5 + y / 5) % 2 == 0 { 255 } else { 120 },
            ]);
        }
    }
    let mut png_bytes = Vec::new();
    {
        let mut enc = png::Encoder::new(&mut png_bytes, w, h);
        enc.set_color(png::ColorType::Rgba);
        enc.set_depth(png::BitDepth::Eight);
        let mut wr = enc.write_header().unwrap();
        wr.write_image_data(&raw).unwrap();
    }
    let data: &'static [u8] = Box::leak(png_bytes.into_boxed_slice());
    let mut p = Path::default();
    p.style(None, Some(1), None).rect(40.0, 31.0, 400.0, 210.0);
    write(
        dir,
        "g13_png_in_jpeg2",
        (480.0, 272.0),
        0xffffff,
        vec![
            Tag::DefineBitsJpeg2 {
                id: 10,
                jpeg_data: data,
            },
            shape(
                1,
                3,
                vec![FillStyle::Bitmap {
                    id: 10,
                    matrix: scale_rot(200.0, 140.0, 0.0, 40.0, 31.0),
                    is_smoothed: false,
                    is_repeating: false,
                }],
                vec![],
                p.recs,
                rect_px(40.0, 31.0, 440.0, 241.0),
            ),
            place(1, 1, Matrix::IDENTITY),
        ],
    );
}

fn main() {
    let dir = std::path::PathBuf::from(std::env::args().nth(1).unwrap_or_else(|| "out/gen".into()));
    std::fs::create_dir_all(&dir).expect("cannot create output directory");
    gen_solid(&dir);
    gen_holes(&dir);
    gen_strokes(&dir);
    gen_linear(&dir);
    gen_radial(&dir);
    gen_bitmap_clipped(&dir);
    gen_bitmap_repeat(&dir);
    gen_bitmap_large(&dir);
    gen_static_text(&dir);
    gen_morph(&dir);
    gen_sprites(&dir);
    gen_scaled_stage(&dir);
    gen_png_in_jpeg2(&dir);
    println!("gen_test_swfs: wrote 13 SWFs to {}", dir.display());
}
