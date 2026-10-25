# runtime (libswfrt)

C11 runtime linked into every compiled game. It sits on top of the platform layer
(`../platform/platform.h`) and never calls pspsdk or SDL directly except inside its
renderer backends.

| Path | Contents |
|---|---|
| `include/swfrt/assets.h`, `src/assets.c` | assets.bin loader and validator ([format](../docs/dev/assets-bin-format.md)) |
| `include/swfrt/movie.h` | Display objects, timeline playback, transforms |
| `src/display/` | Display list, timeline commands and gotos, render walk, text fields |
| `src/render/` | Renderer interface (`render.h`), OpenGL (`gl.c`) and GE (`ge.c`) backends, colour transforms, the PSP VRAM texture cache (`texture_cache.c`) |
| `src/avm1/` | AVM1 value model, objects, calls, scopes, GC and the core classes (Object, Function, Array, String, Number, Math, Error) |
| `src/builtins/` | Flash built-ins: events and the action queue, MovieClip methods, drawing API, buttons, Key, Mouse, timers, TextField, Date, Sound, SharedObject, Stage and Color |
| `src/audio/`, `include/swfrt/mixer.h` | Software mixer (PCM16, IMA ADPCM, MP3 via `third_party/minimp3`) |
| `src/config.c`, `include/swfrt/config.h` | `anyswf.toml` settings |
| `src/debug/`, `include/swfrt/profiler.h` | Profiler overlay and memory accounting |
