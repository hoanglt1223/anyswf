//! `anyswf build`: SWF -> game.c + assets.bin -> native program for one target.

use crate::config::{self, GameConfig};
use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::process::Command;
use swf2c::targets::Target;

pub type Result<T> = std::result::Result<T, String>;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BuildTarget {
    Psp,
    Linux,
    Windows,
    Macos,
    Android,
}

impl BuildTarget {
    pub fn parse(name: &str) -> Option<BuildTarget> {
        Some(match name {
            "psp" => BuildTarget::Psp,
            "linux" => BuildTarget::Linux,
            "windows" => BuildTarget::Windows,
            "macos" => BuildTarget::Macos,
            "android" => BuildTarget::Android,
            _ => return None,
        })
    }

    pub fn name(self) -> &'static str {
        match self {
            BuildTarget::Psp => "psp",
            BuildTarget::Linux => "linux",
            BuildTarget::Windows => "windows",
            BuildTarget::Macos => "macos",
            BuildTarget::Android => "android",
        }
    }

    fn conversion(self) -> Target {
        match self {
            BuildTarget::Psp => Target::Psp,
            BuildTarget::Android => Target::Android,
            _ => Target::Desktop,
        }
    }

    /// Desktop targets build with the host compiler, so only on their own OS.
    fn host_ok(self) -> bool {
        match self {
            BuildTarget::Linux => cfg!(target_os = "linux"),
            BuildTarget::Windows => cfg!(windows),
            BuildTarget::Macos => cfg!(target_os = "macos"),
            BuildTarget::Psp | BuildTarget::Android => true,
        }
    }
}

#[derive(Debug, Default, Clone, PartialEq)]
pub struct AndroidOptions {
    pub abis: Vec<String>,
    pub package: Option<String>,
    pub version_code: Option<u32>,
    pub touch_keys: Option<String>,
    pub sdk: Option<PathBuf>,
    pub ndk: Option<PathBuf>,
    pub keystore: Option<PathBuf>,
    pub keystore_pass_env: Option<String>,
    pub key_alias: Option<String>,
    pub icon: Option<PathBuf>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct BuildOptions {
    pub input: PathBuf,
    pub target: BuildTarget,
    pub output: PathBuf,
    pub config: Option<PathBuf>,
    pub title: Option<String>,
    pub id: Option<String>,
    pub tolerance: Option<f32>,
    pub source: Option<PathBuf>,
    pub work: Option<PathBuf>,
    pub jobs: Option<u32>,
    pub cmake_args: Vec<String>,
    pub android: AndroidOptions,
}

/// Everything later steps need, resolved once.
pub struct Job {
    pub opts: BuildOptions,
    pub config: GameConfig,
    pub title: String,
    pub name: String,
    pub id: String,
    pub version: String,
    pub source: PathBuf,
    pub work: PathBuf,
    pub gen_dir: PathBuf,
}

pub fn run(cmd: &mut Command, what: &str) -> Result<()> {
    let line: Vec<String> = std::iter::once(cmd.get_program())
        .chain(cmd.get_args())
        .map(|a| a.to_string_lossy().into_owned())
        .collect();
    println!("anyswf: $ {}", line.join(" "));
    let status = cmd
        .status()
        .map_err(|e| format!("{what}: cannot run {}: {e}", line[0]))?;
    if status.success() {
        Ok(())
    } else {
        Err(format!("{what} failed ({status})"))
    }
}

pub fn on_path(program: &str) -> bool {
    let exe = format!("{program}{}", std::env::consts::EXE_SUFFIX);
    std::env::var_os("PATH")
        .is_some_and(|p| std::env::split_paths(&p).any(|d| d.join(&exe).is_file()))
}

pub fn copy(from: &Path, to: &Path) -> Result<()> {
    if let Some(parent) = to.parent() {
        mkdir(parent)?;
    }
    std::fs::copy(from, to)
        .map(|_| ())
        .map_err(|e| format!("cannot copy {} to {}: {e}", from.display(), to.display()))
}

pub fn mkdir(dir: &Path) -> Result<()> {
    std::fs::create_dir_all(dir).map_err(|e| format!("cannot create {}: {e}", dir.display()))
}

pub fn write(path: &Path, text: &str) -> Result<()> {
    if let Some(parent) = path.parent() {
        mkdir(parent)?;
    }
    std::fs::write(path, text).map_err(|e| format!("cannot write {}: {e}", path.display()))
}

fn is_source_tree(dir: &Path) -> bool {
    dir.join("cmake/anyswf-game.cmake").is_file() && dir.join("runtime/CMakeLists.txt").is_file()
}

/// --source, then $ANYSWF_SOURCE, then a source tree this program was built in or shipped
/// with (`<prefix>/share/anyswf/source` next to `<prefix>/bin`).
fn find_source(explicit: Option<&Path>) -> Result<PathBuf> {
    if let Some(dir) = explicit {
        return if is_source_tree(dir) {
            Ok(dir.to_path_buf())
        } else {
            Err(format!(
                "--source {} is not an AnySWF source tree",
                dir.display()
            ))
        };
    }
    if let Some(dir) = std::env::var_os("ANYSWF_SOURCE").map(PathBuf::from) {
        return if is_source_tree(&dir) {
            Ok(dir)
        } else {
            Err(format!(
                "ANYSWF_SOURCE={} is not an AnySWF source tree",
                dir.display()
            ))
        };
    }
    let exe = std::env::current_exe().map_err(|e| format!("cannot locate anyswf: {e}"))?;
    for dir in exe.ancestors().skip(1) {
        for candidate in [dir.to_path_buf(), dir.join("share/anyswf/source")] {
            if is_source_tree(&candidate) {
                return Ok(candidate);
            }
        }
    }
    Err("no AnySWF source tree found; pass --source <dir> or set ANYSWF_SOURCE".into())
}

fn absolute(p: &Path) -> Result<PathBuf> {
    std::path::absolute(p).map_err(|e| format!("bad path {}: {e}", p.display()))
}

impl Job {
    pub fn new(opts: BuildOptions) -> Result<Job> {
        if !opts.target.host_ok() {
            return Err(format!(
                "--target {} builds only on {} itself (the host compiler is used); this is {}",
                opts.target.name(),
                opts.target.name(),
                std::env::consts::OS
            ));
        }
        if !opts.input.is_file() {
            return Err(format!("{} does not exist", opts.input.display()));
        }
        let config = match &opts.config {
            Some(p) => GameConfig::load(p)?,
            None => GameConfig::default(),
        };
        let stem = opts
            .input
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
        let title = opts
            .title
            .clone()
            .or_else(|| config.title.clone())
            .unwrap_or(stem);
        let name = config::file_name(&title);
        let id = opts
            .id
            .clone()
            .or_else(|| config.id.clone())
            .unwrap_or_else(|| name.clone());
        if !config::valid_id(&id) {
            return Err(format!(
                "id '{id}' may only use A-Z a-z 0-9 _ - (at most 32 characters)"
            ));
        }
        let version = config.version.clone().unwrap_or_else(|| "1.0".into());
        let source = absolute(&find_source(opts.source.as_deref())?)?;
        let output = absolute(&opts.output)?;
        let work = match &opts.work {
            Some(w) => absolute(w)?,
            None => {
                let mut w = output.clone().into_os_string();
                w.push(".work");
                PathBuf::from(w)
            }
        };
        let gen_dir = work.join("gen");
        Ok(Job {
            opts: BuildOptions { output, ..opts },
            config,
            title,
            name,
            id,
            version,
            source,
            work,
            gen_dir,
        })
    }

