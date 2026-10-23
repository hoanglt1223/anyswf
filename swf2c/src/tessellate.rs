//! SWF shape records to triangle meshes.
//!
//! Edge linking (`distill`) is adapted from Ruffle's `ShapeConverter`
//! (render/src/shape_utils.rs, MIT/Apache-2.0): Flash stores fills as an edge soup
//! where fill style 1 lies on the right of an edge and fill style 0 on the left, so
//! style-0 edges are reversed and all edges of a style are linked into closed paths.

use lyon::math::point;
use lyon::path::Path as LyonPath;
use lyon::tessellation::{
    BuffersBuilder, FillOptions, FillRule, FillTessellator, FillVertex, LineCap, LineJoin,
    StrokeOptions, StrokeTessellator, StrokeVertex, VertexBuffers,
};
use swf::{FillStyle, LineCapStyle, LineJoinStyle, LineStyle, ShapeRecord, Twips};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Pt {
    pub x: i32,
    pub y: i32,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Cmd {
    MoveTo(Pt),
    LineTo(Pt),
    QuadTo(Pt, Pt),
}

#[derive(Clone, Debug)]
pub enum DrawPath<'s> {
    Fill {
        style: &'s FillStyle,
        cmds: Vec<Cmd>,
        rule: FillRule,
    },
    Stroke {
        style: &'s LineStyle,
        cmds: Vec<Cmd>,
        closed: bool,
    },
}

#[derive(Clone, Copy)]
struct PathPoint {
    p: Pt,
    control: bool,
}

#[derive(Clone)]
struct Segment {
    points: Vec<PathPoint>,
}

impl Segment {
    fn new(p: Pt) -> Self {
        Segment {
            points: vec![PathPoint { p, control: false }],
        }
    }
    fn is_empty(&self) -> bool {
        self.points.len() <= 1
    }
    fn start(&self) -> Pt {
        self.points[0].p
    }
    fn end(&self) -> Pt {
        self.points[self.points.len() - 1].p
    }
    fn to_cmds(&self, out: &mut Vec<Cmd>) {
        out.push(Cmd::MoveTo(self.start()));
        let mut it = self.points[1..].iter();
        while let Some(pp) = it.next() {
            if pp.control {
                match it.next() {
                    Some(anchor) => out.push(Cmd::QuadTo(pp.p, anchor.p)),
                    None => out.push(Cmd::LineTo(pp.p)),
                }
            } else {
                out.push(Cmd::LineTo(pp.p));
            }
        }
    }
}

#[derive(Clone, Default)]
struct Pending {
    segments: Vec<Segment>,
}

impl Pending {
    fn link(&mut self, mut seg: Segment) {
        if seg.is_empty() {
            return;
        }
        let (mut start_open, mut end_open) = (true, true);
        let mut i = 0;
        while (start_open || end_open) && i < self.segments.len() {
            let other = &mut self.segments[i];
            if start_open && other.end() == seg.start() {
                other.points.extend_from_slice(&seg.points[1..]);
                seg = self.segments.swap_remove(i);
                start_open = false;
            } else if end_open && seg.end() == other.start() {
                std::mem::swap(&mut other.points, &mut seg.points);
                other.points.extend_from_slice(&seg.points[1..]);
                seg = self.segments.swap_remove(i);
                end_open = false;
            } else {
                i += 1;
            }
        }
        self.segments.push(seg);
    }
}

struct Active {
    style: u32,
    seg: Segment,
}

impl Active {
    fn new() -> Self {
        Active {
            style: 0,
            seg: Segment::new(Pt { x: 0, y: 0 }),
        }
    }
}

