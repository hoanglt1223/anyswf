//! Morph shape interpolation: a DefineMorphShape at a given ratio becomes an ordinary
//! shape. Ported from Ruffle's core/src/display_object/morph_shape.rs (MIT/Apache-2.0).

use swf::{
    Color, FillStyle, Fixed8, Fixed16, Gradient, GradientRecord, LineStyle, Matrix, Point,
    ShapeRecord, Twips,
};

pub struct MorphFrame {
    pub fill_styles: Vec<FillStyle>,
    pub line_styles: Vec<LineStyle>,
    pub records: Vec<ShapeRecord>,
}

fn lerp_color(s: Color, e: Color, a: f32, b: f32) -> Color {
    Color {
        r: (a * s.r as f32 + b * e.r as f32) as u8,
        g: (a * s.g as f32 + b * e.g as f32) as u8,
        b: (a * s.b as f32 + b * e.b as f32) as u8,
        a: (a * s.a as f32 + b * e.a as f32) as u8,
    }
}

fn lerp_twips(s: Twips, e: Twips, a: f32, b: f32) -> Twips {
    Twips::new((s.get() as f32 * a + e.get() as f32 * b).round() as i32)
}

fn lerp_point(s: Point<Twips>, e: Point<Twips>, a: f32, b: f32) -> Point<Twips> {
    Point::new(lerp_twips(s.x, e.x, a, b), lerp_twips(s.y, e.y, a, b))
}

fn lerp_matrix(s: &Matrix, e: &Matrix, a: f32, b: f32) -> Matrix {
    let (af, bf) = (Fixed16::from_f32(a), Fixed16::from_f32(b));
    Matrix {
        a: s.a * af + e.a * bf,
        b: s.b * af + e.b * bf,
        c: s.c * af + e.c * bf,
        d: s.d * af + e.d * bf,
        tx: lerp_twips(s.tx, e.tx, a, b),
        ty: lerp_twips(s.ty, e.ty, a, b),
    }
}

fn lerp_gradient(s: &Gradient, e: &Gradient, a: f32, b: f32) -> Gradient {
    Gradient {
        matrix: lerp_matrix(&s.matrix, &e.matrix, a, b),
        spread: s.spread,
        interpolation: s.interpolation,
        records: s
            .records
            .iter()
            .zip(&e.records)
            .map(|(s, e)| GradientRecord {
                ratio: (s.ratio as f32 * a + e.ratio as f32 * b) as u8,
                color: lerp_color(s.color, e.color, a, b),
            })
            .collect(),
    }
}

fn lerp_fill(s: &FillStyle, e: &FillStyle, a: f32, b: f32) -> FillStyle {
    match (s, e) {
        (FillStyle::Color(s), FillStyle::Color(e)) => FillStyle::Color(lerp_color(*s, *e, a, b)),
        (
            FillStyle::Bitmap {
                id,
                matrix: sm,
                is_smoothed,
                is_repeating,
            },
            FillStyle::Bitmap { matrix: em, .. },
        ) => FillStyle::Bitmap {
            id: *id,
            matrix: lerp_matrix(sm, em, a, b),
            is_smoothed: *is_smoothed,
            is_repeating: *is_repeating,
        },
        (FillStyle::LinearGradient(s), FillStyle::LinearGradient(e)) => {
            FillStyle::LinearGradient(lerp_gradient(s, e, a, b))
        }
        (FillStyle::RadialGradient(s), FillStyle::RadialGradient(e)) => {
            FillStyle::RadialGradient(lerp_gradient(s, e, a, b))
        }
        (
            FillStyle::FocalGradient {
                gradient: s,
                focal_point: sf,
            },
            FillStyle::FocalGradient {
                gradient: e,
                focal_point: ef,
            },
        ) => FillStyle::FocalGradient {
            gradient: lerp_gradient(s, e, a, b),
            focal_point: *sf * Fixed8::from_f32(a) + *ef * Fixed8::from_f32(b),
        },
        _ => s.clone(),
    }
}

fn advance(x: &mut Point<Twips>, r: &ShapeRecord) {
    match r {
        ShapeRecord::StraightEdge { delta } => *x += *delta,
        ShapeRecord::CurvedEdge {
            control_delta,
            anchor_delta,
        } => {
            *x += *control_delta;
            *x += *anchor_delta;
        }
        ShapeRecord::StyleChange(sc) => {
            if let Some(m) = &sc.move_to {
                *x = *m;
            }
        }
    }
}