    fn compile_movie(&self) -> Result<()> {
        let tolerance = self.opts.tolerance.or(self.config.tolerance);
        let options = self.opts.target.conversion().options(tolerance);
        println!(
            "anyswf: compiling {} for {}",
            self.opts.input.display(),
            self.opts.target.name()
        );
        swf2c::pipeline::compile(&self.opts.input, &self.gen_dir, &options)
            .map_err(|e| e.to_string())
    }

    /// CMake cache entries that select game-only mode.
    pub fn game_defines(&self, game_c: &Path) -> Vec<String> {
        vec![
            "-DCMAKE_BUILD_TYPE=Release".into(),
            format!("-DANYSWF_GAME_C={}", cmake_path(game_c)),
            format!("-DANYSWF_GAME_NAME={}", self.name),
            // ';' would split a CMake list; quotes and backslashes cannot go into the C string.
            format!(
                "-DANYSWF_GAME_TITLE={}",
                self.title.replace([';', '"', '\\'], "")
            ),
            format!("-DANYSWF_GAME_ID={}", self.id),
        ]
    }

    pub fn parallel_args(&self) -> Vec<String> {
        match self.opts.jobs {
            Some(n) => vec!["--parallel".into(), n.to_string()],
            None => vec!["--parallel".into()],
        }
    }

