//! Sound conversion. Event sounds are decoded to PCM and re-encoded as 4-bit IMA ADPCM
//! (cheap to decode on the Allegrex); streamed MP3 stays MP3 for the Media Engine.
//! The SWF ADPCM decoder follows Ruffle's core/src/backend/audio/decoders/adpcm.rs
//! (MIT/Apache-2.0).

use crate::{Error, Result};
use swf::{AudioCompression, SoundFormat};

const STEP_TABLE: [i32; 89] = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
    2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
];

const SWF_INDEX_TABLE: [&[i32]; 4] = [
    &[-1, 2],
    &[-1, -1, 2, 4],
    &[-1, -1, -1, -1, 2, 4, 6, 8],
    &[-1, -1, -1, -1, -1, -1, -1, -1, 1, 2, 4, 6, 8, 10, 13, 16],
];

const IMA_INDEX_TABLE: [i32; 8] = [-1, -1, -1, -1, 2, 4, 6, 8];

/// Samples per channel in one IMA ADPCM block of the output format.
pub const IMA_BLOCK: usize = 1024;

/// Decoded PCM, interleaved when stereo.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Pcm {
    pub rate: u32,
    pub channels: u8,
    pub samples: Vec<i16>,
}

impl Pcm {
    pub fn frames(&self) -> usize {
        self.samples.len() / self.channels.max(1) as usize
    }
}

struct Bits<'a> {
    data: &'a [u8],
    pos: usize,
}

impl Bits<'_> {
    fn read(&mut self, n: u32) -> Option<u32> {
        let mut v = 0u32;
        for _ in 0..n {
            let byte = *self.data.get(self.pos / 8)?;
            v = (v << 1) | ((byte >> (7 - (self.pos % 8))) & 1) as u32;
            self.pos += 1;
        }
        Some(v)
    }
}

/// Decodes SWF ADPCM. Stops at `max_frames` or when the data runs out.
pub fn decode_swf_adpcm(data: &[u8], stereo: bool, max_frames: usize) -> Vec<i16> {
    let mut b = Bits { data, pos: 0 };
    let Some(code) = b.read(2) else {
        return Vec::new();
    };
    let bits = code + 2;
    let ch = if stereo { 2 } else { 1 };
    let mut sample = [0i32; 2];
    let mut index = [0i32; 2];
    let mut out = Vec::new();
    let mut n = 0u32;
    let sign = 1u32 << (bits - 1);
    'outer: while out.len() / ch < max_frames {
        if n == 0 {
            for c in 0..ch {
                let (Some(s), Some(i)) = (b.read(16), b.read(6)) else {
                    break 'outer;
                };
                sample[c] = s as u16 as i16 as i32;
                index[c] = i as i32;
            }
        }
        n = (n + 1) % 4095;
        for c in 0..ch {
            let Some(d) = b.read(bits) else {
                break 'outer;
            };
            let step = STEP_TABLE[index[c].clamp(0, 88) as usize];
            let mag = d & !sign;
            let mut delta = step >> (bits - 1);
            for k in 0..bits - 1 {
                if mag & (1 << k) != 0 {
                    delta += step >> (bits - 2 - k);
                }
            }
            sample[c] = if d & sign != 0 {
                (sample[c] - delta).max(i16::MIN as i32)
            } else {
                (sample[c] + delta).min(i16::MAX as i32)
            };
            index[c] = (index[c] + SWF_INDEX_TABLE[bits as usize - 2][mag as usize]).clamp(0, 88);
        }
        for s in sample.iter().take(ch) {
            out.push(*s as i16);
        }
    }
    out
}

fn decode_mp3(data: &[u8]) -> Result<Pcm> {
    use symphonia::core::{
        audio::SampleBuffer, codecs::DecoderOptions, errors, formats::FormatReader, io,
    };
    let src = io::MediaSourceStream::new(
        Box::new(std::io::Cursor::new(data.to_vec())),
        Default::default(),
    );
    let mut reader = symphonia::default::formats::MpaReader::try_new(src, &Default::default())
        .map_err(|e| Error::new(format!("cannot read MP3: {e}")))?;
    let params = reader
        .default_track()
        .ok_or_else(|| Error::new("MP3 has no track"))?
        .codec_params
        .clone();
    let mut dec = symphonia::default::get_codecs()
        .make(&params, &DecoderOptions::default())
        .map_err(|e| Error::new(format!("cannot decode MP3: {e}")))?;
    let mut pcm = Pcm {
        rate: params.sample_rate.unwrap_or(44100),
        channels: params.channels.map(|c| c.count() as u8).unwrap_or(1),
        samples: Vec::new(),
    };
    while let Ok(packet) = reader.next_packet() {
        match dec.decode(&packet) {
            Ok(buf) => {
                let mut sb = SampleBuffer::<i16>::new(buf.capacity() as u64, *buf.spec());
                sb.copy_interleaved_ref(buf);
                pcm.samples.extend_from_slice(sb.samples());
            }
            Err(errors::Error::DecodeError(_)) => {}
            Err(_) => break,
        }
    }
    Ok(pcm)
}

