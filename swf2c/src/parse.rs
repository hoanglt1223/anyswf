//! Loading and decompressing SWF files through Ruffle's `swf` crate.

use crate::{Error, Result};

/// Reads a SWF from bytes. `FWS` and `CWS` are accepted; `ZWS` (LZMA) is rejected
/// explicitly so the user gets a clear message instead of a generic parse error.
pub fn decompress(bytes: &[u8]) -> Result<swf::SwfBuf> {
    match bytes.get(..3) {
        Some(b"FWS") | Some(b"CWS") => {}
        Some(b"ZWS") => {
            return Err(Error::new(
                "LZMA-compressed SWF (ZWS) is not supported yet; re-save it uncompressed or zlib-compressed",
            ));
        }
        _ => {
            return Err(Error::new(
                "not a SWF file (expected an FWS or CWS signature)",
            ));
        }
    }
    swf::decompress_swf(bytes).map_err(|e| Error::new(format!("cannot read SWF header: {e}")))
}

/// Tag header: (code, header length, body length), or None when truncated.
fn tag_header(data: &[u8]) -> Option<(u16, usize, usize)> {
    let v = u16::from_le_bytes([*data.first()?, *data.get(1)?]);
    let code = v >> 6;
    let short = (v & 0x3F) as usize;
    if short == 0x3F {
        let l = u32::from_le_bytes(data.get(2..6)?.try_into().ok()?) as usize;
        Some((code, 6, l))
    } else {
        Some((code, 2, short))
    }
}

const DEFINE_SPRITE: u16 = 39;

/// Reads tags the way Flash Player tolerates them: a malformed tag is skipped (with a
/// warning) instead of failing the movie, a sprite with a damaged tag list keeps the
/// tags before the damage, and data running past the end stops the list.
fn read_tags<'a>(data: &'a [u8], version: u8, warnings: &mut Vec<String>) -> Vec<swf::Tag<'a>> {
    let mut tags = Vec::new();
    let mut pos = 0usize;
    while pos < data.len() {
        let Some((code, hlen, blen)) = tag_header(&data[pos..]) else {
            break;
        };
        let end = (pos + hlen).saturating_add(blen);
        if end > data.len() {
            warnings.push(format!(
                "tag {code} at offset {pos} runs past the end of its list; ignored"
            ));
            break;
        }
        let bytes = &data[pos..end];
        match swf::read::Reader::new(bytes, version).read_tag() {
            Ok(swf::Tag::End) => break,
            Ok(tag) => tags.push(tag),
            Err(e) if code == DEFINE_SPRITE && blen >= 4 => {
                let body = &bytes[hlen..];
                let id = u16::from_le_bytes([body[0], body[1]]);
                let num_frames = u16::from_le_bytes([body[2], body[3]]);
                warnings.push(format!("sprite {id}: {e}; keeping the readable tags"));
                let inner = read_tags(&body[4..], version, warnings);
                tags.push(swf::Tag::DefineSprite(swf::Sprite {
                    id,
                    num_frames,
                    tags: inner,
                }));
            }
            Err(e) => warnings.push(format!("skipped malformed tag {code} at offset {pos}: {e}")),
        }
        pos = end;
    }
    tags
}

/// Parses all tags; warnings about skipped damage are returned alongside.
pub fn parse_with_warnings(buf: &swf::SwfBuf) -> Result<(swf::Swf<'_>, Vec<String>)> {
    let mut warnings = Vec::new();
    let tags = read_tags(&buf.data, buf.header.version(), &mut warnings);
    let parsed = swf::Swf {
        header: buf.header.clone(),
        tags,
    };
    if parsed.header.is_action_script_3() {
        return Err(Error::new(
            "this SWF uses ActionScript 3 (AVM2), which AnySWF does not support",
        ));
    }
    Ok((parsed, warnings))
}

pub fn parse(buf: &swf::SwfBuf) -> Result<swf::Swf<'_>> {
    let (parsed, warnings) = parse_with_warnings(buf)?;
    for w in warnings {
        eprintln!("swf2c: warning: {w}");
    }
    Ok(parsed)
}

pub fn read_file(path: &std::path::Path) -> Result<swf::SwfBuf> {
    let bytes = std::fs::read(path)
        .map_err(|e| Error::new(format!("cannot read {}: {e}", path.display())))?;
    decompress(&bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_lzma_with_clear_message() {
        let err = decompress(b"ZWS\x0d\0\0\0\0")
            .err()
            .expect("ZWS must be rejected");
        assert!(err.to_string().contains("LZMA"));
    }

    #[test]
    fn rejects_non_swf() {
        assert!(decompress(b"PK\x03\x04").is_err());
        assert!(decompress(b"").is_err());
    }

    #[test]
    fn damaged_sprite_keeps_its_readable_tags() {
        // DefineSprite id 5, 1 frame, containing ShowFrame then a tag whose length runs
        // past the sprite's end.
        let mut sprite_body = vec![5, 0, 1, 0];
        sprite_body.extend_from_slice(&[0x40, 0x00]); // ShowFrame
        sprite_body.extend_from_slice(&[0x0F, 0x03]); // tag 12 (DoAction), length 15: truncated
        let mut data = Vec::new();
        let hdr = (39u16 << 6) | sprite_body.len() as u16;
        data.extend_from_slice(&hdr.to_le_bytes());
        data.extend_from_slice(&sprite_body);
        data.extend_from_slice(&[0x40, 0x00, 0x00, 0x00]); // ShowFrame, End
        let mut warnings = Vec::new();
        let tags = read_tags(&data, 8, &mut warnings);
        assert_eq!(tags.len(), 2);
        match &tags[0] {
            swf::Tag::DefineSprite(s) => {
                assert_eq!((s.id, s.num_frames), (5, 1));
                assert_eq!(s.tags, vec![swf::Tag::ShowFrame]);
            }
            t => panic!("expected a sprite, got {t:?}"),
        }
        assert!(!warnings.is_empty());
    }

    #[test]
    fn round_trips_a_written_swf() {
        let header = swf::Header {
            compression: swf::Compression::Zlib,
            version: 8,
            stage_size: swf::Rectangle {
                x_min: swf::Twips::ZERO,
                x_max: swf::Twips::from_pixels(480.0),
                y_min: swf::Twips::ZERO,
                y_max: swf::Twips::from_pixels(272.0),
            },
            frame_rate: swf::Fixed8::from_f32(30.0),
            num_frames: 1,
        };
        let mut out = Vec::new();
        swf::write_swf(&header, &[swf::Tag::ShowFrame], &mut out).unwrap();
        assert_eq!(&out[..3], b"CWS");
        let buf = decompress(&out).unwrap();
        let parsed = parse(&buf).unwrap();
        assert_eq!(parsed.header.version(), 8);
        assert_eq!(parsed.header.num_frames(), 1);
    }
}
