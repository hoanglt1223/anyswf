//! `anyswf build --target android`: the game linked into libmain.so per ABI with the NDK,
//! then an APK assembled with the SDK command-line tools (aapt2, javac, d8, zipalign,
//! apksigner) and the JDK's `jar`. No Gradle: the same approach as AnyGBA, offline and fast.

use crate::config;
use crate::export::{Job, Result, cmake_path, copy, mkdir, on_path, run, write};
use std::path::{Path, PathBuf};
use std::process::Command;

pub const MIN_SDK: u32 = 24;
pub const TARGET_SDK: u32 = 35;
pub const DEFAULT_ABIS: [&str; 2] = ["arm64-v8a", "x86_64"];
pub const ABIS: [&str; 4] = ["arm64-v8a", "armeabi-v7a", "x86_64", "x86"];

#[cfg(windows)]
const NDK_HOST: &str = "windows-x86_64";
#[cfg(target_os = "macos")]
const NDK_HOST: &str = "darwin-x86_64";
#[cfg(not(any(windows, target_os = "macos")))]
const NDK_HOST: &str = "linux-x86_64";

struct Tools {
    build_tools: PathBuf,
    android_jar: PathBuf,
    ndk: PathBuf,
}

fn env_path(name: &str) -> Option<PathBuf> {
    std::env::var_os(name)
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
}

fn version_key(name: &str) -> Vec<u32> {
    name.split(|c: char| !c.is_ascii_digit())
        .filter(|s| !s.is_empty())
        .filter_map(|s| s.parse().ok())
        .collect()
}

/// Subdirectories of `dir` accepted by `ok`, newest version first.
fn versioned(dir: &Path, ok: impl Fn(&Path) -> bool) -> Vec<PathBuf> {
    let mut out: Vec<PathBuf> = std::fs::read_dir(dir)
        .into_iter()
        .flatten()
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.is_dir() && ok(p))
        .collect();
    out.sort_by_key(|p| {
        std::cmp::Reverse(version_key(
            &p.file_name().unwrap_or_default().to_string_lossy(),
        ))
    });
    out
}

fn find_sdk(explicit: Option<&Path>) -> Result<PathBuf> {
    if let Some(sdk) = explicit {
        return if sdk.join("platforms").is_dir() {
            Ok(sdk.to_path_buf())
        } else {
            Err(format!(
                "--android-sdk {} is not an Android SDK",
                sdk.display()
            ))
        };
    }
    let mut candidates: Vec<PathBuf> = ["ANDROID_HOME", "ANDROID_SDK_ROOT"]
        .iter()
        .filter_map(|v| env_path(v))
        .collect();
    if cfg!(windows) {
        candidates.extend(env_path("LOCALAPPDATA").map(|p| p.join("Android/Sdk")));
    } else if cfg!(target_os = "macos") {
        candidates.extend(env_path("HOME").map(|p| p.join("Library/Android/sdk")));
    } else {
        candidates.extend(env_path("HOME").map(|p| p.join("Android/Sdk")));
    }
    candidates
        .into_iter()
        .find(|c| c.join("platforms").is_dir())
        .ok_or_else(|| {
            "no Android SDK found; pass --android-sdk or set ANDROID_HOME (docs/user/EXPORT.md)"
                .into()
        })
}

