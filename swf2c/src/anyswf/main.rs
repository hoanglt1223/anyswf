//! anyswf: builds native games from SWF files (swf2c compile + the AnySWF runtime).

mod android;
mod config;
mod export;

use export::{AndroidOptions, BuildOptions, BuildTarget};
use std::path::PathBuf;
use std::process::ExitCode;

const USAGE: &str = "\
usage: anyswf <command> [args]

commands:
  build <game.swf> --target <target> -o <dir>    compile a SWF into a native game in <dir>
      targets: psp      EBOOT.PBP + assets.bin (pspdev toolchain via $PSPDEV or Docker)
               linux    <name> + assets.bin       (on Linux)
               windows  <name>.exe + assets.bin   (on Windows, MSVC)
               macos    <name>.app, ad-hoc signed (on macOS)
               android  <name>.apk (Android SDK + NDK, JDK 17+)
      [--config <game.toml>]   title, id, version, tolerance and [android] settings
      [--title <title>]        window, EBOOT and app name (default: config, else file name)
      [--id <id>]              save folder name (default: derived from the title)
      [--tolerance <px>]       curve flattening error in screen pixels (default 0.35)
      [--source <dir>]         AnySWF source tree (default: $ANYSWF_SOURCE, else the tree
                               this program was built in or shipped with)
      [--work <dir>]           build files, reused by later builds (default: <dir>.work)
      [--jobs <n>]             parallel compile jobs
      [--cmake-arg <arg>]      extra CMake configure argument (repeatable)
    android only:
      [--abi <abi,...>]        arm64-v8a, armeabi-v7a, x86_64, x86 (default arm64-v8a,x86_64)
      [--package <id>]         application id (default io.anyport.anyswf.<id>)
      [--version-code <n>]     (default: minutes since 2024, so rebuilds install as updates)
      [--touch-keys <k,...>]   on-screen buttons, e.g. left,right,up,down,space
      [--icon <png>]           launcher icon
      [--android-sdk <dir>] [--android-ndk <dir>]
      [--keystore <file> --keystore-pass-env <VAR> [--key-alias <alias>]]
                               release signing (default: the debug keystore)
  help                                           show this message
  version                                        print the version
";

#[derive(Debug, PartialEq)]
enum Cmd {
    Help,
    Version,
    Build(Box<BuildOptions>),
}

fn parse_args<I: IntoIterator<Item = String>>(args: I) -> Result<Cmd, String> {
    let mut args = args.into_iter();
    match args.next().as_deref() {
        None | Some("help" | "-h" | "--help") => Ok(Cmd::Help),
        Some("version" | "-V" | "--version") => Ok(Cmd::Version),
        Some("build") => parse_build(args).map(|o| Cmd::Build(Box::new(o))),
        Some(other) => Err(format!("unknown command '{other}'")),
    }
}

fn parse_build(mut args: impl Iterator<Item = String>) -> Result<BuildOptions, String> {
    let mut input = None;
    let mut target = None;
    let mut output = None;
    let mut o = BuildOptions {
        input: PathBuf::new(),
        target: BuildTarget::Linux,
        output: PathBuf::new(),
        config: None,
        title: None,
        id: None,
        tolerance: None,
        source: None,
        work: None,
        jobs: None,
        cmake_args: Vec::new(),
        android: AndroidOptions::default(),
    };
    while let Some(a) = args.next() {
        if !a.starts_with('-') {
            if input.replace(PathBuf::from(&a)).is_some() {
                return Err(format!("unexpected argument '{a}'"));
            }
            continue;
        }
        let mut value = || args.next().ok_or_else(|| format!("{a} needs a value"));
        match a.as_str() {
            "--target" => {
                let v = value()?;
                target = Some(BuildTarget::parse(&v).ok_or_else(|| {
                    format!("unknown target '{v}' (psp, linux, windows, macos, android)")
                })?);
            }
            "-o" | "--output" => output = Some(PathBuf::from(value()?)),
            "--config" => o.config = Some(value()?.into()),
            "--title" => {
                let v = value()?;
                if v.trim().is_empty() {
                    return Err("--title is empty".into());
                }
                o.title = Some(v);
            }
            "--id" => o.id = Some(value()?),
            "--tolerance" => {
                let v = value()?;
                o.tolerance = Some(
                    swf2c::pipeline::parse_tolerance(&v)
                        .ok_or_else(|| format!("bad tolerance '{v}' (0 < px <= 10)"))?,
                );
            }
            "--source" => o.source = Some(value()?.into()),
            "--work" => o.work = Some(value()?.into()),
            "--jobs" => {
                let v = value()?;
                o.jobs = Some(
                    v.parse()
                        .ok()
                        .filter(|n| *n > 0)
                        .ok_or_else(|| format!("bad --jobs '{v}'"))?,
                );
            }
            "--cmake-arg" => o.cmake_args.push(value()?),
            "--abi" => {
                for abi in value()?.split(',') {
                    if !android::ABIS.contains(&abi) {
                        return Err(format!(
                            "unknown ABI '{abi}' ({})",
                            android::ABIS.join(", ")
                        ));
                    }
                    if !o.android.abis.iter().any(|x| x == abi) {
                        o.android.abis.push(abi.to_owned());
                    }
                }
            }
            "--package" => {
                let v = value()?;
                if !config::valid_package(&v) {
                    return Err(format!("'{v}' is not a valid Android package id"));
                }
                o.android.package = Some(v);
            }
            "--version-code" => {
                let v = value()?;
                o.android.version_code = Some(
                    v.parse()
                        .ok()
                        .filter(|n| (1..=2_100_000_000).contains(n))
                        .ok_or_else(|| format!("bad --version-code '{v}'"))?,
                );
            }
            "--touch-keys" => o.android.touch_keys = Some(value()?),
            "--icon" => o.android.icon = Some(value()?.into()),
            "--android-sdk" => o.android.sdk = Some(value()?.into()),
            "--android-ndk" => o.android.ndk = Some(value()?.into()),
            "--keystore" => o.android.keystore = Some(value()?.into()),
            "--keystore-pass-env" => o.android.keystore_pass_env = Some(value()?),
            "--key-alias" => o.android.key_alias = Some(value()?),
            _ => return Err(format!("unknown option '{a}'")),
        }
    }
    o.input = input.ok_or("build needs a SWF file")?;
    o.target = target.ok_or("build needs --target")?;
    o.output = output.ok_or("build needs -o <dir>")?;
    Ok(o)
}

