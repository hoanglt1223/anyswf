//! Per-target conversion settings: the logical screen a movie is laid out on and how much
//! detail meshes, gradients and bitmaps keep for the display it ends up on.

use crate::convert::{Options, SCREEN_H, SCREEN_W};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Target {
    /// PSP: the stage is fitted into the 480x272 screen ("show all") and assets are
    /// reduced to that size.
    Psp,
    /// Windows, Linux, macOS: the logical screen is the original stage. Windows can be
    /// scaled up, so curves and gradients keep detail for twice the stage size.
    Desktop,
    /// Android: the original stage, shown full screen on high-density displays.
    Android,
}

impl Target {
    pub fn parse(name: &str) -> Option<Target> {
        match name {
            "psp" => Some(Target::Psp),
            "pc" | "desktop" | "windows" | "linux" | "macos" => Some(Target::Desktop),
            "android" => Some(Target::Android),
            _ => None,
        }
    }

    /// Conversion options; `tolerance_px` overrides the default curve flattening error.
    pub fn options(self, tolerance_px: Option<f32>) -> Options {
        let base = Options::default();
        let (screen, detail) = match self {
            Target::Psp => (Some((SCREEN_W as u16, SCREEN_H as u16)), 1.0),
            Target::Desktop => (None, 2.0),
            Target::Android => (None, 3.0),
        };
        Options {
            tolerance_px: tolerance_px.unwrap_or(base.tolerance_px),
            screen,
            detail,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names() {
        assert_eq!(Target::parse("psp"), Some(Target::Psp));
        assert_eq!(Target::parse("linux"), Some(Target::Desktop));
        assert_eq!(Target::parse("windows"), Some(Target::Desktop));
        assert_eq!(Target::parse("android"), Some(Target::Android));
        assert_eq!(Target::parse("ios"), None);
    }

    #[test]
    fn psp_matches_the_default_options() {
        let d = Options::default();
        let p = Target::Psp.options(None);
        assert_eq!(
            (p.tolerance_px, p.screen, p.detail),
            (d.tolerance_px, d.screen, d.detail)
        );
        assert_eq!(p.screen, Some((480, 272)));
    }

    #[test]
    fn pc_targets_keep_the_stage_and_add_detail() {
        let o = Target::Desktop.options(Some(0.5));
        assert_eq!(o.screen, None);
        assert_eq!(o.tolerance_px, 0.5);
        assert!(o.detail > 1.0);
        assert!(Target::Android.options(None).detail >= o.detail);
    }
}
