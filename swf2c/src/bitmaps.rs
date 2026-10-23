//! Bitmap decoding (DefineBits*, DefineBitsLossless*) and conversion to GE textures:
//! scaled for the target, quantised to CLUT8 when that is close enough to lossless,
//! padded to powers of two, split above 512x512, and swizzled.
//!
//! Decoding follows Ruffle's render/src/utils.rs (MIT/Apache-2.0). Unlike Ruffle we keep
//! colours straight (not premultiplied) because the GE blends with straight alpha.

use crate::gradients::Wrap;
use crate::{Error, Result};
use std::collections::HashMap;
use std::io::Read;

pub const MAX_TEX: u32 = 512;

/// Straight-alpha RGBA8 image.
#[derive(Clone, Debug, PartialEq)]
pub struct Image {
    pub width: u32,
    pub height: u32,
    pub rgba: Vec<u8>,
}

impl Image {
    pub fn from_pixels(width: u32, height: u32, px: &[[u8; 4]]) -> Self {
        Image {
            width,
            height,
            rgba: px.iter().flatten().copied().collect(),
        }
    }

    pub fn pixel(&self, x: u32, y: u32) -> [u8; 4] {
        let i = ((y * self.width + x) * 4) as usize;
        [
            self.rgba[i],
            self.rgba[i + 1],
            self.rgba[i + 2],
            self.rgba[i + 3],
        ]
    }

    /// Grows the image to w x h by repeating its last column and row.
    pub fn pad_replicate(&self, w: u32, h: u32) -> Image {
        let mut rgba = Vec::with_capacity((w * h * 4) as usize);
        for y in 0..h {
            let sy = y.min(self.height - 1);
            for x in 0..w {
                rgba.extend_from_slice(&self.pixel(x.min(self.width - 1), sy));
            }
        }
        Image {
            width: w,
            height: h,
            rgba,
        }
    }

    pub fn crop(&self, x0: u32, y0: u32, w: u32, h: u32) -> Image {
        let mut rgba = Vec::with_capacity((w * h * 4) as usize);
        for y in y0..y0 + h {
            let s = ((y * self.width + x0) * 4) as usize;
            rgba.extend_from_slice(&self.rgba[s..s + (w * 4) as usize]);
        }
        Image {
            width: w,
            height: h,
            rgba,
        }
    }

    /// Area-averaged resample, weighted by alpha so transparent pixels do not bleed colour.
    pub fn resize(&self, w: u32, h: u32) -> Image {
        if w == self.width && h == self.height {
            return self.clone();
        }
        let mut out = Vec::with_capacity((w * h * 4) as usize);
        let sx = self.width as f64 / w as f64;
        let sy = self.height as f64 / h as f64;
        for oy in 0..h {
            let y0 = oy as f64 * sy;
            let y1 = y0 + sy;
            for ox in 0..w {
                let x0 = ox as f64 * sx;
                let x1 = x0 + sx;
                let mut acc = [0f64; 4];
                let mut area = 0f64;
                let mut y = y0.floor() as u32;
                while (y as f64) < y1 && y < self.height {
                    let wy = (y1.min(y as f64 + 1.0) - y0.max(y as f64)).max(0.0);
                    let mut x = x0.floor() as u32;
                    while (x as f64) < x1 && x < self.width {
                        let wx = (x1.min(x as f64 + 1.0) - x0.max(x as f64)).max(0.0);
                        let wgt = wx * wy;
                        let p = self.pixel(x, y);
                        let a = p[3] as f64 / 255.0;
                        acc[0] += p[0] as f64 * a * wgt;
                        acc[1] += p[1] as f64 * a * wgt;
                        acc[2] += p[2] as f64 * a * wgt;
                        acc[3] += a * wgt;
                        area += wgt;
                        x += 1;
                    }
                    y += 1;
                }
                if area == 0.0 || acc[3] == 0.0 {
                    out.extend_from_slice(&[0, 0, 0, 0]);
                } else {
                    let a = acc[3] / area;
                    out.extend_from_slice(&[
                        (acc[0] / acc[3]).round().clamp(0.0, 255.0) as u8,
                        (acc[1] / acc[3]).round().clamp(0.0, 255.0) as u8,
                        (acc[2] / acc[3]).round().clamp(0.0, 255.0) as u8,
                        (a * 255.0).round().clamp(0.0, 255.0) as u8,
                    ]);
                }
            }
        }
        Image {
            width: w,
            height: h,
            rgba: out,
        }
    }
}