/// Decodes the audio payload of a DefineSound or a run of stream blocks.
pub fn decode(format: &SoundFormat, data: &[u8], frames: usize) -> Result<Pcm> {
    let channels = if format.is_stereo { 2 } else { 1 };
    let rate = format.sample_rate as u32;
    let samples = match format.compression {
        AudioCompression::Uncompressed | AudioCompression::UncompressedUnknownEndian => {
            if format.is_16_bit {
                data.chunks_exact(2)
                    .map(|b| i16::from_le_bytes([b[0], b[1]]))
                    .collect()
            } else {
                data.iter().map(|&b| ((b as i16) - 128) << 8).collect()
            }
        }
        AudioCompression::Adpcm => decode_swf_adpcm(data, format.is_stereo, frames),
        AudioCompression::Mp3 => {
            // DefineSound MP3 data starts with a 16-bit SeekSamples field.
            let body = data.get(2..).unwrap_or_default();
            let mut pcm = decode_mp3(body)?;
            let keep = frames.min(pcm.frames()) * pcm.channels as usize;
            if frames > 0 {
                pcm.samples.truncate(keep);
            }
            return Ok(pcm);
        }
        other => {
            return Err(Error::new(format!("unsupported sound codec {other:?}")));
        }
    };
    let mut pcm = Pcm {
        rate,
        channels,
        samples,
    };
    if frames > 0 && pcm.frames() > frames {
        pcm.samples.truncate(frames * channels as usize);
    }
    Ok(pcm)
}

/// Encodes PCM as blocks of `IMA_BLOCK` frames. Each block holds, per channel, a 4-byte
/// header (i16 predictor, u8 step index, u8 0) followed by IMA_BLOCK/2 bytes of nibbles
/// (low nibble first); channels are stored one after another inside the block.
pub fn encode_ima(pcm: &Pcm) -> Vec<u8> {
    let ch = pcm.channels.max(1) as usize;
    let frames = pcm.frames();
    let mut out = Vec::new();
    let mut state = vec![(0i32, 0i32); ch];
    for start in (0..frames).step_by(IMA_BLOCK) {
        for (c, st) in state.iter_mut().enumerate() {
            let first = pcm.samples[start * ch + c] as i32;
            st.0 = first;
            out.extend_from_slice(&(first as i16).to_le_bytes());
            out.push(st.1 as u8);
            out.push(0);
            let mut byte = 0u8;
            for k in 0..IMA_BLOCK {
                let f = start + k;
                let s = if f < frames {
                    pcm.samples[f * ch + c] as i32
                } else {
                    st.0
                };
                let nib = ima_encode_one(st, s);
                if k % 2 == 0 {
                    byte = nib;
                } else {
                    out.push(byte | (nib << 4));
                }
            }
        }
    }
    out
}

fn ima_step(st: &mut (i32, i32), nib: u8) -> i32 {
    let step = STEP_TABLE[st.1 as usize];
    let mut diff = step >> 3;
    if nib & 1 != 0 {
        diff += step >> 2;
    }
    if nib & 2 != 0 {
        diff += step >> 1;
    }
    if nib & 4 != 0 {
        diff += step;
    }
    st.0 = if nib & 8 != 0 {
        st.0 - diff
    } else {
        st.0 + diff
    }
    .clamp(-32768, 32767);
    st.1 = (st.1 + IMA_INDEX_TABLE[(nib & 7) as usize]).clamp(0, 88);
    st.0
}

fn ima_encode_one(st: &mut (i32, i32), sample: i32) -> u8 {
    let step = STEP_TABLE[st.1 as usize];
    let mut diff = sample - st.0;
    let mut nib = 0u8;
    if diff < 0 {
        nib = 8;
        diff = -diff;
    }
    let mut s = step;
    for bit in [4u8, 2, 1] {
        if diff >= s {
            nib |= bit;
            diff -= s;
        }
        s >>= 1;
    }
    ima_step(st, nib);
    nib
}

