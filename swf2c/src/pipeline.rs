//! Shared by the swf2c and anyswf command lines: SWF to assets.bin and game.c.

use crate::{Error, Result, assets_bin, convert, parse};
use std::path::Path;

/// Curve flattening error in screen pixels, as accepted on the command line.
pub fn parse_tolerance(v: &str) -> Option<f32> {
    v.parse::<f32>()
        .ok()
        .filter(|t| t.is_finite() && *t > 0.0 && *t <= 10.0)
}

fn write_file(path: &Path, data: &[u8]) -> Result<()> {
    std::fs::write(path, data)
        .map_err(|e| Error::new(format!("cannot write {}: {e}", path.display())))
}

/// Converts the movie's assets and writes them to `output`, reporting warnings on stderr
/// and a summary on stdout.
pub fn convert_assets(swf: &swf::Swf, opts: &convert::Options, output: &Path) -> Result<()> {
    let out = convert::convert(swf, opts)?;
    for w in &out.warnings {
        eprintln!("swf2c: warning: {w}");
    }
    let bin = assets_bin::serialize(&out.assets);
    write_file(output, &bin)?;
    let s = &out.stats;
    println!(
        "swf2c: {} bytes -> {} ({}x{} screen, {} draws, {} triangles, {} KiB textures, {} KiB sound)",
        bin.len(),
        output.display(),
        out.assets.screen.0,
        out.assets.screen.1,
        s.draws,
        s.triangles,
        s.texture_bytes / 1024,
        s.sound_bytes / 1024
    );
    Ok(())
}

/// Writes `<output>/assets.bin` and `<output>/game.c` (the compiled ActionScript).
pub fn compile(input: &Path, output: &Path, opts: &convert::Options) -> Result<()> {
    let buf = parse::read_file(input)?;
    let swf = parse::parse(&buf)?;
    std::fs::create_dir_all(output)
        .map_err(|e| Error::new(format!("cannot create {}: {e}", output.display())))?;
    convert_assets(&swf, opts, &output.join("assets.bin"))?;
    let scripts = crate::scripts::Scripts::collect(&swf.tags);
    let name = input
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let code = crate::avm1::emit_c::emit_movie(swf.header.version(), &scripts, &name)?;
    for w in &code.warnings {
        eprintln!("swf2c: warning: {w}");
    }
    let c_path = output.join("game.c");
    write_file(&c_path, code.source.as_bytes())?;
    println!(
        "swf2c: {} scripts, {} functions ({} with a dynamic stack) -> {}",
        scripts.list.len(),
        code.functions,
        code.dynamic_blocks,
        c_path.display()
    );
    Ok(())
}
