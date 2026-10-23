# runtime (libswfrt)

C11 runtime linked into every compiled game. It sits on top of the platform layer
(`../platform/platform.h`) and never calls pspsdk or SDL directly except inside its
renderer backends.

| Path | Contents |
|---|---|
| `include/swfrt/assets.h`, `src/assets.c` | assets.bin loader and validator ([format](../docs/dev/assets-bin-format.md)) |
| `include/swfrt/movie.h` | Display objects, timeline playback, transforms |
| `src/display/` | Display list, timeline commands and gotos, render walk, text fields |
| `src/render/` | Renderer interface (`render.h`), OpenGL backend (`gl.c`), colour transforms |
