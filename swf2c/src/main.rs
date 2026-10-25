use std::path::PathBuf;
use std::process::ExitCode;
use swf2c::targets::Target;
use swf2c::{characters, convert, parse, pipeline};

const USAGE: &str = "\
usage: swf2c <command> [args]

commands:
  inspect <game.swf>                       print the header and character table
  assets <game.swf> -o <assets.bin>        convert shapes, bitmaps, sounds and timelines
         [--tolerance <px>]                curve flattening error in screen pixels (default 0.35)
         [--target psp|pc|android]         screen and detail settings (default psp)
  compile <game.swf> -o <dir>              write <dir>/game.c (compiled ActionScript) and
         [--tolerance <px>] [--target ...] <dir>/assets.bin
  help                                     show this message
  version                                  print the swf2c version
";

enum Command {
    Help,
    Version,
    Inspect(PathBuf),
    Assets {
        input: PathBuf,
        output: PathBuf,
        options: convert::Options,
    },
    Compile {
        input: PathBuf,
        output: PathBuf,
        options: convert::Options,
    },
}

#[derive(Debug)]
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
        Some(cmd @ ("assets" | "compile")) => {
            let compile = cmd == "compile";
            let mut input = None;
            let mut output = None;
            let mut tolerance = None;
            let mut target = Target::Psp;
            while let Some(a) = args.next() {
                match a.as_str() {
                    "-o" => output = Some(args.next().ok_or(ArgError("-o needs a path".into()))?),
                    "--tolerance" => {
                        let v = args
                            .next()
                            .ok_or(ArgError("--tolerance needs a value".into()))?;
                        tolerance = Some(
                            pipeline::parse_tolerance(&v)
                                .ok_or(ArgError(format!("bad tolerance '{v}' (0 < px <= 10)")))?,
                        );
                    }
                    "--target" => {
                        let v = args
                            .next()
                            .ok_or(ArgError("--target needs a name".into()))?;
                        target = Target::parse(&v)
                            .ok_or(ArgError(format!("unknown target '{v}' (psp, pc, android)")))?;
                    }
                    _ if input.is_none() && !a.starts_with('-') => input = Some(a),
                    _ => return Err(ArgError(format!("unexpected argument '{a}'"))),
                }
            }
            let options = target.options(tolerance);
            match (input, output) {
                (Some(i), Some(o)) if compile => Ok(Command::Compile {
                    input: i.into(),
                    output: o.into(),
                    options,
                }),
                (Some(i), Some(o)) => Ok(Command::Assets {
                    input: i.into(),
                    output: o.into(),
                    options,
                }),
                _ => Err(ArgError(format!("{cmd} needs <game.swf> and -o <path>"))),
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
            options,
        } => {
            let buf = parse::read_file(&input)?;
            let swf = parse::parse(&buf)?;
            pipeline::convert_assets(&swf, &options, &output)?;
        }
        Command::Compile {
            input,
            output,
            options,
        } => pipeline::compile(&input, &output, &options)?,
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
        assert!(matches!(parse(&[]), Ok(Command::Help)));
        assert!(matches!(parse(&["--help"]), Ok(Command::Help)));
    }

    #[test]
    fn version_aliases() {
        assert!(matches!(parse(&["version"]), Ok(Command::Version)));
        assert!(matches!(parse(&["-V"]), Ok(Command::Version)));
    }

    #[test]
    fn unknown_command_is_rejected() {
        assert!(parse(&["frobnicate"]).is_err());
    }

    #[test]
    fn assets_arguments() {
        match parse(&["assets", "a.swf", "-o", "a.bin", "--tolerance", "0.5"]) {
            Ok(Command::Assets {
                input,
                output,
                options,
            }) => {
                assert_eq!((input, output), ("a.swf".into(), "a.bin".into()));
                assert_eq!(options.tolerance_px, 0.5);
                assert_eq!(options.screen, Some((480, 272)));
            }
            _ => panic!("expected assets"),
        }
        match parse(&["compile", "a.swf", "--target", "pc", "-o", "d"]) {
            Ok(Command::Compile { options, .. }) => assert_eq!(options.screen, None),
            _ => panic!("expected compile"),
        }
        assert!(parse(&["compile", "a.swf", "-o", "d", "--target", "ps2"]).is_err());
        assert!(parse(&["assets", "a.swf"]).is_err());
        assert!(parse(&["assets", "a.swf", "-o", "x", "--tolerance", "-1"]).is_err());
        assert!(parse(&["inspect"]).is_err());
    }
}
