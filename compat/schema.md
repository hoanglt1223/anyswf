# Compatibility data

One TOML file per movie in `compat/`. `tools/compat-site/generate.py` renders them into
`progress.html`, `compat.json` and SVG badges, and `.github/workflows/pages.yml` checks and
publishes the page. The format is AnyGBA's (same stages, same `title`, `kind`, `stage`,
`evidence`, `blockers`, `updated` fields); the rest is AnySWF-specific.

```toml
title = "Sky Hop"
kind = "game"                         # game | homebrew | test-suite
source = "https://example.com/skyhop" # where the movie comes from (a page, never a file in this repo)
swf_sha256 = "3b1f..."                # optional, games: identifies the tested file (64 lower-case hex)
swf = "tests/swf-src/Functions.as"    # homebrew and test suites only: the source in this repo
swf_version = 8                       # SWF header version
actionscript = "AS2"                  # none | AS1 | AS2
stage = "playable"                    # see below
targets = ["pc", "psp-ppsspp"]        # where it was tried: pc, psp-ppsspp, psp, android-emulator, android
psp_fps = 30                          # optional: measured on a PSP-3000 (not PPSSPP)
evidence = "played to the end on PC and in PPSSPP; 30 fps on hardware in the first level"
blockers = ["sound is silent: Sound.attachSound streams not implemented"]
unimplemented = ["Sound.attachSound", "TextField.setTextFormat"]   # from the runtime's unimplemented-call log
notes = "optional free text"
updated = 2026-10-25
```

Third-party games (`kind = "game"`) are never added to the repository and never named by a
`swf` path: an entry describes a file the tester owns. See `CONTRIBUTING.md`.

## Stages

| Stage | Meaning |
|---|---|
| `fails` | Stops with an error before the first frame, or never shows a picture |
| `boots` | Shows its first frames, but stops with an error or shows wrong output before the title screen or main content |
| `menus` | Title screen and menus work; the game itself does not |
| `in-game` | The game runs without errors; not checked against Flash Player or Ruffle |
| `playable` | Can be played start to finish, and pictures and sound match Flash Player or Ruffle where checked |
| `perfect` | No known difference from Flash Player: all of the movie's own checks pass (test suites), or pictures and sound match throughout |

A movie only moves up a stage with the evidence recorded in `evidence`. The PSP is the
product: a game is not `playable` on the PSP until a `psp` run (real hardware) says so.
