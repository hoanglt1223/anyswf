# AnySWF

Ahead-of-time compiler that turns Flash (AS1/AS2) SWF movies into native PSP
homebrew (`EBOOT.PBP`) plus a PC debug build. ActionScript bytecode is compiled
to C and vector art is tessellated at build time, so the PSP only runs native
code and draws triangles.

Status: early. The platform layer, the asset pipeline (`swf2c assets`) and the
PC renderer with display list and timeline playback work; ActionScript
compilation and the PSP renderer are in progress. No game content ships with
this project: bring your own files. The only SWFs in the repository are Ruffle's
MIT/Apache test movies in `tests/ruffle/`.

## Layout

| Path | Contents |
|---|---|
| `swf2c/` | Rust workspace for the SWF-to-C compiler and asset converter |
| `runtime/` | C11 runtime (libswfrt): assets.bin loader, display list, timeline, renderer |
| `apps/player/` | `anyswf-player`: plays an assets.bin; headless PNG screenshots for tests |
| `docs/dev/assets-bin-format.md` | The assets.bin file format |
| `platform/platform.h` | Small platform API: frame pacing, input, audio, files, clock |
| `platform/psp/`, `platform/sdl/` | PSP (pspsdk/GU) and PC (SDL3/OpenGL) backends |
| `apps/hello/` | Hello triangle + tone, the smoke test for both backends |
| `tests/platform/` | Platform unit tests (PC) |
| `tests/swf-src/` | Self-authored AS2 tests (`*.as`) with expected trace output |
| `tests/runtime/` | Runtime unit tests (loader validation, timeline) |
| `tests/render/` | Render tests: cases, Ruffle reference PNGs, comparison scripts |
| `tests/ruffle/` | Test SWFs copied from Ruffle (MIT/Apache-2.0) |
| `cmake/psp.cmake` | PSP toolchain file (wraps pspdev's) |
| `docker/psp-build.Dockerfile` | Pinned `pspdev/pspdev` build image |

The platform layer is application-neutral and is shared with the other PSP
ports in this workspace; keep Flash-specific code out of `platform/`.

## Build

PC (SDL3 3.4.18 is downloaded and built by CMake, pinned by hash):

```sh
cmake -S . -B out/msvc -G "Visual Studio 17 2022" -A x64      # Windows
cmake --build out/msvc --config Release
ctest --test-dir out/msvc -C Release --output-on-failure
out/msvc/apps/hello/Release/hello.exe --smoke                  # hidden window, silent audio, self-checking

cmake -S . -B out/linux -G Ninja -DCMAKE_BUILD_TYPE=Release     # Linux
cmake --build out/linux && out/linux/apps/hello/hello
```

`hello` flags: `--frames N` exits after N frames, `--headless` hides the window
and uses SDL's dummy audio driver, `--smoke` is headless and checks pixels,
audio progress and the save-file round trip (exit code 1 on failure).

PSP (needs Docker; works from Windows through Git Bash):

```sh
scripts/psp-build.sh                                  # -> out/psp/apps/hello/EBOOT.PBP
PSP_CMAKE_ARGS=-DBUILD_PRX=ON scripts/psp-build.sh    # PRX for psplink
```

Copy `EBOOT.PBP` to `ms0:/PSP/GAME/ANYSWFHELLO/`. Saves go to
`ms0:/PSP/SAVEDATA/<APP_ID>/`.

Compiler and asset converter:

```sh
cd swf2c && cargo build --release && cargo test
swf2c inspect game.swf                      # header and character table
swf2c assets game.swf -o assets.bin         # meshes, textures, sounds, timelines
anyswf-player assets.bin                    # PC playback (windowed)
anyswf-player assets.bin --screenshot f.png # headless, frame 1 as PNG
```

Render tests (generated SWFs plus Ruffle test movies, compared with references
rendered by Ruffle; see `tests/render/make-render-refs.py` for how they are made):

```sh
tests/render/run.sh out/msvc/apps/player/Release/anyswf-player.exe
```

Test corpus (MTASC 1.14, fetched with a pinned checksum into `tools/`):

```sh
tests/fetch-mtasc.sh
tests/build-corpus.sh            # -> out/corpus/*.swf + *.expected
```

## Controls on PC

Arrows = D-pad, Z/X/A/S = Cross/Circle/Square/Triangle, Q/W = L/R,
Enter = Start, Right Shift = Select, IJKL = analog nub, Esc = quit.
Gamepads map by position (south button = Cross).

## License

MIT, see [LICENSE](LICENSE). Third-party code, data and test files: [THIRD_PARTY.md](THIRD_PARTY.md).
