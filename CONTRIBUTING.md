# Contributing

This repository ships one product: the Rust VapourSynth plugin and its
host-independent PAL/NTSC signal core. Keep that boundary explicit; do not
reintroduce a second host frontend or a second build system.

## Repository layout

- `src/` — Rust library and VapourSynth adapter. Numerical code must remain
  independent of host ABI details.
- `tests/` — Rust integration tests and the optional VapourSynth host smoke
  test.
- `benches/` — release-mode performance measurements for the Rust core.
- `data/luts/` — reviewed calibration inputs.
- `data/thresholds/` — reviewed calibration outputs used by the quality
  workflow.
- `data/lut_tables.inc` — generated build input owned by
  `tools/gen_lut_tables.py`; it is parsed by `build.rs`, not compiled.
- `tools/` — maintained corpus, calibration, and reporting tools. Their
  disposable outputs belong under `build/calibration/`.

## Required checks

```sh
cargo fmt --all --check
cargo check --all-targets
cargo clippy --all-targets -- -D warnings
cargo test --lib --tests
cargo test --lib --tests --release
cargo build --release
```

Run `cargo bench --bench core` for performance changes. Run
`python tests/vapoursynth_plugin.py <plugin>` when the VapourSynth adapter or
its public filter contract changes.

## Generated and local state

Cargo writes build state to `target/`. Calibration commands write temporary
histograms and candidate tables to `build/calibration/` (override with
`CALIBRATION_OUTPUT_DIR`). Both locations are disposable, ignored, and must
not be committed. Regenerate `data/lut_tables.inc` with:

```sh
python tools/gen_lut_tables.py
```

The generated file must remain deterministic and match the reviewed text
tables in `data/luts/`.