fn main() -> ExitCode {
    let cmd = match parse_args(std::env::args().skip(1)) {
        Ok(c) => c,
        Err(msg) => {
            eprint!("anyswf: {msg}\n\n{USAGE}");
            return ExitCode::from(2);
        }
    };
    let result = match cmd {
        Cmd::Help => {
            print!("{USAGE}");
            Ok(())
        }
        Cmd::Version => {
            println!("anyswf {}", env!("CARGO_PKG_VERSION"));
            Ok(())
        }
        Cmd::Build(o) => export::build(*o),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("anyswf: {e}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(args: &[&str]) -> Result<Cmd, String> {
        parse_args(args.iter().map(|s| s.to_string()))
    }

    #[test]
    fn help_and_version() {
        assert_eq!(parse(&[]), Ok(Cmd::Help));
        assert_eq!(parse(&["-V"]), Ok(Cmd::Version));
        assert!(parse(&["frobnicate"]).is_err());
    }

    #[test]
    fn build_arguments() {
        let Ok(Cmd::Build(o)) = parse(&[
            "build",
            "g.swf",
            "--target",
            "android",
            "-o",
            "out",
            "--abi",
            "arm64-v8a,x86_64,arm64-v8a",
            "--touch-keys",
            "left,space",
            "--cmake-arg",
            "-DX=1",
            "--jobs",
            "4",
        ]) else {
            panic!("expected build");
        };
        assert_eq!(o.input, PathBuf::from("g.swf"));
        assert_eq!(o.target, BuildTarget::Android);
        assert_eq!(o.output, PathBuf::from("out"));
        assert_eq!(o.android.abis, ["arm64-v8a", "x86_64"]);
        assert_eq!(o.android.touch_keys.as_deref(), Some("left,space"));
        assert_eq!(o.cmake_args, ["-DX=1"]);
        assert_eq!(o.jobs, Some(4));
    }

    #[test]
    fn build_errors() {
        assert!(parse(&["build", "g.swf", "-o", "out"]).is_err());
        assert!(parse(&["build", "g.swf", "--target", "linux"]).is_err());
        assert!(parse(&["build", "--target", "linux", "-o", "out"]).is_err());
        assert!(parse(&["build", "g.swf", "--target", "ps2", "-o", "o"]).is_err());
        assert!(parse(&["build", "g.swf", "h.swf", "--target", "psp", "-o", "o"]).is_err());
        assert!(
            parse(&[
                "build", "g.swf", "--target", "psp", "-o", "o", "--abi", "mips"
            ])
            .is_err()
        );
        assert!(
            parse(&[
                "build",
                "g.swf",
                "--target",
                "psp",
                "-o",
                "o",
                "--package",
                "x"
            ])
            .is_err()
        );
        assert!(
            parse(&[
                "build", "g.swf", "--target", "psp", "-o", "o", "--jobs", "0"
            ])
            .is_err()
        );
        assert!(parse(&["build", "g.swf", "--target", "psp", "-o"]).is_err());
    }
}