fn zlib(data: &[u8]) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    flate2::read::ZlibDecoder::new(data)
        .read_to_end(&mut out)
        .map_err(|e| Error::new(format!("bad zlib data in bitmap: {e}")))?;
    Ok(out)
}

fn check_size(w: u32, h: u32) -> Result<()> {
    if w == 0 || h == 0 || (w as u64) * (h as u64) > 16 * 1024 * 1024 {
        return Err(Error::new(format!("unreasonable bitmap size {w}x{h}")));
    }
    Ok(())
}

pub fn decode_lossless(tag: &swf::DefineBitsLossless) -> Result<Image> {
    let (w, h) = (tag.width as u32, tag.height as u32);
    check_size(w, h)?;
    let data = zlib(&tag.data)?;
    let alpha = tag.version == 2;
    let short = || Error::new(format!("bitmap {} pixel data is truncated", tag.id));
    let mut rgba = Vec::with_capacity((w * h * 4) as usize);
    match tag.format {
        swf::BitmapFormat::Rgb15 => {
            let pitch = ((w + 1) & !1) as usize * 2;
            for y in 0..h as usize {
                for x in 0..w as usize {
                    let i = y * pitch + x * 2;
                    let v = u16::from_be_bytes([
                        *data.get(i).ok_or_else(short)?,
                        *data.get(i + 1).ok_or_else(short)?,
                    ]);
                    let c = |s: u16| (((v >> s) & 0x1F) * 255 / 31) as u8;
                    rgba.extend_from_slice(&[c(10), c(5), c(0), 255]);
                }
            }
        }
        swf::BitmapFormat::Rgb32 => {
            let need = (w * h * 4) as usize;
            if data.len() < need {
                return Err(short());
            }
            for px in data[..need].chunks_exact(4) {
                let a = if alpha { px[0] } else { 255 };
                rgba.extend_from_slice(&unmultiply([px[1], px[2], px[3], a], alpha));
            }
        }
        swf::BitmapFormat::ColorMap8 { num_colors } => {
            let n = num_colors as usize + 1;
            let entry = if alpha { 4 } else { 3 };
            if data.len() < n * entry {
                return Err(short());
            }
            let pal: Vec<[u8; 4]> = (0..n)
                .map(|i| {
                    let e = &data[i * entry..];
                    let a = if alpha { e[3] } else { 255 };
                    unmultiply([e[0], e[1], e[2], a], alpha)
                })
                .collect();
            let pitch = ((w + 3) & !3) as usize;
            let base = n * entry;
            let missing = if alpha { [0, 0, 0, 0] } else { [0, 0, 0, 255] };
            for y in 0..h as usize {
                for x in 0..w as usize {
                    let ix = *data.get(base + y * pitch + x).ok_or_else(short)? as usize;
                    rgba.extend_from_slice(pal.get(ix).unwrap_or(&missing));
                }
            }
        }
    }
    Ok(Image {
        width: w,
        height: h,
        rgba,
    })
}

fn unmultiply(p: [u8; 4], premultiplied: bool) -> [u8; 4] {
    if !premultiplied || p[3] == 255 {
        return p;
    }
    if p[3] == 0 {
        return [0, 0, 0, 0];
    }
    let a = p[3] as u32;
    let f = |c: u8| ((c as u32 * 255 + a / 2) / a).min(255) as u8;
    [f(p[0]), f(p[1]), f(p[2]), p[3]]
}

