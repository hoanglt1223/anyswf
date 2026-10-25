//! Per-game build settings (`--config game.toml`) and the names derived from a title.

use std::path::Path;
use std::str::FromStr;

/// Keys `anyswf build` reads. Other top-level keys and tables are left to the runtime's
/// per-game settings and ignored here.
#[derive(Debug, Default, Clone, PartialEq)]
pub struct GameConfig {
    pub title: Option<String>,
    pub id: Option<String>,
    pub version: Option<String>,
    pub tolerance: Option<f32>,
    pub android: AndroidConfig,
}

#[derive(Debug, Default, Clone, PartialEq)]
pub struct AndroidConfig {
    pub package: Option<String>,
    pub version_code: Option<u32>,
    pub touch_keys: Vec<String>,
}

fn string(v: &toml::Value, key: &str) -> Result<String, String> {
    v.as_str()
        .map(str::to_owned)
        .ok_or_else(|| format!("'{key}' must be a string"))
}

impl GameConfig {
    pub fn parse(text: &str) -> Result<GameConfig, String> {
        let table = toml::Table::from_str(text).map_err(|e| e.to_string())?;
        let mut c = GameConfig::default();
        for (key, v) in &table {
            match key.as_str() {
                "title" => c.title = Some(string(v, key)?),
                "id" => c.id = Some(string(v, key)?),
                "version" => c.version = Some(string(v, key)?),
                "tolerance" => {
                    let t = v
                        .as_float()
                        .or_else(|| v.as_integer().map(|i| i as f64))
                        .map(|f| f as f32)
                        .filter(|t| t.is_finite() && *t > 0.0 && *t <= 10.0)
                        .ok_or("'tolerance' must be a number in (0, 10]")?;
                    c.tolerance = Some(t);
                }
                "android" => {
                    let t = v.as_table().ok_or("'android' must be a table")?;
                    for (k, v) in t {
                        match k.as_str() {
                            "package" => c.android.package = Some(string(v, "android.package")?),
                            "version_code" => {
                                let n = v
                                    .as_integer()
                                    .filter(|n| (1..=2_100_000_000).contains(n))
                                    .ok_or("'android.version_code' must be 1..2100000000")?;
                                c.android.version_code = Some(n as u32);
                            }
                            "touch_keys" => {
                                let arr = v
                                    .as_array()
                                    .ok_or("'android.touch_keys' must be an array of key names")?;
                                c.android.touch_keys = arr
                                    .iter()
                                    .map(|k| string(k, "android.touch_keys"))
                                    .collect::<Result<_, _>>()?;
                            }
                            other => return Err(format!("unknown key 'android.{other}'")),
                        }
                    }
                }
                _ => {}
            }
        }
        Ok(c)
    }

    pub fn load(path: &Path) -> Result<GameConfig, String> {
        let text = std::fs::read_to_string(path)
            .map_err(|e| format!("cannot read {}: {e}", path.display()))?;
        GameConfig::parse(&text).map_err(|e| format!("{}: {e}", path.display()))
    }
}

/// File and target name for a title: letters, digits, '-' and '_' (spaces become '-'),
/// at most 32 characters, "game" if nothing is left. Also a valid runtime save id.
pub fn file_name(title: &str) -> String {
    let mut out = String::new();
    for ch in title.chars() {
        if ch.is_ascii_alphanumeric() || ch == '_' || ch == '-' {
            out.push(ch);
        } else if (ch.is_whitespace() || ch == '.') && !out.is_empty() && !out.ends_with('-') {
            out.push('-');
        }
        if out.len() == 32 {
            break;
        }
    }
    let out = out.trim_end_matches('-').to_owned();
    if out.is_empty() { "game".into() } else { out }
}

/// A save id from --id or the config: the same characters the runtime accepts.
pub fn valid_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 32
        && id
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
}

/// Java package naming: two or more dot-separated segments, each starting with a letter.
pub fn valid_package(id: &str) -> bool {
    let parts: Vec<&str> = id.split('.').collect();
    parts.len() >= 2
        && id.len() <= 200
        && parts.iter().all(|p| {
            p.chars().next().is_some_and(|c| c.is_ascii_alphabetic())
                && p.chars().all(|c| c.is_ascii_alphanumeric() || c == '_')
        })
}

/// `io.anyport.anyswf.<id>` with the id reduced to a valid segment.
pub fn default_package(id: &str) -> String {
    let mut seg: String = id
        .chars()
        .filter(|c| c.is_ascii_alphanumeric() || *c == '_')
        .map(|c| c.to_ascii_lowercase())
        .collect();
    if !seg.starts_with(|c: char| c.is_ascii_alphabetic()) {
        seg.insert(0, 'g');
    }
    format!("io.anyport.anyswf.{seg}")
}