    pub fn assets_bin(&self) -> PathBuf {
        self.gen_dir.join("assets.bin")
    }
}

/// CMake wants forward slashes, also on Windows.
pub fn cmake_path(p: &Path) -> String {
    p.to_string_lossy().replace('\\', "/")
}

pub fn build(opts: BuildOptions) -> Result<()> {
    let job = Job::new(opts)?;
    mkdir(&job.work)?;
    job.compile_movie()?;
    match job.opts.target {
        BuildTarget::Psp => build_psp(&job),
        BuildTarget::Android => crate::android::build(&job),
        _ => build_desktop(&job),
    }
}

fn find_built(dir: &Path, file: &str) -> Result<PathBuf> {
    for c in [dir.join(file), dir.join("Release").join(file)] {
        if c.is_file() {
            return Ok(c);
        }
    }
    Err(format!(
        "the build produced no {file} under {}",
        dir.display()
    ))
}

fn build_desktop(job: &Job) -> Result<()> {
    let bin = job.work.join(job.opts.target.name());
    let mut configure = Command::new("cmake");
    configure.arg("-S").arg(&job.source).arg("-B").arg(&bin);
    // Ninja needs a compiler environment on Windows; there CMake picks Visual Studio.
    if std::env::var_os("CMAKE_GENERATOR").is_none() && !cfg!(windows) && on_path("ninja") {
        configure.args(["-G", "Ninja"]);
    }
    configure
        .args(job.game_defines(&job.gen_dir.join("game.c")))
        .args(&job.opts.cmake_args);
    run(&mut configure, "CMake configure")?;
    run(
        Command::new("cmake")
            .arg("--build")
            .arg(&bin)
            .args(["--config", "Release", "--target", &job.name])
            .args(job.parallel_args()),
        "CMake build",
    )?;
    let exe_name = format!("{}{}", job.name, std::env::consts::EXE_SUFFIX);
    let exe = find_built(&bin, &exe_name)?;
    let out = &job.opts.output;
    if job.opts.target == BuildTarget::Macos {
        let app = out.join(format!("{}.app", job.name));
        let contents = app.join("Contents");
        copy(&exe, &contents.join("MacOS").join(&job.name))?;
        copy(&job.assets_bin(), &contents.join("Resources/assets.bin"))?;
        write(&contents.join("Info.plist"), &info_plist(job))?;
        if on_path("codesign") {
            run(
                Command::new("codesign")
                    .args(["--force", "--deep", "--sign", "-"])
                    .arg(&app),
                "ad-hoc signing",
            )?;
        } else {
            eprintln!(
                "anyswf: warning: codesign not found; {} is unsigned",
                app.display()
            );
        }
        println!("anyswf: {}", app.display());
    } else {
        let dest = out.join(&exe_name);
        copy(&exe, &dest)?;
        copy(&job.assets_bin(), &out.join("assets.bin"))?;
        println!("anyswf: {} (+ assets.bin)", dest.display());
    }
    Ok(())
}

pub fn info_plist(job: &Job) -> String {
    let bundle_id = job
        .opts
        .android
        .package
        .clone()
        .or_else(|| job.config.android.package.clone())
        .unwrap_or_else(|| config::default_package(&job.id));
    let e = config::xml_escape;
    format!(
        r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>{exe}</string>
    <key>CFBundleIdentifier</key><string>{id}</string>
    <key>CFBundleName</key><string>{title}</string>
    <key>CFBundleDisplayName</key><string>{title}</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>{version}</string>
    <key>CFBundleVersion</key><string>{version}</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
</dict>
</plist>
"#,
        exe = e(&job.name),
        id = e(&bundle_id),
        title = e(&job.title),
        version = e(&job.version),
    )
}

const PSP_IMAGE: &str = "anyswf-psp-build:local";

fn build_psp(job: &Job) -> Result<()> {
    let bin = job.work.join("psp");
    let eboot = bin.join("EBOOT.PBP");
    if eboot.exists() {
        std::fs::remove_file(&eboot)
            .map_err(|e| format!("cannot remove {}: {e}", eboot.display()))?;
    }
    if std::env::var_os("PSPDEV").is_some() {
        let mut configure = Command::new("cmake");
        configure
            .arg("-S")
            .arg(&job.source)
            .arg("-B")
            .arg(&bin)
            .arg(format!(
                "-DCMAKE_TOOLCHAIN_FILE={}",
                cmake_path(&job.source.join("cmake/psp.cmake"))
            ))
            .args(job.game_defines(&job.gen_dir.join("game.c")))
            .args(&job.opts.cmake_args);
        run(&mut configure, "CMake configure (PSP)")?;
        run(
            Command::new("cmake")
                .arg("--build")
                .arg(&bin)
                .args(job.parallel_args()),
            "CMake build (PSP)",
        )?;
    } else {
        build_psp_in_docker(job)?;
    }
    if !eboot.is_file() {
        return Err(format!("the PSP build produced no {}", eboot.display()));
    }
    let out = &job.opts.output;
    copy(&eboot, &out.join("EBOOT.PBP"))?;
    copy(&job.assets_bin(), &out.join("assets.bin"))?;
    println!(
        "anyswf: {} (+ assets.bin; copy the folder to ms0:/PSP/GAME/)",
        out.join("EBOOT.PBP").display()
    );
    Ok(())
}

/// The pspdev toolchain from the pinned image in docker/, with the source tree mounted
/// read-only at /src and the work folder at /work.
fn build_psp_in_docker(job: &Job) -> Result<()> {
    if !on_path("docker") {
        return Err("--target psp needs the pspdev toolchain (set PSPDEV) or Docker".into());
    }
    let have_image = Command::new("docker")
        .args(["image", "inspect", PSP_IMAGE])
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()
        .is_ok_and(|s| s.success());
    if !have_image {
        run(
            Command::new("docker")
                .args(["build", "-t", PSP_IMAGE, "-f"])
                .arg(job.source.join("docker/psp-build.Dockerfile"))
                .arg(job.source.join("docker")),
            "building the PSP toolchain image",
        )?;
    }
    let docker = |args: Vec<String>| {
        let mut c = Command::new("docker");
        c.args(["run", "--rm"]);
        if let Some(user) = owner_of(&job.work) {
            c.args(["--user", &user]);
        }
        c.arg("-v")
            .arg(mount(&job.source, "/src:ro"))
            .arg("-v")
            .arg(mount(&job.work, "/work"))
            .arg(PSP_IMAGE)
            .args(args);
        c
    };
    let mut configure = vec![
        "cmake".into(),
        "-S".into(),
        "/src".into(),
        "-B".into(),
        "/work/psp".into(),
        "-DCMAKE_TOOLCHAIN_FILE=/src/cmake/psp.cmake".into(),
    ];
    configure.extend(job.game_defines(Path::new("/work/gen/game.c")));
    configure.extend(job.opts.cmake_args.iter().cloned());
    run(&mut docker(configure), "CMake configure (PSP, Docker)")?;
    let mut build = vec!["cmake".into(), "--build".into(), "/work/psp".into()];
    build.extend(job.parallel_args());
    run(&mut docker(build), "CMake build (PSP, Docker)")
}

fn mount(host: &Path, container: &str) -> OsString {
    let mut m = host.as_os_str().to_owned();
    m.push(":");
    m.push(container);
    m
}

/// "uid:gid" of the work folder, so files the container writes stay the user's.
#[cfg(unix)]
fn owner_of(dir: &Path) -> Option<String> {
    use std::os::unix::fs::MetadataExt;
    std::fs::metadata(dir)
        .ok()
        .map(|m| format!("{}:{}", m.uid(), m.gid()))
}

#[cfg(not(unix))]
fn owner_of(_dir: &Path) -> Option<String> {
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn opts(target: BuildTarget) -> BuildOptions {
        BuildOptions {
            input: PathBuf::from(file!()),
            target,
            output: PathBuf::from("out/x"),
            config: None,
            title: Some("Sky <Hop>".into()),
            id: None,
            tolerance: None,
            source: Some(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..")),
            work: None,
            jobs: Some(3),
            cmake_args: vec![],
            android: AndroidOptions::default(),
        }
    }

    #[test]
    fn job_resolves_names_and_paths() {
        let job = Job::new(opts(BuildTarget::Psp)).unwrap();
        assert_eq!(job.title, "Sky <Hop>");
        assert_eq!(job.name, "Sky-Hop");
        assert_eq!(job.id, "Sky-Hop");
        assert!(job.work.to_string_lossy().ends_with("x.work"));
        assert!(job.opts.output.is_absolute());
        let d = job.game_defines(Path::new("/w/game.c"));
        assert!(d.contains(&"-DANYSWF_GAME_NAME=Sky-Hop".to_string()));
        assert!(d.contains(&"-DANYSWF_GAME_C=/w/game.c".to_string()));
        assert_eq!(job.parallel_args(), ["--parallel", "3"]);
        let plist = info_plist(&job);
        assert!(plist.contains("<string>Sky &lt;Hop&gt;</string>"));
        assert!(plist.contains("<string>io.anyport.anyswf.skyhop</string>"));
    }

    #[test]
    fn job_rejects_bad_input() {
        let mut o = opts(BuildTarget::Psp);
        o.id = Some("bad id".into());
        assert!(Job::new(o).is_err());
        let mut o = opts(BuildTarget::Psp);
        o.input = PathBuf::from("missing.swf");
        assert!(Job::new(o).is_err());
        let mut o = opts(BuildTarget::Psp);
        o.source = Some(PathBuf::from("/"));
        assert!(Job::new(o).is_err());
    }

    #[test]
    fn desktop_targets_need_their_host() {
        let foreign = if cfg!(windows) {
            BuildTarget::Linux
        } else {
            BuildTarget::Windows
        };
        assert!(Job::new(opts(foreign)).is_err());
    }
}