fn find_tools(job: &Job) -> Result<Tools> {
    let a = &job.opts.android;
    let sdk = find_sdk(a.sdk.as_deref())?;
    let exe = std::env::consts::EXE_SUFFIX;
    let build_tools = versioned(&sdk.join("build-tools"), |p| {
        p.join("lib/d8.jar").is_file()
            && p.join("lib/apksigner.jar").is_file()
            && p.join(format!("aapt2{exe}")).is_file()
            && p.join(format!("zipalign{exe}")).is_file()
    })
    .into_iter()
    .next()
    .ok_or_else(|| {
        format!(
            "no build-tools under {}; install them with sdkmanager",
            sdk.display()
        )
    })?;
    let android_jar = versioned(&sdk.join("platforms"), |p| {
        p.join("android.jar").is_file()
            && version_key(&p.file_name().unwrap_or_default().to_string_lossy())
                .first()
                .is_some_and(|v| *v >= TARGET_SDK)
    })
    .into_iter()
    .next()
    .map(|p| p.join("android.jar"))
    .ok_or_else(|| {
        format!(
            "no platforms/android-{TARGET_SDK} (or newer) under {}; install it with sdkmanager",
            sdk.display()
        )
    })?;
    let ndk_ok = |p: &Path| p.join("build/cmake/android.toolchain.cmake").is_file();
    let ndk = if let Some(n) = &a.ndk {
        if !ndk_ok(n) {
            return Err(format!("--android-ndk {} is not an NDK", n.display()));
        }
        n.clone()
    } else if let Some(n) = ["ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"]
        .iter()
        .filter_map(|v| env_path(v))
        .find(|p| ndk_ok(p))
    {
        n
    } else {
        versioned(&sdk.join("ndk"), ndk_ok)
            .into_iter()
            .next()
            .ok_or_else(|| {
                format!(
                    "no Android NDK under {}; install one with sdkmanager or pass --android-ndk",
                    sdk.join("ndk").display()
                )
            })?
    };
    Ok(Tools {
        build_tools,
        android_jar,
        ndk,
    })
}

fn jdk(name: &str) -> PathBuf {
    let file = format!("{name}{}", std::env::consts::EXE_SUFFIX);
    if let Some(home) = env_path("JAVA_HOME") {
        let p = home.join("bin").join(&file);
        if p.is_file() {
            return p;
        }
    }
    PathBuf::from(name)
}

/// Builds libmain.so for each ABI; returns (abi, stripped library) pairs and the SDL source
/// tree the first configure fetched (its Java classes go into the APK).
fn build_native(
    job: &Job,
    tools: &Tools,
    abis: &[String],
) -> Result<(Vec<(String, PathBuf)>, PathBuf)> {
    let toolchain = tools.ndk.join("build/cmake/android.toolchain.cmake");
    let strip = tools
        .ndk
        .join("toolchains/llvm/prebuilt")
        .join(NDK_HOST)
        .join(format!("bin/llvm-strip{}", std::env::consts::EXE_SUFFIX));
    let mut libs = Vec::new();
    let mut sdl_src: Option<PathBuf> = None;
    for abi in abis {
        let bin = job.work.join("android").join(abi);
        let mut configure = Command::new("cmake");
        configure.arg("-S").arg(&job.source).arg("-B").arg(&bin);
        if on_path("ninja") {
            configure.args(["-G", "Ninja"]);
        }
        configure
            .arg(format!("-DCMAKE_TOOLCHAIN_FILE={}", cmake_path(&toolchain)))
            .arg(format!("-DANDROID_ABI={abi}"))
            .arg(format!("-DANDROID_PLATFORM=android-{MIN_SDK}"))
            .args(job.game_defines(&job.gen_dir.join("game.c")));
        // One SDL download serves every ABI.
        if let Some(src) = &sdl_src {
            configure.arg(format!(
                "-DFETCHCONTENT_SOURCE_DIR_SDL3={}",
                cmake_path(src)
            ));
        }
        configure.args(&job.opts.cmake_args);
        run(&mut configure, &format!("CMake configure ({abi})"))?;
        run(
            Command::new("cmake")
                .arg("--build")
                .arg(&bin)
                .args(["--target", &job.name])
                .args(job.parallel_args()),
            &format!("CMake build ({abi})"),
        )?;
        let lib = bin.join("libmain.so");
        if !lib.is_file() {
            return Err(format!("the {abi} build produced no {}", lib.display()));
        }
        let stripped = bin.join("libmain.stripped.so");
        if strip.is_file() {
            run(
                Command::new(&strip)
                    .arg("--strip-unneeded")
                    .arg("-o")
                    .arg(&stripped)
                    .arg(&lib),
                "llvm-strip",
            )?;
        } else {
            copy(&lib, &stripped)?;
        }
        libs.push((abi.clone(), stripped));
        if sdl_src.is_none() {
            let src = bin.join("_deps/sdl3-src");
            sdl_src = Some(if src.is_dir() {
                src
            } else {
                // The caller passed FETCHCONTENT_SOURCE_DIR_SDL3 itself.
                job.opts
                    .cmake_args
                    .iter()
                    .find_map(|a| a.strip_prefix("-DFETCHCONTENT_SOURCE_DIR_SDL3="))
                    .map(PathBuf::from)
                    .ok_or("cannot find the SDL source tree of the Android build")?
            });
        }
    }
    let sdl = sdl_src.ok_or("no ABI to build")?;
    Ok((libs, sdl))
}

