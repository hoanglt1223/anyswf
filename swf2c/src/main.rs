use std::path::PathBuf;
use std::process::ExitCode;
use swf2c::{assets_bin, characters, convert, parse};

const USAGE: &str = "\
usage: swf2c <command> [args]

commands:
  inspect <game.swf>                       print the header and character table
  assets <game.swf> -o <assets.bin>        convert shapes, bitmaps, sounds and timelines
         [--tolerance <px>]                curve flattening error in screen pixels (default 0.35)
  help                                     show this message
  version                                  print the swf2c version
";

#[derive(Debug, PartialEq)]
enum Command {
    Help,
    Version,
    Inspect(PathBuf),
    Assets {
        input: PathBuf,
        output: PathBuf,
        tolerance: f32,
    },
}

#[derive(Debug, PartialEq, Eq)]
struct ArgError(String);

fn parse_args<I: IntoIterator<Item = String>>(args: I) -> Result<Command, ArgError> {
    let mut args = args.into_iter();
    match args.next().as_deref() {
        None | Some("help" | "-h" | "--help") => Ok(Command::Help),
        Some("version" | "-V" | "--version") => Ok(Command::Version),
        Some("inspect") => match (args.next(), args.next()) {
            (Some(p), None) => Ok(Command::Inspect(p.into())),
            _ => Err(ArgError("inspect takes exactly one SWF path".into())),
        },
        Some("assets") => {
            let mut input = None;
            let mut output = None;
            let mut tolerance = convert::Options::default().tolerance_px;
            while let Some(a) = args.next() {
                match a.as_str() {
                    "-o" => output = Some(args.next().ok_or(ArgError("-o needs a path".into()))?),
                    "--tolerance" => {
                        let v = args
                            .next()
                            .ok_or(ArgError("--tolerance needs a value".into()))?;
                        tolerance = v
                            .parse::<f32>()
                            .ok()
                            .filter(|t| t.is_finite() && *t > 0.0 && *t <= 10.0)
                            .ok_or(ArgError(format!("bad tolerance '{v}' (0 < px <= 10)")))?;
                    }
                    _ if input.is_none() && !a.starts_with('-') => input = Some(a),
                    _ => return Err(ArgError(format!("unexpected argument '{a}'"))),
                }
            }
            match (input, output) {
                (Some(i), Some(o)) => Ok(Command::Assets {
                    input: i.into(),
                    output: o.into(),
                    tolerance,
                }),
                _ => Err(ArgError(
                    "assets needs <game.swf> and -o <assets.bin>".into(),
                )),
            }
        }
        Some(other) => Err(ArgError(format!("unknown command '{other}'"))),
    }
}

fn run(cmd: Command) -> Result<(), swf2c::Error> {
    match cmd {
        Command::Help => print!("{USAGE}"),
        Command::Version => println!("swf2c {}", env!("CARGO_PKG_VERSION")),
        Command::Inspect(path) => {
            let buf = parse::read_file(&path)?;
            let swf = parse::parse(&buf)?;
            let chars = characters::Characters::collect(swf.header.version(), &swf.tags);
            print!("{}", characters::report(&swf, &chars));
        }
        Command::Assets {
            input,
            output,
            tolerance,
        } => {
            let buf = parse::read_file(&input)?;
            let swf = parse::parse(&buf)?;
            let out = convert::convert(
                &swf,
                &convert::Options {
                    tolerance_px: tolerance,
                },
            )?;
            for w in &out.warnings {
                eprintln!("swf2c: warning: {w}");
            }
            let bin = assets_bin::serialize(&out.assets);
            std::fs::write(&output, &bin).map_err(|e| {
                swf2c::Error::new(format!("cannot write {}: {e}", output.display()))
            })?;
            let s = &out.stats;
            println!(
                "swf2c: {} bytes -> {} ({} draws, {} triangles, {} KiB textures, {} KiB sound)",
                bin.len(),
                output.display(),
                s.draws,
                s.triangles,
                s.texture_bytes / 1024,
                s.sound_bytes / 1024
            );
        }
    }
    Ok(())
}

fn main() -> ExitCode {
    let cmd = match parse_args(std::env::args().skip(1)) {
        Ok(c) => c,
        Err(ArgError(msg)) => {
            eprint!("swf2c: {msg}\n\n{USAGE}");
            return ExitCode::from(2);
        }
    };
    match run(cmd) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("swf2c: {e}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(args: &[&str]) -> Result<Command, ArgError> {
        parse_args(args.iter().map(|s| s.to_string()))
    }

    #[test]
    fn no_args_shows_help() {
        assert_eq!(parse(&[]), Ok(Command::Help));
        assert_eq!(parse(&["--help"]), Ok(Command::Help));
    }

    #[test]
    fn version_aliases() {
        assert_eq!(parse(&["version"]), Ok(Command::Version));
        assert_eq!(parse(&["-V"]), Ok(Command::Version));
    }

    #[test]
    fn unknown_command_is_rejected() {
        assert!(parse(&["frobnicate"]).is_err());
    }

    #[test]
    fn assets_arguments() {
        assert_eq!(
            parse(&["assets", "a.swf", "-o", "a.bin", "--tolerance", "0.5"]),
            Ok(Command::Assets {
                input: "a.swf".into(),
                output: "a.bin".into(),
                tolerance: 0.5
            })
        );
        assert!(parse(&["assets", "a.swf"]).is_err());
        assert!(parse(&["assets", "a.swf", "-o", "x", "--tolerance", "-1"]).is_err());
        assert!(parse(&["inspect"]).is_err());
    }
}