/// Removes the erroneous EOI+SOI pair Flash tolerates before the SOF marker and makes
/// sure the stream ends with EOI (see Ruffle's `remove_invalid_jpeg_data`).
fn clean_jpeg(data: &[u8]) -> Vec<u8> {
    let mut out: Vec<u8> = if let Some(s) = data.strip_prefix(&[0xFF, 0xD9, 0xFF, 0xD8]) {
        s.to_vec()
    } else {
        let mut pos = 0usize;
        let mut found = None;
        while pos + 4 <= data.len() {
            match &data[pos..pos + 4] {
                [0xFF, 0xD9, 0xFF, 0xD8] => {
                    found = Some(pos);
                    break;
                }
                [0xFF, 0xD0..=0xD9, _, _] => pos += 2,
                [0xFF, 0xC0, _, _] => break,
                [0xFF, _, a, b] => pos += 2 + u16::from_be_bytes([*a, *b]) as usize,
                _ => break,
            }
        }
        match found {
            Some(p) => [&data[..p], &data[p + 4..]].concat(),
            None => data.to_vec(),
        }
    };
    if !out.ends_with(&[0xFF, 0xD9]) {
        out.extend_from_slice(&[0xFF, 0xD9]);
    }
    out
}

fn decode_jpeg(data: &[u8], alpha: Option<&[u8]>) -> Result<Image> {
    let data = clean_jpeg(data);
    let mut dec = jpeg_decoder::Decoder::new(&data[..]);
    let px = dec
        .decode()
        .map_err(|e| Error::new(format!("cannot decode JPEG: {e}")))?;
    let info = dec
        .info()
        .ok_or_else(|| Error::new("JPEG has no frame header"))?;
    let (w, h) = (info.width as u32, info.height as u32);
    check_size(w, h)?;
    let n = (w * h) as usize;
    let mut rgba = Vec::with_capacity(n * 4);
    match info.pixel_format {
        jpeg_decoder::PixelFormat::RGB24 => {
            for p in px.chunks_exact(3).take(n) {
                rgba.extend_from_slice(&[p[0], p[1], p[2], 255]);
            }
        }
        jpeg_decoder::PixelFormat::L8 => {
            for &v in px.iter().take(n) {
                rgba.extend_from_slice(&[v, v, v, 255]);
            }
        }
        jpeg_decoder::PixelFormat::CMYK32 => {
            for p in px.chunks_exact(4).take(n) {
                let k = 255 - p[3] as u32;
                let f = |c: u8| ((255 - c as u32) * k / 255) as u8;
                rgba.extend_from_slice(&[f(p[0]), f(p[1]), f(p[2]), 255]);
            }
        }
        jpeg_decoder::PixelFormat::L16 => {
            for p in px.chunks_exact(2).take(n) {
                rgba.extend_from_slice(&[p[0], p[0], p[0], 255]);
            }
        }
    }
    if rgba.len() != n * 4 {
        return Err(Error::new("JPEG pixel data is truncated"));
    }
    if let Some(alpha) = alpha.filter(|a| !a.is_empty()) {
        let a = zlib(alpha)?;
        if a.len() >= n {
            // JPEG3 colour is premultiplied by the separate alpha plane; Flash clamps
            // colour to alpha first (Ruffle #6893).
            for i in 0..n {
                let av = a[i];
                let p = &mut rgba[i * 4..i * 4 + 4];
                let pm = [p[0].min(av), p[1].min(av), p[2].min(av), av];
                p.copy_from_slice(&unmultiply(pm, true));
            }
        }
    }
    Ok(Image {
        width: w,
        height: h,
        rgba,
    })
}

fn decode_png(data: &[u8]) -> Result<Image> {
    let mut dec = png::Decoder::new(std::io::Cursor::new(data));
    dec.set_transformations(png::Transformations::normalize_to_color8());
    let mut reader = dec
        .read_info()
        .map_err(|e| Error::new(format!("cannot decode PNG: {e}")))?;
    let size = reader
        .output_buffer_size()
        .ok_or_else(|| Error::new("PNG too large"))?;
    let mut buf = vec![0; size];
    let info = reader
        .next_frame(&mut buf)
        .map_err(|e| Error::new(format!("cannot decode PNG: {e}")))?;
    check_size(info.width, info.height)?;
    let n = (info.width * info.height) as usize;
    let mut rgba = Vec::with_capacity(n * 4);
    match info.color_type {
        png::ColorType::Rgba => rgba.extend_from_slice(&buf[..n * 4]),
        png::ColorType::Rgb => buf[..n * 3]
            .chunks_exact(3)
            .for_each(|p| rgba.extend_from_slice(&[p[0], p[1], p[2], 255])),
        png::ColorType::Grayscale => buf[..n]
            .iter()
            .for_each(|&v| rgba.extend_from_slice(&[v, v, v, 255])),
        png::ColorType::GrayscaleAlpha => buf[..n * 2]
            .chunks_exact(2)
            .for_each(|p| rgba.extend_from_slice(&[p[0], p[0], p[0], p[1]])),
        png::ColorType::Indexed => return Err(Error::new("unexpected indexed PNG output")),
    }
    Ok(Image {
        width: info.width,
        height: info.height,
        rgba,
    })
}