/// Converts shape records into fill and stroke paths, layer by layer, fills before strokes.
pub fn distill<'s>(
    records: &'s [ShapeRecord],
    fill_styles: &'s [FillStyle],
    line_styles: &'s [LineStyle],
    non_zero: bool,
) -> Vec<DrawPath<'s>> {
    let rule = if non_zero {
        FillRule::NonZero
    } else {
        FillRule::EvenOdd
    };
    let mut fills_s = fill_styles;
    let mut lines_s = line_styles;
    let mut fills: Vec<Pending> = vec![Pending::default(); fills_s.len()];
    let mut strokes: Vec<Vec<Segment>> = vec![Vec::new(); lines_s.len()];
    let mut f0 = Active::new();
    let mut f1 = Active::new();
    let mut ln = Active::new();
    let mut cursor = Pt { x: 0, y: 0 };
    let mut out = Vec::new();

    fn flush_fill(a: &mut Active, at: Pt, fills: &mut [Pending], flip: bool) {
        if a.style > 0 && !a.seg.is_empty() {
            let mut seg = std::mem::replace(&mut a.seg, Segment::new(at));
            if flip {
                seg.points.reverse();
            }
            if let Some(p) = fills.get_mut(a.style as usize - 1) {
                p.link(seg);
            }
        } else {
            a.seg = Segment::new(at);
        }
    }
    fn flush_stroke(a: &mut Active, at: Pt, strokes: &mut [Vec<Segment>]) {
        if a.style > 0 && !a.seg.is_empty() {
            let seg = std::mem::replace(&mut a.seg, Segment::new(at));
            if let Some(s) = strokes.get_mut(a.style as usize - 1) {
                s.push(seg);
            }
        } else {
            a.seg = Segment::new(at);
        }
    }
    fn flush_layer<'s>(
        fills: &mut [Pending],
        strokes: &mut [Vec<Segment>],
        fill_styles: &'s [FillStyle],
        line_styles: &'s [LineStyle],
        rule: FillRule,
        out: &mut Vec<DrawPath<'s>>,
    ) {
        for (i, p) in fills.iter_mut().enumerate() {
            if p.segments.is_empty() {
                continue;
            }
            let mut cmds = Vec::new();
            for s in &p.segments {
                s.to_cmds(&mut cmds);
            }
            p.segments.clear();
            out.push(DrawPath::Fill {
                style: &fill_styles[i],
                cmds,
                rule,
            });
        }
        for (i, segs) in strokes.iter_mut().enumerate() {
            for s in segs.iter() {
                if s.is_empty() {
                    continue;
                }
                let mut cmds = Vec::new();
                s.to_cmds(&mut cmds);
                out.push(DrawPath::Stroke {
                    style: &line_styles[i],
                    cmds,
                    closed: s.start() == s.end(),
                });
            }
            segs.clear();
        }
    }

    for rec in records {
        match rec {
            ShapeRecord::StyleChange(sc) => {
                if let Some(m) = &sc.move_to {
                    cursor = Pt {
                        x: m.x.get(),
                        y: m.y.get(),
                    };
                    flush_fill(&mut f1, cursor, &mut fills, false);
                    flush_fill(&mut f0, cursor, &mut fills, true);
                    flush_stroke(&mut ln, cursor, &mut strokes);
                }
                if let Some(ns) = &sc.new_styles {
                    flush_fill(&mut f1, cursor, &mut fills, false);
                    flush_fill(&mut f0, cursor, &mut fills, true);
                    flush_stroke(&mut ln, cursor, &mut strokes);
                    flush_layer(&mut fills, &mut strokes, fills_s, lines_s, rule, &mut out);
                    fills_s = &ns.fill_styles;
                    lines_s = &ns.line_styles;
                    fills = vec![Pending::default(); fills_s.len()];
                    strokes = vec![Vec::new(); lines_s.len()];
                }
                let nf = fills_s.len() as u32;
                let nl = lines_s.len() as u32;
                if let Some(id) = sc.fill_style_1 {
                    flush_fill(&mut f1, cursor, &mut fills, false);
                    f1.style = if id <= nf { id } else { 0 };
                }
                if let Some(id) = sc.fill_style_0 {
                    flush_fill(&mut f0, cursor, &mut fills, true);
                    f0.style = if id <= nf { id } else { 0 };
                }
                if let Some(id) = sc.line_style {
                    flush_stroke(&mut ln, cursor, &mut strokes);
                    ln.style = if id <= nl { id } else { 0 };
                }
            }
            ShapeRecord::StraightEdge { delta } => {
                cursor.x += delta.dx.get();
                cursor.y += delta.dy.get();
                visit(&mut [&mut f1, &mut f0, &mut ln], cursor, false);
            }
            ShapeRecord::CurvedEdge {
                control_delta,
                anchor_delta,
            } => {
                cursor.x += control_delta.dx.get();
                cursor.y += control_delta.dy.get();
                visit(&mut [&mut f1, &mut f0, &mut ln], cursor, true);
                cursor.x += anchor_delta.dx.get();
                cursor.y += anchor_delta.dy.get();
                visit(&mut [&mut f1, &mut f0, &mut ln], cursor, false);
            }
        }
    }
    flush_fill(&mut f1, cursor, &mut fills, false);
    flush_fill(&mut f0, cursor, &mut fills, true);
    flush_stroke(&mut ln, cursor, &mut strokes);
    flush_layer(&mut fills, &mut strokes, fills_s, lines_s, rule, &mut out);
    out
}

