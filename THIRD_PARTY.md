# Third-party code and data

- **Ruffle** (MIT OR Apache-2.0, Copyright (c) 2018-2022 Ruffle LLC and contributors):
  swf2c adapts algorithms from Ruffle: shape edge linking (`swf2c/src/tessellate.rs`),
  gradient ramp and focal formula (`swf2c/src/gradients.rs`), bitmap decoding
  (`swf2c/src/bitmaps.rs`), SWF ADPCM decoding (`swf2c/src/sounds.rs`), morph
  interpolation. Crate dependency: `swf` 0.3.0. The runtime's Flash case-mapping
  tables (`runtime/src/avm1/casemap.c`) come from Ruffle's `wstr/src/tables.rs`.
- **TIC-80** (MIT, Copyright (c) 2017 Vadim Grigoruk): the device-text bitmap font
  in `swf2c/src/device_font.rs` is TIC-80's system font (`src/core/font.inl`).
- **minimp3** (CC0-1.0, lieff/minimp3 commit `ea99364f61c14656440e8d77e9c233ccf3124633`):
  `runtime/third_party/minimp3/minimp3.h`, unmodified, decodes MP3 stream sounds in the
  runtime mixer.
- The runtime's Date, drawing-API path rules, timers, AsBroadcaster and clip-event order
  follow Ruffle's `core/src` (MIT OR Apache-2.0); see the comments in `runtime/src/builtins/`.
- **symphonia** 0.5.5 (MPL-2.0): used unmodified as a dependency for MP3 decoding.
- Other crate dependencies (`lyon`, `jpeg-decoder`, `png`, `gif`, `flate2`, `toml`) are
  MIT OR Apache-2.0.
- **SDL 3** (zlib license): downloaded by CMake (`cmake/sdl3.cmake`) and linked
  statically into PC and Android builds. Android APKs from `anyswf build` also contain
  SDL's Java classes (`org.libsdl.app`, from the same release).
- `tools/compat-site/generate.py` is adapted from AnyGBA's generator (MIT, same author).
- **Ruffle test movies** in `tests/ruffle/` (MIT OR Apache-2.0) and the reference
  renders in `tests/render/ref/`, which are produced by Ruffle's exporter; see
  `tests/ruffle/README.md`.
- **Ruffle AVM1 tests** in `tests/ruffle-avm1/` (MIT OR Apache-2.0): test movies,
  Flash Player trace output and `.fla` sources; see `tests/ruffle-avm1/README.md`.
