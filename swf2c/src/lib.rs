//! swf2c library: SWF parsing, asset conversion, and (later) AVM1-to-C compilation.

pub mod assets_bin;
pub mod avm1;
pub mod bitmaps;
pub mod characters;
pub mod convert;
pub mod device_font;
pub mod gradients;
pub mod morph;
pub mod parse;
pub mod scripts;
pub mod sounds;
pub mod tessellate;
pub mod text;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error(String);

impl Error {
    pub fn new(msg: impl Into<String>) -> Self {
        Error(msg.into())
    }
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;
