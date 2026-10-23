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

pub fn parse(buf: &swf::SwfBuf) -> Result<swf::Swf<'_>> {
    let parsed =
        swf::parse_swf(buf).map_err(|e| Error::new(format!("cannot parse SWF tags: {e}")))?;
    if parsed.header.is_action_script_3() {
        return Err(Error::new(
            "this SWF uses ActionScript 3 (AVM2), which AnySWF does not support",
        ));
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