fn visit(actives: &mut [&mut Active], p: Pt, control: bool) {
    for a in actives.iter_mut() {
        if a.style > 0 {
            a.seg.points.push(PathPoint { p, control });
        }
    }
}

/// How a draw is painted. Solid draws carry per-vertex colours.
#[derive(Clone, Debug, PartialEq)]
pub enum Paint {
    Solid,
    Gradient(FillStyle),
    Bitmap {
        id: u16,
        matrix: swf::Matrix,
        smooth: bool,
        repeat: bool,
    },
}

#[derive(Clone, Debug)]
pub struct RawDraw {
    pub paint: Paint,
    pub stroke: bool,
    /// Twips, shape-local.
    pub pos: Vec<[f32; 2]>,
    /// RGBA, only for `Paint::Solid`.
    pub color: Vec<[u8; 4]>,
    pub idx: Vec<u32>,
}

impl RawDraw {
    fn new(paint: Paint, stroke: bool) -> Self {
        RawDraw {
            paint,
            stroke,
            pos: Vec::new(),
            color: Vec::new(),
            idx: Vec::new(),
        }
    }
}

pub struct TessOptions {
    /// Maximum chord error, in twips.
    pub tolerance: f32,
    /// Hairline and thin strokes are widened to this many twips (one target pixel).
    pub min_stroke: f32,
}

#[derive(Default)]
pub struct TessStats {
    pub failures: u32,
}

fn lyon_path(cmds: &[Cmd], close: bool) -> LyonPath {
    let mut b = LyonPath::builder();
    let mut open = false;
    let pt = |p: &Pt| point(p.x as f32, p.y as f32);
    let mut start: Option<Pt> = None;
    for c in cmds {
        match c {
            Cmd::MoveTo(p) => {
                if open {
                    b.end(close);
                    open = false;
                }
                start = Some(*p);
            }
            Cmd::LineTo(p) => {
                if !open {
                    b.begin(pt(&start.unwrap_or(Pt { x: 0, y: 0 })));
                    open = true;
                }
                b.line_to(pt(p));
            }
            Cmd::QuadTo(c1, p) => {
                if !open {
                    b.begin(pt(&start.unwrap_or(Pt { x: 0, y: 0 })));
                    open = true;
                }
                b.quadratic_bezier_to(pt(c1), pt(p));
            }
        }
    }
    if open {
        b.end(close);
    }
    b.build()
}

fn paint_of(style: &FillStyle) -> (Paint, [u8; 4]) {
    match style {
        FillStyle::Color(c) => (Paint::Solid, [c.r, c.g, c.b, c.a]),
        FillStyle::Bitmap {
            id,
            matrix,
            is_smoothed,
            is_repeating,
        } => (
            Paint::Bitmap {
                id: *id,
                matrix: *matrix,
                smooth: *is_smoothed,
                repeat: *is_repeating,
            },
            [255; 4],
        ),
        g => (Paint::Gradient(g.clone()), [255; 4]),
    }
}

struct Ctor {
    color: [u8; 4],
}