fn default_version_code() -> u32 {
    // Minutes since 2024-01-01, so every rebuild installs over the previous one.
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    (now.saturating_sub(1_704_067_200) / 60).clamp(1, 2_100_000_000) as u32
}

/// `<meta-data>` value with the player arguments GameActivity passes on (no spaces inside).
pub fn player_args(touch_keys: &[String]) -> String {
    if touch_keys.is_empty() {
        String::new()
    } else {
        format!("--touch-keys {}", touch_keys.join(","))
    }
}

/// Landscape unless the stage is taller than wide.
fn orientations(job: &Job) -> Result<&'static str> {
    let bin = std::fs::read(job.assets_bin())
        .map_err(|e| format!("cannot read {}: {e}", job.assets_bin().display()))?;
    let rd = |o: usize| u16::from_le_bytes([bin[o], bin[o + 1]]);
    let (w, h) = if bin.len() >= 52 {
        (rd(48), rd(50))
    } else {
        (0, 0)
    };
    Ok(if h > w {
        "Portrait PortraitUpsideDown"
    } else {
        "LandscapeLeft LandscapeRight"
    })
}

fn touch_keys(job: &Job) -> Result<Vec<String>> {
    let keys: Vec<String> = match &job.opts.android.touch_keys {
        Some(list) => list.split(',').map(|s| s.trim().to_owned()).collect(),
        None => job.config.android.touch_keys.clone(),
    };
    if keys.len() > 12 {
        return Err("at most 12 touch keys".into());
    }
    if let Some(bad) = keys.iter().find(|k| !config::valid_key_name(k)) {
        return Err(format!(
            "unknown touch key '{bad}' (see docs/user/EXPORT.md)"
        ));
    }
    Ok(keys)
}

fn ensure_debug_keystore(path: &Path) -> Result<()> {
    if path.is_file() {
        return Ok(());
    }
    if let Some(dir) = path.parent() {
        mkdir(dir)?;
    }
    run(
        Command::new(jdk("keytool"))
            .args(["-genkeypair", "-keystore"])
            .arg(path)
            .args([
                "-storepass",
                "android",
                "-alias",
                "androiddebugkey",
                "-keypass",
                "android",
                "-keyalg",
                "RSA",
                "-keysize",
                "2048",
                "-validity",
                "10000",
                "-dname",
                "CN=Android Debug,O=Android,C=US",
            ]),
        "creating the debug keystore",
    )
}

fn debug_keystore() -> Result<PathBuf> {
    if let Some(home) = env_path("ANDROID_USER_HOME") {
        return Ok(home.join("debug.keystore"));
    }
    let home = env_path(if cfg!(windows) { "USERPROFILE" } else { "HOME" })
        .ok_or("no home folder for the debug keystore; pass --keystore")?;
    Ok(home.join(".android/debug.keystore"))
}

fn java_sources(dir: &Path, out: &mut Vec<PathBuf>) -> Result<()> {
    let entries =
        std::fs::read_dir(dir).map_err(|e| format!("cannot list {}: {e}", dir.display()))?;
    for e in entries.flatten() {
        let p = e.path();
        if p.is_dir() {
            java_sources(&p, out)?;
        } else if p.extension().is_some_and(|x| x == "java") {
            out.push(p);
        }
    }
    Ok(())
}

fn copy_tree(from: &Path, to: &Path) -> Result<()> {
    mkdir(to)?;
    let entries =
        std::fs::read_dir(from).map_err(|e| format!("cannot list {}: {e}", from.display()))?;
    for e in entries.flatten() {
        let p = e.path();
        let dest = to.join(e.file_name());
        if p.is_dir() {
            copy_tree(&p, &dest)?;
        } else {
            copy(&p, &dest)?;
        }
    }
    Ok(())
}

