use std::process::ExitCode;

const USAGE: &str = "\
usage: swf2c <command>

commands:
  help       show this message
  version    print the swf2c version
";

#[derive(Debug, PartialEq, Eq)]
enum Command {
    Help,
    Version,
}

#[derive(Debug, PartialEq, Eq)]
enum ArgError {
    Unknown(String),
}

fn parse_args<I: IntoIterator<Item = String>>(args: I) -> Result<Command, ArgError> {
    let mut args = args.into_iter();
    match args.next().as_deref() {
        None | Some("help" | "-h" | "--help") => Ok(Command::Help),
        Some("version" | "-V" | "--version") => Ok(Command::Version),
        Some(other) => Err(ArgError::Unknown(other.to_owned())),
    }
}

fn main() -> ExitCode {
    match parse_args(std::env::args().skip(1)) {
        Ok(Command::Help) => {
            print!("{USAGE}");
            ExitCode::SUCCESS
        }
        Ok(Command::Version) => {
            println!("swf2c {}", env!("CARGO_PKG_VERSION"));
            ExitCode::SUCCESS
        }
        Err(ArgError::Unknown(cmd)) => {
            eprint!("swf2c: unknown command '{cmd}'\n\n{USAGE}");
            ExitCode::from(2)
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
        assert_eq!(
            parse(&["frobnicate"]),
            Err(ArgError::Unknown("frobnicate".into()))
        );
    }
}