/// Key names the player's --touch-keys accepts. Mirrors plat_key_from_name() in
/// platform/common/plat_common.c, so a bad name fails the build, not the game.
pub fn valid_key_name(name: &str) -> bool {
    let n = name.to_ascii_lowercase();
    let b = n.as_bytes();
    if b.len() == 1 {
        return b[0].is_ascii_alphanumeric();
    }
    if let Some(k) = n
        .strip_prefix('f')
        .filter(|f| !f.starts_with('0'))
        .and_then(|f| f.parse::<u32>().ok())
    {
        return (1..=12).contains(&k);
    }
    matches!(
        n.as_str(),
        "backspace"
            | "tab"
            | "enter"
            | "return"
            | "shift"
            | "control"
            | "ctrl"
            | "alt"
            | "escape"
            | "esc"
            | "space"
            | "pageup"
            | "pagedown"
            | "end"
            | "home"
            | "left"
            | "up"
            | "right"
            | "down"
            | "insert"
            | "delete"
    )
}

pub fn xml_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            '\'' => out.push_str("&apos;"),
            c if (c as u32) < 0x20 && c != '\t' && c != '\n' => {}
            c => out.push(c),
        }
    }
    out
}

/// Android resource strings also treat '@' and '?' at the start, and '\' anywhere, specially.
pub fn android_label(s: &str) -> String {
    let mut out = xml_escape(s).replace('\\', "\\\\");
    if out.starts_with('@') || out.starts_with('?') {
        out.insert(0, '\\');
    }
    out
}

/// Replaces each `@KEY@`; fails if a placeholder is left.
pub fn fill_template(text: &str, values: &[(&str, &str)]) -> Result<String, String> {
    let mut out = text.to_owned();
    for (k, v) in values {
        out = out.replace(&format!("@{k}@"), v);
    }
    let bytes = out.as_bytes();
    for (i, w) in bytes.windows(2).enumerate() {
        if w[0] == b'@' && w[1].is_ascii_uppercase() {
            let rest = &out[i + 1..];
            if let Some(end) = rest.find('@') {
                let name = &rest[..end];
                if name.chars().all(|c| c.is_ascii_uppercase() || c == '_') {
                    return Err(format!("template placeholder @{name}@ has no value"));
                }
            }
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_known_keys_and_ignores_runtime_tables() {
        let c = GameConfig::parse(
            r#"
title = "Sky Hop"
id = "sky-hop"
tolerance = 0.25
[android]
package = "com.example.skyhop"
version_code = 7
touch_keys = ["left", "right", "space"]
[keys]
cross = "space"
"#,
        )
        .unwrap();
        assert_eq!(c.title.as_deref(), Some("Sky Hop"));
        assert_eq!(c.id.as_deref(), Some("sky-hop"));
        assert_eq!(c.tolerance, Some(0.25));
        assert_eq!(c.android.package.as_deref(), Some("com.example.skyhop"));
        assert_eq!(c.android.version_code, Some(7));
        assert_eq!(c.android.touch_keys, ["left", "right", "space"]);
    }

    #[test]
    fn rejects_bad_values() {
        assert!(GameConfig::parse("title = 3").is_err());
        assert!(GameConfig::parse("tolerance = 50").is_err());
        assert!(GameConfig::parse("[android]\nversion_code = 0").is_err());
        assert!(GameConfig::parse("[android]\ntouch_keys = \"left\"").is_err());
        assert!(GameConfig::parse("[android]\npackge = \"a.b\"").is_err());
        assert!(GameConfig::parse("title = ").is_err());
    }

    #[test]
    fn names() {
        assert_eq!(file_name("Sky Hop: Deluxe!"), "Sky-Hop-Deluxe");
        assert_eq!(file_name("  "), "game");
        assert_eq!(file_name("a.b c"), "a-b-c");
        assert_eq!(file_name(&"x".repeat(40)).len(), 32);
        assert!(valid_id("Sky-Hop_2") && !valid_id("a b") && !valid_id(""));
        assert_eq!(default_package("Sky-Hop"), "io.anyport.anyswf.skyhop");
        assert_eq!(default_package("2048"), "io.anyport.anyswf.g2048");
        assert!(valid_package("io.anyport.anyswf.g2048"));
        assert!(!valid_package("single") && !valid_package("a..b") && !valid_package("a.1b"));
    }

    #[test]
    fn key_names_match_the_runtime() {
        for k in [
            "a", "Z", "7", "LEFT", "space", "Enter", "ctrl", "f1", "F12", "esc",
        ] {
            assert!(valid_key_name(k), "{k}");
        }
        for k in ["", "f0", "f13", "f01", "lefty", "!!", "fx"] {
            assert!(!valid_key_name(k), "{k}");
        }
    }

    #[test]
    fn escaping_and_templates() {
        assert_eq!(xml_escape("a<b & \"c\""), "a&lt;b &amp; &quot;c&quot;");
        assert_eq!(android_label("@home\\"), "\\@home\\\\");
        assert_eq!(
            fill_template("<a b=\"@X@\"/>", &[("X", "1")]).unwrap(),
            "<a b=\"1\"/>"
        );
        assert!(fill_template("@X@ @MISSING@", &[("X", "1")]).is_err());
        assert!(fill_template("mail@example.com", &[]).is_ok());
    }
}
