# vapoursynth-composite

PAL and NTSC composite-video encoding and restoration for VapourSynth, written
in Rust on top of [`vapoursynth-rs`](https://github.com/rust-av/vapoursynth-rs).

The Rust implementation is deliberately split into two layers:

- a host-independent signal core with explicit raster, phase, level, and plane
  contracts;
- a narrow VapourSynth adapter that owns frame requests and format validation.

The current Rust milestone implements exact colour-frame phase generation,
fixed-point PAL/NTSC modulation, a one-line notch reference decoder, and an
adaptive same-field spatial comb. The earlier C implementation's temporal
Transform separator, trained LUTs, confidence masks, refinement loop, and
arbitrary-width internal resampling are not yet part of the Rust API. The
plugin rejects unsupported modes instead of silently substituting a weaker
algorithm.

## Build

Install current stable Rust (the crate's MSRV is 1.85), then run:

```sh
cargo build --release
```

The plugin is written to `target/release` as `libvapoursynth_composite.so` on
Linux, `libvapoursynth_composite.dylib` on macOS, or
`vapoursynth_composite.dll` on Windows. Rename the Unix library to
`composite.so`/`composite.dylib` when installing it in VapourSynth's plugin
directory.

The crate does not link against VapourSynth. The host supplies the API table
when it loads the plugin, which keeps cross-platform builds reproducible.

## Input contract

This milestone accepts the composite raster directly:

| Standard | Input/output width | Height | Format |
|---|---:|---:|---|
| PAL | 928 | 576 | `YUV444P16` for `Encode`/`Restore`, `GRAY16` for `Decode` |
| NTSC | 758 | 480 or 486 | `YUV444P16` for `Encode`/`Restore`, `GRAY16` for `Decode` |

For NTSC 480-line input, `_FieldBased=2` selects the top-field-first raster
placement. Missing, progressive, or bottom-field-first metadata uses the
480-line DV placement.

Use VapourSynth's resize plugin explicitly before and after the filter when
working on ordinary BT.601 widths. Making the resampling geometry explicit is
preferable to hiding a second filter graph while the Rust resampler is still
under differential validation.

## Filters

```python
import vapoursynth as vs

core = vs.core

# `clip` is already YUV444P16 on the 4fsc raster.
composite = core.composite.Encode(
    clip,
    standard="ntsc",
    setup=0,
    precomb=0,
)

decoded = core.composite.Decode(
    composite,
    standard="ntsc",
    setup=0,
    dimensions=2,
)

restored = core.composite.Restore(
    clip,
    standard="ntsc",
    setup=0,
    precomb=0,
    dimensions=2,
)
```

`dimensions=1` selects the notch reference decoder. `dimensions=2` selects the
adaptive spatial comb and is the default. Other values are rejected until the
temporal implementation reaches parity with the previous measured corpus.

## Development contract

The quick gate is:

```sh
cargo fmt --all --check
cargo check --all-targets
cargo clippy --all-targets -- -D warnings
cargo test --all-targets
cargo test --all-targets --release
```

CI runs the build on Linux, Windows, and macOS. Performance claims require a
release benchmark against the preserved C baseline on the same machine; no
speedup is claimed before that comparison exists.

## License

GPL-3.0-or-later. See `COPYING`.
