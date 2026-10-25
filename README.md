# AnySWF

Ahead-of-time compiler that turns Flash (AS1/AS2) SWF movies into native PSP
homebrew (`EBOOT.PBP`), and into native Windows, Linux, macOS and Android games.
ActionScript bytecode is compiled to C and vector art is tessellated at build time,
so the PSP only runs native code and draws triangles.

```sh
anyswf build game.swf --target psp|linux|windows|macos|android -o out/game
```

See [docs/user/EXPORT.md](docs/user/EXPORT.md) for building and
[docs/user/USAGE.md](docs/user/USAGE.md) for controls, per-game settings, saves and the
supported ActionScript. Compatibility: `compat/*.toml`, rendered
by `tools/compat-site/generate.py`; how to test a game and report it:
[CONTRIBUTING.md](CONTRIBUTING.md).

Status: early. The platform layer, the asset pipeline (`swf2c assets`), the renderer
(display list, timeline, colour transforms, masks, add/multiply/screen blend modes),
the AVM1 compiler (`swf2c compile`) and the runtime built-ins games use (MovieClip
methods and events, the drawing API, buttons, Key, Mouse, Sound, SharedObject,
TextField, Date, timers) work on PC and under PPSSPP; nothing has been checked on PSP
hardware yet. No game content ships with this project: bring your own files. The only
SWFs in the repository are Ruffle's MIT/Apache test movies in `tests/ruffle/`,
`tests/ruffle-avm1/` and `tests/ruffle-avm1-builtins/`.

## Layout

| Path | Contents |
|---|---|
| `swf2c/` | Rust workspace for the SWF-to-C compiler and asset converter (`swf2c`) and the `anyswf build` front end (`src/anyswf/`) |
| `runtime/` | C11 runtime (libswfrt): assets.bin loader, display list, timeline, renderer, AVM1 VM, built-ins, sound mixer, profiler |
| `apps/player/` | `anyswf-player`: plays an assets.bin; headless PNG screenshots for tests |
| `docs/dev/assets-bin-format.md` | The assets.bin file format |
| `docs/user/USAGE.md` | Controls, `anyswf.toml`, saves, sound, supported ActionScript, profiler |
| `platform/platform.h` | Small platform API: frame pacing, input, audio, files, clock |
| `platform/psp/`, `platform/sdl/` | PSP (pspsdk/GU) and PC (SDL3/OpenGL 2.1, OpenGL ES 1.1 on Android) backends |
| `platform/android/` | APK template (manifest, `GameActivity`, resources) for `anyswf build --target android` |
| `compat/`, `tools/compat-site/` | Compatibility records and the page generator |
| `apps/hello/` | Hello triangle + tone, the smoke test for both backends |
| `tests/platform/` | Platform unit tests (PC) |
| `tests/swf-src/` | Self-authored AS2 tests (`*.as`) with expected trace output and optional `*.toml` (frames, scripted input, runs); `make-base-swfs.py` writes the self-made movies some compile into |
| `tests/runtime/` | Runtime unit tests (loader validation, timeline, config, mixer, drawing tessellation, texture cache) |
| `tests/render/` | Render tests: cases, Ruffle reference PNGs, comparison scripts |
| `tests/ruffle/` | Render test SWFs copied from Ruffle (MIT/Apache-2.0) |
| `tests/ruffle-avm1/` | AVM1 test SWFs and Flash Player traces copied from Ruffle (MIT/Apache-2.0) |
| `tests/ruffle-avm1-builtins/` | Ruffle AVM1 tests of the built-ins, run as a separate suite with its own floor |
| `tests/avm1/` | AVM1 conformance runner (compile, build, run headless, diff traces) |
| `tests/psp/` | The AS2 corpus as EBOOTs under PPSSPPHeadless (trace diff) |
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
scripts/psp-build.sh                                  # -> out/psp/apps/{hello,player}/EBOOT.PBP
PSP_CMAKE_ARGS=-DBUILD_PRX=ON scripts/psp-build.sh    # PRX for psplink
```

Copy `EBOOT.PBP` to `ms0:/PSP/GAME/ANYSWFHELLO/`. Saves go to
`ms0:/PSP/SAVEDATA/<APP_ID>/`.

Compiler and asset converter:

```sh
cd swf2c && cargo build --release && cargo test
swf2c inspect game.swf                      # header and character table
swf2c assets game.swf -o assets.bin         # meshes, textures, sounds, timelines
             [--target psp|pc|android]      # screen and detail settings (default psp)
anyswf-player assets.bin                    # PC playback (windowed)
anyswf-player assets.bin --screenshot f.png # headless, frame 1 as PNG
```

Player options for tests (`anyswf-player` and exported games): `--headless` runs
deterministically (one SWF frame per display frame, frame-based clock, sound mixed on
the game thread), `--input F:BUTTONS[@X/Y],...` scripts buttons and the cursor,
`--save-dir DIR` keeps saves out of the user folder, `--wav out.wav` writes the mixed
sound, `--config anyswf.toml` overrides the settings file, `--profile` logs the profiler
once per second.

Render tests (generated SWFs, Ruffle test movies and scripted AS2 movies from
`tests/render/as`, compared with references rendered by Ruffle; see
`tests/render/make-render-refs.py` for how they are made; needs MTASC):

```sh
tests/render/run.sh out/msvc/apps/player/Release/anyswf-player.exe
```

Test corpus (MTASC 1.14, fetched with a pinned checksum into `tools/`):

```sh
tests/fetch-mtasc.sh
tests/build-corpus.sh            # -> out/corpus/*.swf + *.expected
```

AVM1 conformance (self-authored corpus plus `tests/ruffle-avm1`; passes at 100% of
the corpus and at least 80% of the Ruffle tests). The built-in tests copied from Ruffle
run as a second suite whose failures are known gaps, with a floor against regressions:

```sh
python tests/avm1/run-avm1-tests.py --swf2c <swf2c> --out out/avm1 --corpus out/corpus \
    [--generator "Visual Studio 17 2022"] [--jobs N] [--only ruffle_<name> ...]
python tests/avm1/run-avm1-tests.py --swf2c <swf2c> --out out/avm1-builtins \
    --ruffle tests/ruffle-avm1-builtins --min-ruffle 50
```

AS2 corpus on the PSP target (PPSSPPHeadless Docker image `ppsspp-headless:local`):

```sh
python tests/psp/run-psp-corpus.py --anyswf <anyswf>   # skips scripted-input and UTC-only tests
```

Releases: pushing a `v*` tag runs `.github/workflows/release.yml`, which publishes
swf2c, anyswf (with the source tree it builds games from) and anyswf-player for
Windows, Linux and macOS plus the PSP EBOOTs. `.github/workflows/pages.yml` checks
`compat/` and publishes the compatibility page once Pages is enabled and the
repository variable `PAGES_ENABLED` is `true`.

## Controls on PC

Arrows = D-pad, Z/X/A/S = Cross/Circle/Square/Triangle, Q/W = L/R,
Enter = Start, Right Shift = Select, IJKL = analog nub, Esc = quit.
Gamepads map by position (south button = Cross). The platform also reports the
mouse (or touch) and every key as Flash key codes; the player gives those to Flash's
`Key` and `Mouse` (see [docs/user/USAGE.md](docs/user/USAGE.md)).

## License

MIT, see [LICENSE](LICENSE). Third-party code, data and test files: [THIRD_PARTY.md](THIRD_PARTY.md).