pub fn build(job: &Job) -> Result<()> {
    let a = &job.opts.android;
    let abis: Vec<String> = if a.abis.is_empty() {
        DEFAULT_ABIS.iter().map(|s| s.to_string()).collect()
    } else {
        a.abis.clone()
    };
    let package = a
        .package
        .clone()
        .or_else(|| job.config.android.package.clone())
        .unwrap_or_else(|| config::default_package(&job.id));
    if !config::valid_package(&package) {
        return Err(format!("'{package}' is not a valid Android package id"));
    }
    if a.keystore.is_some() != a.keystore_pass_env.is_some() {
        return Err("--keystore and --keystore-pass-env go together".into());
    }
    let keys = touch_keys(job)?;
    let tools = find_tools(job)?;
    let templ = job.source.join("platform/android");
    if !templ.join("AndroidManifest.xml").is_file() {
        return Err(format!("no Android template under {}", templ.display()));
    }

    let (libs, sdl_src) = build_native(job, &tools, &abis)?;

    let dir = job.work.join("android/apk");
    if dir.exists() {
        std::fs::remove_dir_all(&dir)
            .map_err(|e| format!("cannot clean {}: {e}", dir.display()))?;
    }
    mkdir(&dir)?;
    let manifest = config::fill_template(
        &std::fs::read_to_string(templ.join("AndroidManifest.xml"))
            .map_err(|e| format!("cannot read the manifest template: {e}"))?,
        &[
            ("PACKAGE", &package),
            ("LABEL", &config::android_label(&job.title)),
            ("ORIENTATIONS", orientations(job)?),
            ("ARGS", &config::xml_escape(&player_args(&keys))),
        ],
    )?;
    write(&dir.join("AndroidManifest.xml"), &manifest)?;
    let res = dir.join("res");
    copy_tree(&templ.join("res"), &res)?;
    if let Some(icon) = &a.icon {
        let png =
            std::fs::read(icon).map_err(|e| format!("cannot read {}: {e}", icon.display()))?;
        if !png.starts_with(b"\x89PNG") {
            return Err(format!("--icon {} is not a PNG file", icon.display()));
        }
        copy(icon, &res.join("mipmap-xxxhdpi/ic_launcher.png"))?;
    }
    copy(&job.assets_bin(), &dir.join("assets/assets.bin"))?;
    job.copy_runtime_config(&dir.join("assets"))?;

    let exe = std::env::consts::EXE_SUFFIX;
    let aapt2 = tools.build_tools.join(format!("aapt2{exe}"));
    run(
        Command::new(&aapt2)
            .args(["compile", "--dir"])
            .arg(&res)
            .arg("-o")
            .arg(dir.join("res.zip")),
        "aapt2 compile",
    )?;
    let version_code = a
        .version_code
        .or(job.config.android.version_code)
        .unwrap_or_else(default_version_code);
    run(
        Command::new(&aapt2)
            .arg("link")
            .arg("-o")
            .arg(dir.join("unaligned.apk"))
            .arg("-I")
            .arg(&tools.android_jar)
            .arg("--manifest")
            .arg(dir.join("AndroidManifest.xml"))
            .args(["--min-sdk-version", &MIN_SDK.to_string()])
            .args(["--target-sdk-version", &TARGET_SDK.to_string()])
            .args(["--version-code", &version_code.to_string()])
            .args(["--version-name", &job.version])
            .args(["-0", "bin"])
            .arg("-A")
            .arg(dir.join("assets"))
            .arg(dir.join("res.zip")),
        "aapt2 link",
    )?;

    let mut sources = Vec::new();
    java_sources(&templ.join("java"), &mut sources)?;
    java_sources(
        &sdl_src.join("android-project/app/src/main/java"),
        &mut sources,
    )?;
    let list: String = sources
        .iter()
        .map(|p| format!("\"{}\"\n", cmake_path(p)))
        .collect();
    write(&dir.join("sources.txt"), &list)?;
    let mut boot = tools.android_jar.clone().into_os_string();
    let lambda = tools.build_tools.join("core-lambda-stubs.jar");
    if lambda.is_file() {
        boot.push(if cfg!(windows) { ";" } else { ":" });
        boot.push(&lambda);
    }
    run(
        Command::new(jdk("javac"))
            .args([
                "-encoding",
                "UTF-8",
                "-source",
                "8",
                "-target",
                "8",
                "-nowarn",
            ])
            .args(["-Xlint:-options", "-bootclasspath"])
            .arg(&boot)
            .arg("-d")
            .arg(dir.join("classes"))
            .arg(format!("@{}", dir.join("sources.txt").display())),
        "javac",
    )?;
    run(
        Command::new(jdk("jar"))
            .arg("cf")
            .arg(dir.join("classes.jar"))
            .arg("-C")
            .arg(dir.join("classes"))
            .arg("."),
        "jar (classes)",
    )?;
    let stage = dir.join("stage");
    mkdir(&stage)?;
    run(
        Command::new(jdk("java"))
            .arg("-cp")
            .arg(tools.build_tools.join("lib/d8.jar"))
            .args([
                "com.android.tools.r8.D8",
                "--release",
                "--min-api",
                &MIN_SDK.to_string(),
            ])
            .arg("--lib")
            .arg(&tools.android_jar)
            .arg("--output")
            .arg(&stage)
            .arg(dir.join("classes.jar")),
        "d8",
    )?;
    for (abi, lib) in &libs {
        copy(lib, &stage.join("lib").join(abi).join("libmain.so"))?;
    }
    // Stored (-0): extractNativeLibs="false" maps the libraries straight from the APK.
    run(
        Command::new(jdk("jar"))
            .arg("u0Mf")
            .arg(dir.join("unaligned.apk"))
            .arg("-C")
            .arg(&stage)
            .arg("."),
        "jar (dex and libraries)",
    )?;
    run(
        Command::new(tools.build_tools.join(format!("zipalign{exe}")))
            .args(["-f", "-P", "16", "4"])
            .arg(dir.join("unaligned.apk"))
            .arg(dir.join("aligned.apk")),
        "zipalign",
    )?;

    let apk = job.opts.output.join(format!("{}.apk", job.name));
    mkdir(&job.opts.output)?;
    let mut sign = Command::new(jdk("java"));
    sign.arg("-jar")
        .arg(tools.build_tools.join("lib/apksigner.jar"))
        .arg("sign");
    match (&a.keystore, &a.keystore_pass_env) {
        (Some(ks), Some(var)) => {
            if !ks.is_file() {
                return Err(format!("keystore {} does not exist", ks.display()));
            }
            if std::env::var_os(var).is_none() {
                return Err(format!("environment variable {var} is not set"));
            }
            let pass = format!("env:{var}");
            sign.arg("--ks")
                .arg(ks)
                .args([
                    "--ks-key-alias",
                    a.key_alias.as_deref().unwrap_or("release"),
                ])
                .args(["--ks-pass", &pass, "--key-pass", &pass]);
        }
        _ => {
            let ks = debug_keystore()?;
            ensure_debug_keystore(&ks)?;
            sign.arg("--ks")
                .arg(&ks)
                .args(["--ks-key-alias", "androiddebugkey"])
                .args(["--ks-pass", "pass:android", "--key-pass", "pass:android"]);
        }
    }
    sign.arg("--out").arg(&apk).arg(dir.join("aligned.apk"));
    run(&mut sign, "apksigner")?;
    println!(
        "anyswf: {} ({package}, ABIs {})",
        apk.display(),
        abis.join(", ")
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn versions_sort_numerically() {
        assert!(version_key("35.0.0") > version_key("9.0.0"));
        assert!(version_key("android-35") > version_key("android-34"));
        assert_eq!(version_key("27.2.12479018"), [27, 2, 12479018]);
    }

    #[test]
    fn args_for_the_manifest() {
        assert_eq!(player_args(&[]), "");
        assert_eq!(
            player_args(&["left".into(), "space".into()]),
            "--touch-keys left,space"
        );
    }

    #[test]
    fn version_code_is_in_range() {
        let v = default_version_code();
        assert!((1..=2_100_000_000).contains(&v));
    }
}
