# Third-party code and data

- **Ruffle** (MIT OR Apache-2.0, Copyright (c) 2018-2022 Ruffle LLC and contributors):
  swf2c adapts algorithms from Ruffle: shape edge linking (`swf2c/src/tessellate.rs`),
  gradient ramp and focal formula (`swf2c/src/gradients.rs`), bitmap decoding
  (`swf2c/src/bitmaps.rs`), SWF ADPCM decoding (`swf2c/src/sounds.rs`), morph
  interpolation. Crate dependency: `swf` 0.3.0.
- **TIC-80** (MIT, Copyright (c) 2017 Vadim Grigoruk): the device-text bitmap font
  in `swf2c/src/device_font.rs` is TIC-80's system font (`src/core/font.inl`).
- **symphonia** 0.5.5 (MPL-2.0): used unmodified as a dependency for MP3 decoding.
- Other crate dependencies (`lyon`, `jpeg-decoder`, `png`, `gif`, `flate2`) are
  MIT OR Apache-2.0.
- **Ruffle test movies** in `tests/ruffle/` (MIT OR Apache-2.0) and the reference
  renders in `tests/render/ref/`, which are produced by Ruffle's exporter; see
  `tests/ruffle/README.md`.
