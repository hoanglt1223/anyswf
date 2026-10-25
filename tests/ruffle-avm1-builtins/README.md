# Ruffle AVM1 tests for the runtime built-ins

Each directory here is copied unmodified from Ruffle's test suite
(`tests/tests/swfs/avm1/<name>/` at Ruffle commit
`8b25621fc7096de700830d4764bfd1f7585c73d5`): `test.swf`, the Flash Player trace in
`output.txt`, `test.toml`, and the `.fla` source where Ruffle has one.

They are every test whose name matches the built-ins the runtime implements (MovieClip
methods, buttons, Key, Mouse, Sound, SharedObject, TextField, Date, Color, Stage,
timers) and that `tests/avm1/run-avm1-tests.py` statically selects, except the
`edittext_*` tests, which check rich-text layout and HTML formatting the runtime does
not have. They are kept apart from `tests/ruffle-avm1` so the pass rate of that set
stays comparable; this set runs with its own floor (`--min-ruffle 50`, see
`.github/workflows/ci.yml`) and its failures are the runtime's known gaps.

Ruffle is dual licensed MIT OR Apache-2.0; see [LICENSE.md](LICENSE.md). Copyright (c)
2018-2022 Ruffle LLC and Ruffle contributors.