fn decode_gif(data: &[u8]) -> Result<Image> {
    let mut opts = gif::DecodeOptions::new();
    opts.set_color_output(gif::ColorOutput::RGBA);
    let mut r = opts
        .read_info(data)
        .map_err(|e| Error::new(format!("cannot decode GIF: {e}")))?;
    let f = r
        .read_next_frame()
        .map_err(|e| Error::new(format!("cannot decode GIF: {e}")))?
        .ok_or_else(|| Error::new("GIF has no frames"))?;
    check_size(f.width as u32, f.height as u32)?;
    Ok(Image {
        width: f.width as u32,
        height: f.height as u32,
        rgba: f.buffer.to_vec(),
    })
}

/// DefineBitsJPEG2/3 may carry JPEG, PNG or GIF data.
pub fn decode_jpeg_tag(data: &[u8], alpha: Option<&[u8]>) -> Result<Image> {
    match data {
        [0x89, b'P', b'N', b'G', ..] => decode_png(data),
        [b'G', b'I', b'F', b'8', ..] => decode_gif(data),
        _ => decode_jpeg(data, alpha),
    }
}

/// DefineBits: JPEG data without tables; the shared JPEGTables tag supplies them.
pub fn decode_jpeg1(data: &[u8], tables: Option<&[u8]>) -> Result<Image> {
    match tables {
        Some(t) if t.len() >= 2 && data.len() >= 2 => {
            let mut full = t[..t.len() - 2].to_vec();
            full.extend_from_slice(&data[2..]);
            decode_jpeg(&full, None)
        }
        _ => decode_jpeg(data, None),
    }
}

/// GE pixel formats (values match GU_PSM_*).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum TexFormat {
    Rgb565 = 0,
    Rgba5551 = 1,
    Rgba4444 = 2,
    Rgba8888 = 3,
    Clut8 = 5,
}

impl TexFormat {
    pub fn bits(self) -> u32 {
        match self {
            TexFormat::Clut8 => 8,
            TexFormat::Rgba8888 => 32,
            _ => 16,
        }
    }
}

#[derive(Clone, Debug)]
pub struct Texture {
    pub format: TexFormat,
    pub width: u32,
    pub height: u32,
    pub used_w: u32,
    pub used_h: u32,
    pub swizzled: bool,
    pub wrap: Wrap,
    pub data: Vec<u8>,
    /// RGBA8888 palette for CLUT8, padded to a multiple of 16 entries.
    pub clut: Vec<u8>,
}

pub fn next_pow2(v: u32) -> u32 {
    v.max(1).next_power_of_two()
}

fn pack(format: TexFormat, p: [u8; 4]) -> u32 {
    let [r, g, b, a] = p.map(|c| c as u32);
    match format {
        TexFormat::Rgb565 => (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11),
        TexFormat::Rgba5551 => (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15),
        TexFormat::Rgba4444 => (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12),
        TexFormat::Rgba8888 => r | (g << 8) | (b << 16) | (a << 24),
        TexFormat::Clut8 => unreachable!("CLUT8 is packed through its palette"),
    }
}

