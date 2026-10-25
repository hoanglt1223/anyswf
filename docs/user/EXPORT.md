# Building games with `anyswf build`

`anyswf build` turns a SWF you have the right to use into a native game:

```sh
anyswf build game.swf --target psp     -o out/game-psp      # EBOOT.PBP + assets.bin
anyswf build game.swf --target linux   -o out/game-linux    # <Name> + assets.bin
anyswf build game.swf --target windows -o out/game-win      # <Name>.exe + assets.bin
anyswf build game.swf --target macos   -o out/game-mac      # <Name>.app (ad-hoc signed)
anyswf build game.swf --target android -o out/game-android  # <Name>.apk
```

It compiles the movie with `swf2c` (ActionScript to C, shapes to meshes, bitmaps to
textures) using the target's settings, then builds the AnySWF runtime and the game
with CMake. `anyswf help` lists every option. Build files go to `<dir>.work` (or
`--work`); keep it to make the next build of the same target fast.

`anyswf` needs the AnySWF source tree. It finds it next to the program (a release
package carries it in `share/anyswf/source`; a program built in a checkout uses the
checkout), or takes `--source <dir>` / `$ANYSWF_SOURCE`.

## Targets

| Target | Needs | Screen | Output |
|---|---|---|---|
| `psp` | `$PSPDEV` (pspdev toolchain) or Docker; the image is built from `docker/psp-build.Dockerfile` on first use | the stage fitted into 480x272 | `EBOOT.PBP` and `assets.bin`: copy the folder to `ms0:/PSP/GAME/<Folder>/` |
| `linux` | CMake 3.24+, a C compiler, SDL3's build dependencies (see `.github/workflows/ci.yml`), Ninja optional | the original stage size | executable and `assets.bin`, kept side by side |
| `windows` | Visual Studio 2022 (MSVC) and CMake; run from any prompt | as Linux | `<Name>.exe` and `assets.bin` (no console window) |
| `macos` | Xcode command-line tools and CMake | as Linux | `<Name>.app` with `assets.bin` in `Contents/Resources`, ad-hoc signed |
| `android` | Android SDK (`platforms;android-35`, `build-tools`), NDK, JDK 17+, CMake | as Linux, full screen | `<Name>.apk` |

Desktop targets build only on their own OS (the host compiler is used). The PSP and
Android targets build on any host.

PC builds draw at the window's resolution with 4x multisampling (`--msaa N` on the
game's command line changes it), keep the stage's aspect ratio with bars in a resized
window, and tessellate curves for up to twice the stage size. The platform layer reports
the real mouse (or a touch) in stage pixels and the keyboard as Flash key codes
(`Key.isDown` values) for the runtime's Mouse and Key objects. Esc quits. Saves (SharedObject) go to
the platform's user data folder: `%APPDATA%\anyport\<id>\` on Windows,
`~/.local/share/anyport/<id>/` on Linux, `~/Library/Application Support/anyport/<id>/`
on macOS, the app's internal storage on Android, `ms0:/PSP/SAVEDATA/<id>/` on PSP.

Game command line (PC): `<Name> [--headless] [--frames N] [--screenshot f.png]
[--msaa N] [--touch-keys k1,k2,...]`. `--screenshot` renders frame N (default 1) at the
stage size without a window, which is how CI checks an export.

## Per-game settings

`--config game.toml` reads these keys; other keys and tables are ignored, so the file can
also hold the runtime's per-game settings. Command-line options win.

```toml
title = "Sky Hop"           # window, EBOOT and app name (default: the SWF file name)
id = "sky-hop"              # save folder; A-Z a-z 0-9 _ - (default: from the title)
version = "1.2"             # app version (Android, macOS)
tolerance = 0.25            # curve flattening error in screen pixels (default 0.35)

[android]
package = "com.example.skyhop"                         # default io.anyport.anyswf.<id>
version_code = 12                                      # default: minutes since 2024
touch_keys = ["left", "right", "up", "down", "space"]  # on-screen buttons
```

## Android

```sh
anyswf build game.swf --target android -o out/android --touch-keys left,right,space \
    [--abi arm64-v8a,x86_64] [--android-sdk DIR] [--android-ndk DIR] [--icon icon.png]
adb install -r out/android/<Name>.apk
```

The SDK is found through `--android-sdk`, `ANDROID_HOME`, `ANDROID_SDK_ROOT`, then the
default install folder; the NDK through `--android-ndk`, `ANDROID_NDK_HOME`,
`ANDROID_NDK_ROOT`, then the newest `<sdk>/ndk/*`. Command-line tools only:

```sh
sdkmanager "platforms;android-35" "build-tools;35.0.0" "ndk;27.2.12479018"
```

Touches act as the mouse; arrow keys listed in `--touch-keys` form a d-pad at the
bottom left, other keys a grid at the bottom right (names: a letter or digit, `left`,
`right`, `up`, `down`, `space`, `enter`, `shift`, `control`, `alt`, `escape`,
`backspace`, `tab`, `pageup`, `pagedown`, `home`, `end`, `insert`, `delete`, `f1`-`f12`).
Landscape stages lock the screen to landscape, taller stages to portrait.

APKs are signed with the debug keystore (`~/.android/debug.keystore`, created if
missing). For a release key: `--keystore my.jks --keystore-pass-env KS_PASS
--key-alias mykey`. Extra player arguments for testing:
`adb shell am start -n <package>/io.anyport.anyswf.GameActivity --esa args --frames,600`.

How it is built: the game and runtime link into `libmain.so` per ABI with the NDK
(OpenGL ES 1.1, SDL linked statically), then `aapt2`, `javac`, `d8`, the JDK's `jar`,
`zipalign` and `apksigner` assemble the APK. No Gradle, so it works offline and takes
seconds after the first native build. The template is `platform/android/`.

## Windows

From a checkout or a release package on Windows (Visual Studio 2022 with the C++
workload, CMake 3.24+, Rust for building `anyswf` itself):

```bat
cargo build --release --manifest-path swf2c\Cargo.toml
swf2c\target\release\anyswf.exe build game.swf --target windows -o out\game-win
```

CMake uses the Visual Studio generator unless `CMAKE_GENERATOR` is set.