impl lyon::tessellation::FillVertexConstructor<([f32; 2], [u8; 4])> for Ctor {
    fn new_vertex(&mut self, v: FillVertex) -> ([f32; 2], [u8; 4]) {
        let p = v.position();
        ([p.x, p.y], self.color)
    }
}

impl lyon::tessellation::StrokeVertexConstructor<([f32; 2], [u8; 4])> for Ctor {
    fn new_vertex(&mut self, v: StrokeVertex) -> ([f32; 2], [u8; 4]) {
        let p = v.position();
        ([p.x, p.y], self.color)
    }
}

/// Tessellates distilled paths. Consecutive solid fills (or solid strokes) share one draw.
pub fn tessellate(paths: &[DrawPath], opt: &TessOptions, stats: &mut TessStats) -> Vec<RawDraw> {
    let mut fill_tess = FillTessellator::new();
    let mut stroke_tess = StrokeTessellator::new();
    let mut draws: Vec<RawDraw> = Vec::new();
    for path in paths {
        let mut buf: VertexBuffers<([f32; 2], [u8; 4]), u32> = VertexBuffers::new();
        let (paint, stroke, result) = match path {
            DrawPath::Fill { style, cmds, rule } => {
                let (paint, color) = paint_of(style);
                let lp = lyon_path(cmds, true);
                let o = FillOptions::default()
                    .with_tolerance(opt.tolerance)
                    .with_fill_rule(*rule);
                let r = fill_tess.tessellate_path(
                    &lp,
                    &o,
                    &mut BuffersBuilder::new(&mut buf, Ctor { color }),
                );
                (paint, false, r.is_ok())
            }
            DrawPath::Stroke {
                style,
                cmds,
                closed,
            } => {
                let (paint, color) = paint_of(style.fill_style());
                let close = *closed && style.allow_close();
                let lp = lyon_path(cmds, close);
                let width = (style.width().get() as f32).max(opt.min_stroke);
                let cap = |c: LineCapStyle| match c {
                    LineCapStyle::None => LineCap::Butt,
                    LineCapStyle::Round => LineCap::Round,
                    LineCapStyle::Square => LineCap::Square,
                };
                let mut o = StrokeOptions::default()
                    .with_tolerance(opt.tolerance)
                    .with_line_width(width)
                    .with_start_cap(cap(style.start_cap()))
                    .with_end_cap(cap(style.end_cap()));
                o = match style.join_style() {
                    LineJoinStyle::Round => o.with_line_join(LineJoin::Round),
                    LineJoinStyle::Bevel => o.with_line_join(LineJoin::Bevel),
                    LineJoinStyle::Miter(limit) => {
                        let l = limit.to_f32();
                        if l >= StrokeOptions::MINIMUM_MITER_LIMIT {
                            o.with_line_join(LineJoin::MiterClip).with_miter_limit(l)
                        } else {
                            o.with_line_join(LineJoin::Bevel)
                        }
                    }
                };
                let r = stroke_tess.tessellate_path(
                    &lp,
                    &o,
                    &mut BuffersBuilder::new(&mut buf, Ctor { color }),
                );
                (paint, true, r.is_ok())
            }
        };
        if !result {
            stats.failures += 1;
            continue;
        }
        if buf.indices.len() < 3 {
            continue;
        }
        let merge = paint == Paint::Solid
            && draws
                .last()
                .is_some_and(|d| d.paint == Paint::Solid && d.stroke == stroke);
        if !merge {
            draws.push(RawDraw::new(paint, stroke));
        }
        let d = draws.last_mut().expect("draw pushed above");
        let base = d.pos.len() as u32;
        for (p, c) in &buf.vertices {
            d.pos.push(*p);
            if d.paint == Paint::Solid {
                d.color.push(*c);
            }
        }
        d.idx.extend(buf.indices.iter().map(|i| i + base));
    }
    draws
}

/// Bounds of all vertices, in twips: (x_min, y_min, x_max, y_max).
pub fn bounds(draws: &[RawDraw]) -> Option<[f32; 4]> {
    let mut b: Option<[f32; 4]> = None;
    for d in draws {
        for p in &d.pos {
            b = Some(match b {
                None => [p[0], p[1], p[0], p[1]],
                Some(r) => [
                    r[0].min(p[0]),
                    r[1].min(p[1]),
                    r[2].max(p[0]),
                    r[3].max(p[1]),
                ],
            });
        }
    }
    b
}