/// Median-cut palette of at most 256 colours plus the index of each pixel.
fn quantize(img: &Image) -> (Vec<[u8; 4]>, Vec<u8>) {
    let mut counts: HashMap<[u8; 4], u32> = HashMap::new();
    for p in img.rgba.chunks_exact(4) {
        *counts.entry([p[0], p[1], p[2], p[3]]).or_insert(0) += 1;
    }
    let mut colors: Vec<([u8; 4], u32)> = counts.into_iter().collect();
    colors.sort_unstable();
    let palette: Vec<[u8; 4]> = if colors.len() <= 256 {
        colors.iter().map(|c| c.0).collect()
    } else {
        fn score(b: &[([u8; 4], u32)]) -> (usize, u64) {
            let (axis, range) = (0..4)
                .map(|ax| {
                    let lo = b.iter().map(|c| c.0[ax]).min().unwrap_or(0);
                    let hi = b.iter().map(|c| c.0[ax]).max().unwrap_or(0);
                    (ax, (hi - lo) as u64)
                })
                .max_by_key(|x| x.1)
                .unwrap_or((0, 0));
            if b.len() < 2 {
                return (axis, 0);
            }
            let weight: u64 = b.iter().map(|c| c.1 as u64).sum();
            (axis, range * ((weight as f64).sqrt() as u64 + 1))
        }
        let first = score(&colors);
        type ColorBox = (Vec<([u8; 4], u32)>, usize, u64);
        let mut boxes: Vec<ColorBox> = vec![(colors, first.0, first.1)];
        while boxes.len() < 256 {
            let Some((bi, _)) = boxes.iter().enumerate().max_by_key(|(_, b)| b.2) else {
                break;
            };
            if boxes[bi].2 == 0 {
                break;
            }
            let (mut b, axis, _) = boxes.swap_remove(bi);
            b.sort_unstable_by_key(|c| c.0[axis]);
            let total: u64 = b.iter().map(|c| c.1 as u64).sum();
            let mut acc = 0u64;
            let mut split = 1;
            for (i, c) in b.iter().enumerate() {
                acc += c.1 as u64;
                if acc * 2 >= total {
                    split = (i + 1).clamp(1, b.len() - 1);
                    break;
                }
            }
            let hi = b.split_off(split);
            for part in [b, hi] {
                let (ax, sc) = score(&part);
                boxes.push((part, ax, sc));
            }
        }
        boxes
            .iter()
            .map(|(b, _, _)| {
                let w: f64 = b.iter().map(|c| c.1 as f64).sum();
                let mut m = [0f64; 4];
                for c in b {
                    for (k, mk) in m.iter_mut().enumerate() {
                        *mk += c.0[k] as f64 * c.1 as f64;
                    }
                }
                m.map(|v| (v / w).round() as u8)
            })
            .collect()
    };
    let mut cache: HashMap<[u8; 4], u8> = HashMap::new();
    let idx = img
        .rgba
        .chunks_exact(4)
        .map(|p| {
            let c = [p[0], p[1], p[2], p[3]];
            *cache.entry(c).or_insert_with(|| {
                palette
                    .iter()
                    .enumerate()
                    .min_by_key(|(_, q)| {
                        (0..4)
                            .map(|k| (c[k] as i32 - q[k] as i32).pow(2))
                            .sum::<i32>()
                    })
                    .map(|(i, _)| i as u8)
                    .unwrap_or(0)
            })
        })
        .collect();
    (palette, idx)
}

fn psnr(img: &Image, palette: &[[u8; 4]], idx: &[u8]) -> f64 {
    let mut se = 0f64;
    for (p, &i) in img.rgba.chunks_exact(4).zip(idx) {
        let q = palette[i as usize];
        for k in 0..4 {
            se += ((p[k] as f64) - (q[k] as f64)).powi(2);
        }
    }
    let mse = se / (img.rgba.len() as f64);
    if mse == 0.0 {
        f64::INFINITY
    } else {
        10.0 * (255.0f64 * 255.0 / mse).log10()
    }
}

/// Minimum PSNR (dB) at which a CLUT8 version counts as close enough to lossless.
pub const CLUT_MIN_PSNR: f64 = 40.0;

/// Swizzles a linear texture into the GE's 16-byte x 8-row block order.
pub fn swizzle(data: &[u8], width_bytes: usize, height: usize) -> Vec<u8> {
    let mut out = Vec::with_capacity(data.len());
    for by in 0..height / 8 {
        for bx in 0..width_bytes / 16 {
            for row in 0..8 {
                let s = (by * 8 + row) * width_bytes + bx * 16;
                out.extend_from_slice(&data[s..s + 16]);
            }
        }
    }
    out
}