fn lerp_edge(
    sp: Point<Twips>,
    ep: Point<Twips>,
    s: &ShapeRecord,
    e: &ShapeRecord,
    a: f32,
    b: f32,
) -> ShapeRecord {
    let pen = lerp_point(sp, ep, a, b);
    // Straight edges are treated as curves with the control point at their midpoint.
    let curve = |pen: Point<Twips>, r: &ShapeRecord| -> Option<(Point<Twips>, Point<Twips>)> {
        match r {
            ShapeRecord::StraightEdge { delta } => Some((pen + *delta / 2, pen + *delta)),
            ShapeRecord::CurvedEdge {
                control_delta,
                anchor_delta,
            } => {
                let c = pen + *control_delta;
                Some((c, c + *anchor_delta))
            }
            ShapeRecord::StyleChange(_) => None,
        }
    };
    if let (ShapeRecord::StraightEdge { delta: sd }, ShapeRecord::StraightEdge { delta: ed }) =
        (s, e)
    {
        let anchor = lerp_point(sp + *sd, ep + *ed, a, b);
        return ShapeRecord::StraightEdge {
            delta: anchor - pen,
        };
    }
    let (Some((sc, sa)), Some((ec, ea))) = (curve(sp, s), curve(ep, e)) else {
        return s.clone();
    };
    let control = lerp_point(sc, ec, a, b);
    let anchor = lerp_point(sa, ea, a, b);
    ShapeRecord::CurvedEdge {
        control_delta: control - pen,
        anchor_delta: anchor - control,
    }
}

/// Ratio 0 is the start shape, 65535 the end shape.
pub fn frame(m: &swf::DefineMorphShape, ratio: u16) -> MorphFrame {
    let b = ratio as f32 / 65535.0;
    let a = 1.0 - b;
    let fill_styles = m
        .start
        .fill_styles
        .iter()
        .zip(&m.end.fill_styles)
        .map(|(s, e)| lerp_fill(s, e, a, b))
        .collect();
    let line_styles = m
        .start
        .line_styles
        .iter()
        .zip(&m.end.line_styles)
        .map(|(s, e)| {
            s.clone()
                .with_width(lerp_twips(s.width(), e.width(), a, b))
                .with_fill_style(lerp_fill(s.fill_style(), e.fill_style(), a, b))
        })
        .collect();
    let mut records = Vec::with_capacity(m.start.shape.len());
    let mut si = m.start.shape.iter();
    let mut ei = m.end.shape.iter();
    let (mut s, mut e) = (si.next(), ei.next());
    let mut sp = Point::new(Twips::ZERO, Twips::ZERO);
    let mut ep = sp;
    while let (Some(sr), Some(er)) = (s, e) {
        match (sr, er) {
            (ShapeRecord::StyleChange(sc), ShapeRecord::StyleChange(ec)) => {
                let mut out = sc.clone();
                if sc.move_to.is_some() || ec.move_to.is_some() {
                    if let Some(p) = sc.move_to {
                        sp = p;
                    }
                    if let Some(p) = ec.move_to {
                        ep = p;
                    }
                    out.move_to = Some(lerp_point(sp, ep, a, b));
                }
                records.push(ShapeRecord::StyleChange(out));
                s = si.next();
                e = ei.next();
            }
            (ShapeRecord::StyleChange(sc), _) => {
                let mut out = sc.clone();
                if let Some(p) = sc.move_to {
                    sp = p;
                    out.move_to = Some(lerp_point(sp, ep, a, b));
                }
                records.push(ShapeRecord::StyleChange(out));
                s = si.next();
            }
            (_, ShapeRecord::StyleChange(ec)) => {
                let mut out = ec.clone();
                if let Some(p) = ec.move_to {
                    ep = p;
                    out.move_to = Some(lerp_point(sp, ep, a, b));
                }
                records.push(ShapeRecord::StyleChange(out));
                e = ei.next();
            }
            _ => {
                records.push(lerp_edge(sp, ep, sr, er, a, b));
                advance(&mut sp, sr);
                advance(&mut ep, er);
                s = si.next();
                e = ei.next();
            }
        }
    }
    MorphFrame {
        fill_styles,
        line_styles,
        records,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use swf::{PointDelta, StyleChangeData};

    fn mv(x: i32) -> ShapeRecord {
        ShapeRecord::StyleChange(Box::new(StyleChangeData {
            move_to: Some(Point::new(Twips::new(x), Twips::ZERO)),
            fill_style_0: None,
            fill_style_1: Some(1),
            line_style: None,
            new_styles: None,
        }))
    }

    #[test]
    fn halfway_interpolates_points_and_colours() {
        let shape = |x: i32, d: i32, c: Color| swf::MorphShape {
            shape_bounds: Default::default(),
            edge_bounds: Default::default(),
            fill_styles: vec![FillStyle::Color(c)],
            line_styles: vec![],
            shape: vec![
                mv(x),
                ShapeRecord::StraightEdge {
                    delta: PointDelta::new(Twips::new(d), Twips::ZERO),
                },
            ],
        };
        let m = swf::DefineMorphShape {
            version: 1,
            id: 1,
            flags: swf::DefineMorphShapeFlag::empty(),
            start: shape(0, 100, Color::BLACK),
            end: shape(200, 300, Color::WHITE),
        };
        let f = frame(&m, 32768);
        match &f.records[0] {
            ShapeRecord::StyleChange(sc) => assert_eq!(sc.move_to.unwrap().x.get(), 100),
            _ => panic!(),
        }
        match &f.records[1] {
            ShapeRecord::StraightEdge { delta } => assert_eq!(delta.dx.get(), 200),
            _ => panic!(),
        }
        match f.fill_styles[0] {
            FillStyle::Color(c) => assert_eq!((c.r, c.g, c.b), (127, 127, 127)),
            _ => panic!(),
        }
        let start = frame(&m, 0);
        assert_eq!(start.fill_styles[0], FillStyle::Color(Color::BLACK));
    }
}