pub fn twips_rect(r: &swf::Rectangle<Twips>) -> [i32; 4] {
    [r.x_min.get(), r.y_min.get(), r.x_max.get(), r.y_max.get()]
}

#[cfg(test)]
mod tests {
    use super::*;
    use swf::{Color, PointDelta, StyleChangeData};

    fn move_to(x: i32, y: i32, f0: Option<u32>, f1: Option<u32>, l: Option<u32>) -> ShapeRecord {
        ShapeRecord::StyleChange(Box::new(StyleChangeData {
            move_to: Some(swf::Point::new(Twips::new(x), Twips::new(y))),
            fill_style_0: f0,
            fill_style_1: f1,
            line_style: l,
            new_styles: None,
        }))
    }
    fn line(dx: i32, dy: i32) -> ShapeRecord {
        ShapeRecord::StraightEdge {
            delta: PointDelta::new(Twips::new(dx), Twips::new(dy)),
        }
    }

    fn opts() -> TessOptions {
        TessOptions {
            tolerance: 2.0,
            min_stroke: 20.0,
        }
    }

    #[test]
    fn square_from_reversed_fill0_edges_links_and_fills() {
        // A 100x100 twip square drawn counter-clockwise with fill style 0 on its inside.
        let recs = vec![
            move_to(0, 0, Some(1), None, None),
            line(0, 100),
            line(100, 0),
            line(0, -100),
            line(-100, 0),
        ];
        let fills = vec![FillStyle::Color(Color::RED)];
        let paths = distill(&recs, &fills, &[], false);
        assert_eq!(paths.len(), 1);
        let mut st = TessStats::default();
        let draws = tessellate(&paths, &opts(), &mut st);
        assert_eq!(st.failures, 0);
        assert_eq!(draws.len(), 1);
        assert_eq!(draws[0].idx.len(), 6);
        assert_eq!(bounds(&draws), Some([0.0, 0.0, 100.0, 100.0]));
        assert!(draws[0].color.iter().all(|c| *c == [255, 0, 0, 255]));
    }

    #[test]
    fn edge_soup_is_linked_into_one_contour() {
        // Two halves of a square given out of order, both with fill style 1.
        let recs = vec![
            move_to(100, 100, None, Some(1), None),
            line(-100, 0),
            line(0, -100),
            move_to(0, 0, None, Some(1), None),
            line(100, 0),
            line(0, 100),
        ];
        let fills = vec![FillStyle::Color(Color::BLUE)];
        let paths = distill(&recs, &fills, &[], false);
        match &paths[0] {
            DrawPath::Fill { cmds, .. } => {
                assert_eq!(
                    cmds.iter().filter(|c| matches!(c, Cmd::MoveTo(_))).count(),
                    1
                )
            }
            _ => panic!("expected a fill"),
        }
    }

    #[test]
    fn hairline_is_widened_and_strokes_follow_fills() {
        let recs = vec![
            move_to(0, 0, None, Some(1), Some(1)),
            line(200, 0),
            line(0, 200),
            line(-200, -200),
        ];
        let fills = vec![FillStyle::Color(Color::GREEN)];
        let lines = vec![
            LineStyle::new()
                .with_width(Twips::ZERO)
                .with_color(Color::BLACK),
        ];
        let paths = distill(&recs, &fills, &lines, false);
        assert!(matches!(paths[0], DrawPath::Fill { .. }));
        assert!(matches!(paths[1], DrawPath::Stroke { closed: true, .. }));
        let draws = tessellate(&paths, &opts(), &mut TessStats::default());
        assert_eq!(draws.len(), 2);
        assert!(draws[1].stroke);
        let b = bounds(&draws[1..]).unwrap();
        // A 20-twip wide stroke extends 10 twips beyond the path.
        assert!(b[0] <= -9.0 && b[2] >= 209.0);
    }
}