pub fn unswizzle(data: &[u8], width_bytes: usize, height: usize) -> Vec<u8> {
    let mut out = vec![0; data.len()];
    let mut i = 0;
    for by in 0..height / 8 {
        for bx in 0..width_bytes / 16 {
            for row in 0..8 {
                let d = (by * 8 + row) * width_bytes + bx * 16;
                out[d..d + 16].copy_from_slice(&data[i..i + 16]);
                i += 16;
            }
        }
    }
    out
}

/// Encodes an image of at most 512x512 as a texture. `stretch` resamples to the
/// power-of-two size (needed for repeating fills); otherwise the image is padded.
/// `force` picks a format instead of the automatic choice.
pub fn encode(img: &Image, stretch: bool, wrap: Wrap, force: Option<TexFormat>) -> Texture {
    let src = if stretch {
        img.resize(next_pow2(img.width), next_pow2(img.height))
    } else {
        img.clone()
    };
    let (used_w, used_h) = (src.width, src.height);
    let format = force.unwrap_or_else(|| auto_format(&src));
    let bpp_bytes = (format.bits() / 8).max(1) as usize;
    // The GE wants rows of at least 16 bytes; padding replicates the edge texels so
    // filtering at the image border does not pull in padding.
    let pw = next_pow2(used_w).max(16 / bpp_bytes as u32);
    let ph = next_pow2(used_h);
    let padded = src.pad_replicate(pw, ph);
    let mut clut = Vec::new();
    let texels: Vec<u8> = if format == TexFormat::Clut8 {
        let (pal, idx) = quantize(&padded);
        let n = pal.len().div_ceil(16).max(1) * 16;
        for i in 0..n {
            clut.extend_from_slice(&pal.get(i).copied().unwrap_or([0; 4]));
        }
        idx
    } else {
        let mut t = Vec::with_capacity((pw * ph) as usize * bpp_bytes);
        for p in padded.rgba.chunks_exact(4) {
            let v = pack(format, [p[0], p[1], p[2], p[3]]);
            t.extend_from_slice(&v.to_le_bytes()[..bpp_bytes]);
        }
        t
    };
    let width_bytes = pw as usize * bpp_bytes;
    let swizzled = ph >= 8;
    let data = if swizzled {
        swizzle(&texels, width_bytes, ph as usize)
    } else {
        texels
    };
    Texture {
        format,
        width: pw,
        height: ph,
        used_w,
        used_h,
        swizzled,
        wrap,
        data,
        clut,
    }
}

fn auto_format(img: &Image) -> TexFormat {
    let (pal, idx) = quantize(img);
    if psnr(img, &pal, &idx) >= CLUT_MIN_PSNR {
        return TexFormat::Clut8;
    }
    let alphas = img.rgba.chunks_exact(4).map(|p| p[3]);
    let mut opaque = true;
    let mut binary = true;
    for a in alphas {
        opaque &= a == 255;
        binary &= a == 0 || a == 255;
    }
    if opaque {
        TexFormat::Rgb565
    } else if binary {
        TexFormat::Rgba5551
    } else {
        TexFormat::Rgba4444
    }
}

/// A bitmap converted for the target: one texture, or a grid of tiles when larger than
/// 512 in either direction after scaling.
#[derive(Clone, Debug)]
pub struct Converted {
    /// Original size in bitmap pixels (what fill matrices refer to).
    pub orig_w: u32,
    pub orig_h: u32,
    /// Size after target scaling.
    pub img_w: u32,
    pub img_h: u32,
    pub stretched: bool,
    pub tiles: Vec<Tile>,
}

#[derive(Clone, Debug)]
pub struct Tile {
    /// Region of the scaled image this tile covers.
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
    pub texture: Texture,
}