/// Reference decoder for `encode_ima`; the C runtime implements the same algorithm.
pub fn decode_ima(data: &[u8], channels: u8, frames: usize) -> Vec<i16> {
    let ch = channels.max(1) as usize;
    let block_bytes = 4 + IMA_BLOCK / 2;
    let mut out = vec![0i16; frames * ch];
    for (bi, block) in data.chunks(block_bytes * ch).enumerate() {
        for c in 0..ch {
            let Some(b) = block.get(c * block_bytes..(c + 1) * block_bytes) else {
                return out;
            };
            let mut st = (
                i16::from_le_bytes([b[0], b[1]]) as i32,
                (b[2] as i32).min(88),
            );
            for k in 0..IMA_BLOCK {
                let f = bi * IMA_BLOCK + k;
                let byte = b[4 + k / 2];
                let nib = if k % 2 == 0 { byte & 15 } else { byte >> 4 };
                let s = ima_step(&mut st, nib);
                if f < frames {
                    out[f * ch + c] = s as i16;
                }
            }
        }
    }
    out
}

/// MP3 stream blocks start with u16 SampleCount and i16 SeekSamples.
pub fn mp3_block_payload(block: &[u8]) -> (u16, &[u8]) {
    if block.len() < 4 {
        return (0, &[]);
    }
    (u16::from_le_bytes([block[0], block[1]]), &block[4..])
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sine(frames: usize, ch: u8) -> Pcm {
        let mut samples = Vec::new();
        for i in 0..frames {
            let v = ((i as f32 * 0.05).sin() * 12000.0) as i16;
            for c in 0..ch {
                samples.push(if c == 0 { v } else { v / 2 });
            }
        }
        Pcm {
            rate: 22050,
            channels: ch,
            samples,
        }
    }

    fn snr(a: &[i16], b: &[i16]) -> f64 {
        let sig: f64 = a.iter().map(|&x| (x as f64).powi(2)).sum();
        let noise: f64 = a
            .iter()
            .zip(b)
            .map(|(&x, &y)| (x as f64 - y as f64).powi(2))
            .sum();
        10.0 * (sig / noise.max(1.0)).log10()
    }

    #[test]
    fn ima_round_trip_is_close() {
        for ch in [1u8, 2] {
            let pcm = sine(3000, ch);
            let enc = encode_ima(&pcm);
            assert_eq!(enc.len(), 3 * ch as usize * (4 + IMA_BLOCK / 2));
            let dec = decode_ima(&enc, ch, 3000);
            let s = snr(&pcm.samples, &dec);
            assert!(s > 25.0, "SNR {s} dB for {ch} channel(s)");
        }
    }

    #[test]
    fn uncompressed_8bit_and_16bit() {
        let f8 = SoundFormat {
            compression: AudioCompression::Uncompressed,
            sample_rate: 11025,
            is_stereo: false,
            is_16_bit: false,
        };
        assert_eq!(
            decode(&f8, &[128, 255, 0], 0).unwrap().samples,
            vec![0, 127 << 8, -128 << 8]
        );
        let f16 = SoundFormat {
            is_16_bit: true,
            ..f8
        };
        assert_eq!(
            decode(&f16, &[0x34, 0x12], 0).unwrap().samples,
            vec![0x1234]
        );
    }

    #[test]
    fn swf_adpcm_header_and_silence() {
        // 4-bit codes, initial sample 1000, index 0, then a zero delta code.
        // bits: 10 | 0000001111101000 | 000000 | 0000 ...
        let mut bits = String::from("10");
        bits += &format!("{:016b}", 1000u16);
        bits += "000000";
        bits += "0000";
        bits += "0000";
        while bits.len() % 8 != 0 {
            bits.push('0');
        }
        let bytes: Vec<u8> = bits
            .as_bytes()
            .chunks(8)
            .map(|c| u8::from_str_radix(std::str::from_utf8(c).unwrap(), 2).unwrap())
            .collect();
        let out = decode_swf_adpcm(&bytes, false, 2);
        // A zero code still adds step>>3 (7>>3 = 0), so the value holds at 1000.
        assert_eq!(out, vec![1000, 1000]);
    }
}
