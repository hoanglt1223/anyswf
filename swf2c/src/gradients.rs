//! Gradients become small baked textures: linear ones a 1D ramp addressed by u, radial
//! and focal ones a 2D image of the whole gradient square, because distance from the
//! centre does not interpolate linearly across a triangle. Ramp construction and the
//! focal formula follow Ruffle's wgpu renderer (MIT/Apache-2.0).

use crate::bitmaps::Image;
use swf::{FillStyle, Gradient, GradientInterpolation, GradientSpread};

pub const RAMP_SIZE: usize = 256;
pub const RADIAL_SIZE: usize = 64;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Wrap {
    Clamp,
    Repeat,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct GradientKey {
    radial: bool,
    focal_bits: i16,
    spread: u8,
    linear_rgb: bool,
    records: Vec<(u8, [u8; 4])>,
}

pub struct Baked {
    pub image: Image,
    pub wrap: Wrap,
    /// Linear gradients with reflect spread are baked as a forward+mirrored ramp of
    /// twice the period, so u is halved.
    pub u_scale: f32,
}

pub fn key(style: &FillStyle) -> Option<(GradientKey, &Gradient)> {
    let (g, radial, focal) = match style {
        FillStyle::LinearGradient(g) => (g, false, 0),
        FillStyle::RadialGradient(g) => (g, true, 0),
        FillStyle::FocalGradient {
            gradient,
            focal_point,
        } => (gradient, true, focal_point.get()),
        _ => return None,
    };
    Some((
        GradientKey {
            radial,
            focal_bits: focal,
            spread: g.spread as u8,
            linear_rgb: g.interpolation == GradientInterpolation::LinearRgb,
            records: g
                .records
                .iter()
                .map(|r| (r.ratio, [r.color.r, r.color.g, r.color.b, r.color.a]))
                .collect(),
        },
        g,
    ))
}

fn srgb_to_linear(c: f32) -> f32 {
    if c <= 0.04045 {
        c / 12.92
    } else {
        ((c + 0.055) / 1.055).powf(2.4)
    }
}

fn linear_to_srgb(c: f32) -> f32 {
    if c <= 0.0031308 {
        c * 12.92
    } else {
        1.055 * c.powf(1.0 / 2.4) - 0.055
    }
}

fn ramp(k: &GradientKey) -> Vec<[u8; 4]> {
    let recs = &k.records;
    if recs.is_empty() {
        return vec![[0; 4]; RAMP_SIZE];
    }
    let conv = |c: u8| {
        if k.linear_rgb {
            srgb_to_linear(c as f32 / 255.0) * 255.0
        } else {
            c as f32
        }
    };
    let back = |c: f32| {
        if k.linear_rgb {
            linear_to_srgb(c / 255.0) * 255.0
        } else {
            c
        }
    };
    let mut out = Vec::with_capacity(RAMP_SIZE);
    let mut last = 0usize;
    for t in 0..RAMP_SIZE {
        if last + 1 < recs.len() && t > recs[last + 1].0 as usize {
            last += 1;
        }
        let next = (last + 1).min(recs.len() - 1);
        let (lr, lc) = recs[last];
        let (nr, nc) = recs[next];
        let a = if t <= lr as usize || lr == nr {
            0.0
        } else if t > nr as usize {
            1.0
        } else {
            (t as f32 - lr as f32) / (nr as f32 - lr as f32)
        };
        let mut px = [0u8; 4];
        for i in 0..3 {
            let v = conv(lc[i]) + (conv(nc[i]) - conv(lc[i])) * a;
            px[i] = back(v).round().clamp(0.0, 255.0) as u8;
        }
        px[3] = (lc[3] as f32 + (nc[3] as f32 - lc[3] as f32) * a).round() as u8;
        out.push(px);
    }
    out
}

fn sample(r: &[[u8; 4]], t: f32) -> [u8; 4] {
    let i = (t.clamp(0.0, 1.0) * (RAMP_SIZE - 1) as f32).round() as usize;
    r[i]
}

pub fn bake(k: &GradientKey) -> Baked {
    let r = ramp(k);
    let spread = GradientSpread::from_u8(k.spread).unwrap_or(GradientSpread::Pad);
    if !k.radial {
        return match spread {
            GradientSpread::Pad => Baked {
                image: Image::from_pixels(RAMP_SIZE as u32, 1, &r),
                wrap: Wrap::Clamp,
                u_scale: 1.0,
            },
            GradientSpread::Repeat => Baked {
                image: Image::from_pixels(RAMP_SIZE as u32, 1, &r),
                wrap: Wrap::Repeat,
                u_scale: 1.0,
            },
            GradientSpread::Reflect => {
                let mut px = r.clone();
                px.extend(r.iter().rev());
                Baked {
                    image: Image::from_pixels(2 * RAMP_SIZE as u32, 1, &px),
                    wrap: Wrap::Repeat,
                    u_scale: 0.5,
                }
            }
        };
    }
    let focal = (k.focal_bits as f32 / 256.0).clamp(-0.98, 0.98);
    let n = RADIAL_SIZE;
    let mut px = Vec::with_capacity(n * n);
    for y in 0..n {
        for x in 0..n {
            let ux = (x as f32 + 0.5) / n as f32 * 2.0 - 1.0;
            let uy = (y as f32 + 0.5) / n as f32 * 2.0 - 1.0;
            let mut t = if focal == 0.0 {
                (ux * ux + uy * uy).sqrt()
            } else {
                let (dx, dy) = (focal - ux, -uy);
                let l = (dx * dx + dy * dy).sqrt();
                if l == 0.0 {
                    0.0
                } else {
                    let (nx, ny) = (dx / l, dy / l);
                    l / ((1.0 - focal * focal * ny * ny).sqrt() + focal * nx)
                }
            };
            t = match spread {
                GradientSpread::Pad => t,
                GradientSpread::Repeat => t.fract(),
                GradientSpread::Reflect => {
                    if (t as i32) & 1 == 0 {
                        t.fract()
                    } else {
                        1.0 - t.fract()
                    }
                }
            };
            px.push(sample(&r, t));
        }
    }
    Baked {
        image: Image::from_pixels(n as u32, n as u32, &px),
        wrap: Wrap::Clamp,
        u_scale: 1.0,
    }
}

/// Inverse of a SWF matrix as (a, b, c, d, tx, ty) in f64, mapping twips back to the
/// matrix's source space. Degenerate matrices map everything to the origin.
pub fn invert(m: &swf::Matrix) -> [f64; 6] {
    let (a, b, c, d) = (m.a.to_f64(), m.b.to_f64(), m.c.to_f64(), m.d.to_f64());
    let (tx, ty) = (m.tx.get() as f64, m.ty.get() as f64);
    let det = a * d - b * c;
    if det.abs() < 1e-12 {
        return [0.0; 6];
    }
    let (ia, ib, ic, id) = (d / det, -b / det, -c / det, a / det);
    [ia, ib, ic, id, -(ia * tx + ic * ty), -(ib * tx + id * ty)]
}

pub fn apply(inv: &[f64; 6], p: [f32; 2]) -> [f64; 2] {
    let (x, y) = (p[0] as f64, p[1] as f64);
    [
        inv[0] * x + inv[2] * y + inv[4],
        inv[1] * x + inv[3] * y + inv[5],
    ]
}

/// Gradient UV in [0,1] over the gradient square (-16384..16384 gradient units).
pub fn uv(g: &Gradient, p: [f32; 2]) -> [f32; 2] {
    let q = apply(&invert(&g.matrix), p);
    [(q[0] / 32768.0 + 0.5) as f32, (q[1] / 32768.0 + 0.5) as f32]
}

#[cfg(test)]
mod tests {
    use super::*;
    use swf::{Color, Fixed16, GradientRecord, Matrix, Twips};

    fn grad(records: &[(u8, Color)]) -> Gradient {
        Gradient {
            matrix: Matrix::IDENTITY,
            spread: GradientSpread::Pad,
            interpolation: GradientInterpolation::Rgb,
            records: records
                .iter()
                .map(|(ratio, color)| GradientRecord {
                    ratio: *ratio,
                    color: *color,
                })
                .collect(),
        }
    }

    #[test]
    fn linear_ramp_endpoints_and_midpoint() {
        let g = grad(&[(0, Color::BLACK), (255, Color::WHITE)]);
        let (k, _) = key(&FillStyle::LinearGradient(g)).unwrap();
        let b = bake(&k);
        assert_eq!((b.image.width, b.image.height), (256, 1));
        assert_eq!(b.image.pixel(0, 0), [0, 0, 0, 255]);
        assert_eq!(b.image.pixel(255, 0), [255, 255, 255, 255]);
        let mid = b.image.pixel(128, 0)[0];
        assert!((126..=130).contains(&mid), "{mid}");
    }

    #[test]
    fn colours_hold_outside_the_record_range() {
        let g = grad(&[(64, Color::RED), (192, Color::BLUE)]);
        let (k, _) = key(&FillStyle::LinearGradient(g)).unwrap();
        let b = bake(&k);
        assert_eq!(b.image.pixel(10, 0), [255, 0, 0, 255]);
        assert_eq!(b.image.pixel(250, 0), [0, 0, 255, 255]);
    }

    #[test]
    fn radial_centre_and_corner() {
        let g = grad(&[(0, Color::WHITE), (255, Color::BLACK)]);
        let (k, _) = key(&FillStyle::RadialGradient(g)).unwrap();
        let b = bake(&k);
        let c = b.image.pixel(32, 32)[0];
        assert!(c > 245, "{c}");
        assert_eq!(b.image.pixel(0, 0), [0, 0, 0, 255]);
    }

    #[test]
    fn uv_maps_gradient_square_to_unit_square() {
        // Typical Flash gradient matrix: 100px wide box at (50,50): scale 2000/32768.
        let s = 2000.0 / 32768.0;
        let mut g = grad(&[(0, Color::BLACK)]);
        g.matrix = Matrix {
            a: Fixed16::from_f64(s),
            d: Fixed16::from_f64(s),
            tx: Twips::new(2000),
            ty: Twips::new(2000),
            ..Matrix::IDENTITY
        };
        let uv0 = uv(&g, [1000.0, 1000.0]);
        let uv1 = uv(&g, [3000.0, 3000.0]);
        assert!(uv0[0].abs() < 1e-3 && uv0[1].abs() < 1e-3, "{uv0:?}");
        assert!((uv1[0] - 1.0).abs() < 1e-3, "{uv1:?}");
    }
}