pub fn convert(img: &Image, scale: f32, repeating: bool) -> Converted {
    let s = scale.clamp(0.0, 1.0);
    let mut w = ((img.width as f32 * s).round() as u32).max(1);
    let mut h = ((img.height as f32 * s).round() as u32).max(1);
    if repeating {
        // A repeating fill needs one texture whose size is the period, so cap at 512.
        w = w.min(MAX_TEX);
        h = h.min(MAX_TEX);
    }
    let scaled = img.resize(w, h);
    let wrap = if repeating { Wrap::Repeat } else { Wrap::Clamp };
    let mut tiles = Vec::new();
    let mut y = 0;
    while y < h {
        let th = (h - y).min(MAX_TEX);
        let mut x = 0;
        while x < w {
            let tw = (w - x).min(MAX_TEX);
            let part = if tw == w && th == h {
                scaled.clone()
            } else {
                scaled.crop(x, y, tw, th)
            };
            tiles.push(Tile {
                x,
                y,
                w: tw,
                h: th,
                texture: encode(&part, repeating, wrap, None),
            });
            x += tw;
        }
        y += th;
    }
    Converted {
        orig_w: img.width,
        orig_h: img.height,
        img_w: w,
        img_h: h,
        stretched: repeating,
        tiles,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn checker(w: u32, h: u32) -> Image {
        let mut px = Vec::new();
        for y in 0..h {
            for x in 0..w {
                px.push(if (x + y) % 2 == 0 {
                    [255, 0, 0, 255]
                } else {
                    [0, 0, 255, 255]
                });
            }
        }
        Image::from_pixels(w, h, &px)
    }

    #[test]
    fn swizzle_round_trips() {
        let data: Vec<u8> = (0..64 * 16).map(|i| i as u8).collect();
        let s = swizzle(&data, 64, 16);
        assert_ne!(s, data);
        assert_eq!(&s[..16], &data[..16]);
        assert_eq!(&s[16..32], &data[64..80]);
        assert_eq!(unswizzle(&s, 64, 16), data);
    }

    #[test]
    fn two_colour_image_becomes_exact_clut8_and_pads_to_pow2() {
        let t = encode(&checker(20, 10), false, Wrap::Clamp, None);
        assert_eq!(t.format, TexFormat::Clut8);
        assert_eq!((t.width, t.height, t.used_w, t.used_h), (32, 16, 20, 10));
        assert_eq!(t.clut.len(), 16 * 4);
        assert!(t.swizzled);
        let lin = unswizzle(&t.data, 32, 16);
        let red = t
            .clut
            .chunks(4)
            .position(|c| c == [255, 0, 0, 255])
            .unwrap() as u8;
        assert_eq!(lin[0], red);
    }

    #[test]
    fn noisy_opaque_image_falls_back_to_565() {
        let mut px = Vec::new();
        let mut s = 12345u32;
        for _ in 0..64 * 64 {
            s = s.wrapping_mul(1103515245).wrapping_add(12345);
            px.push([(s >> 8) as u8, (s >> 16) as u8, (s >> 24) as u8, 255]);
        }
        let t = encode(&Image::from_pixels(64, 64, &px), false, Wrap::Clamp, None);
        assert_eq!(t.format, TexFormat::Rgb565);
        assert_eq!(t.data.len(), 64 * 64 * 2);
    }

    #[test]
    fn large_bitmap_is_split_into_tiles() {
        let c = convert(&checker(700, 300), 1.0, false);
        assert_eq!(c.tiles.len(), 2);
        assert_eq!((c.tiles[0].w, c.tiles[1].x, c.tiles[1].w), (512, 512, 188));
        let r = convert(&checker(700, 300), 1.0, true);
        assert_eq!(r.tiles.len(), 1);
        assert!(r.tiles[0].texture.width <= 512);
    }

    #[test]
    fn premultiplied_lossless_is_unmultiplied() {
        assert_eq!(unmultiply([64, 0, 0, 128], true), [128, 0, 0, 128]);
        assert_eq!(unmultiply([9, 9, 9, 0], true), [0, 0, 0, 0]);
    }

    #[test]
    fn downscale_averages() {
        let img = Image::from_pixels(2, 1, &[[0, 0, 0, 255], [200, 200, 200, 255]]);
        assert_eq!(img.resize(1, 1).pixel(0, 0), [100, 100, 100, 255]);
    }
}
