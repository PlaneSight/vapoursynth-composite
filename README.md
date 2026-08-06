# vapoursynth-composite

PAL and NTSC composite-video encoding and restoration for VapourSynth. The
production implementation is an Edition 2024 Rust `cdylib` built on
[`vapoursynth-rs`](https://github.com/rust-av/vapoursynth-rs).

The project deliberately separates a host-independent signal core from a
small VapourSynth adapter. The core owns signal geometry, modulation,
spectral separation, calibrated gain tables, and reusable workspaces; the
adapter owns format conversion, temporal frame requests, and frame
properties. That keeps the hot pixel paths allocation-free and confines host
ABI work to one documented module.

## Capabilities

- PAL and NTSC fixed-point composite encoding, including colour-frame phase
  sequencing and NTSC setup.
- `dimensions=1` notch reference, `dimensions=2` spatial separation, and
  `dimensions=3` calibrated temporal spectral separation.
- Built-in PAL 2D/3D and NTSC 3D gain tables, generated into the Rust build
  from the retained calibration source; the C implementation is never
  compiled or linked by Cargo.
- NTSC temporal comb, transform, and motion-routed hybrid policies.
- Fixed and leak-aware chroma equalization, luma-guided CTI, confidence and
  motion diagnostics, and iterative `Restore` luma refinement.
- BT.601-to-4fsc and 4fsc-to-BT.601 horizontal Spline36 geometry, including
  edge splicing for restored output at ordinary widths.

The legacy C sources and their calibration/test corpus remain in the tree as
a parity oracle. Cargo is the canonical build and CI contract. No unmeasured
speedup or bit-exact cross-implementation claim is made.

## Build and install

Install Rust 1.88 or newer, then build:

```sh
cargo build --release
```

The plugin artifact is `libvapoursynth_composite.so` on Linux,
`libvapoursynth_composite.dylib` on macOS, and `vapoursynth_composite.dll` on
Windows. Install it in VapourSynth's plugin directory (rename the Unix
artifact to `composite.so` or `composite.dylib` if required by that layout).

The plugin dynamically receives VapourSynth's API table from the host; it does
not link the host library. `Encode` and `Restore` use VapourSynth's standard
`com.vapoursynth.resize` plugin for their input conversion, so that plugin must
be available at script construction time.

## Input and output contracts

| Filter | Input | Output |
|---|---|---|
| `Encode` | Constant-dimension YUV at any ordinary width/format | 4fsc `GRAY16` composite raster |
| `Decode` | 4fsc `GRAY16` composite raster | `YUV444P16`, 720 pixels wide by default |
| `Restore` | Constant-dimension YUV at any ordinary width/format | `YUV444P16` at the input width by default |

Raw widths are 928 for PAL and 758 for NTSC. `width=0` requests raw 4fsc
output from `Decode` or `Restore`; otherwise `Decode` defaults to 720 and
`Restore` defaults to its input width. PAL uses 576 lines. NTSC accepts the
standard 480- or 486-line rasters; `_FieldBased=2` selects top-field-first
placement for 480 lines, with the DV placement used otherwise.

## VapourSynth use

```python
import vapoursynth as vs

core = vs.core

# `clip` can be an ordinary constant-dimension YUV source.
composite = core.composite.Encode(clip, standard="ntsc", setup=0)

# Three-dimensional NTSC hybrid separation is the default.
decoded = core.composite.Decode(
    composite,
    standard="ntsc",
    dimensions=3,
    transform=2,
    eq=2,
    mask="motion",
)

# A requested diagnostic mask is attached to the decoded frame.
motion = core.std.PropToClip(decoded, prop="CompositeMask")

# Restore encodes, separates, optionally refines luma, then returns BT.601.
restored = core.composite.Restore(
    clip,
    standard="ntsc",
    dimensions=3,
    transform=2,
    refine=1,
)
```

`mask="motion"` is available for NTSC `dimensions=3, transform=2`.
`mask="confidence"` is available with leak-aware equalization (`eq=2`).
`vapoursynth-rs`'s safe filter constructor has one clip output, so the selected
mask is carried as the `CompositeMask` frame property and can be extracted
with `PropToClip`. The picture frame also carries
`CompositeSeparationConfidenceMean`, `CompositeSeparationConfidenceStdDev`,
`CompositeMotionFraction`, and, for `Restore`, `CompositeRefineResidual` and
`CompositeRefineCorrection` when applicable.

## Decode controls

| Control | Values | Meaning |
|---|---|---|
| `dimensions` | `1`, `2`, `3` | Notch, spatial, or temporal separation; default `3` |
| `transform` | `0`, `1`, `2` | NTSC comb, transform, or hybrid; default `2` for temporal NTSC |
| `eq` | `0`, `1`, `2` | Off, fixed, or leak-aware chroma equalization |
| `threshold` / `thresholds` | scalar / per-bin | Hard spectral-separation thresholds |
| `lut` | 0–1 gain values | Custom trained gain table; mutually exclusive with thresholds and `level=1` |
| `level` | `0`, `1` | Threshold or amplitude-limiting separation mode |
| `evidence` | non-negative float | PAL low-frequency luma-evidence prior |
| `cti` | `0`, `1` | Luma-guided chroma transient improvement |
| `refine` | `0`–`16` | `Restore` luma-refinement iterations; default `1` |

The built-in calibrated tables are selected automatically when no custom
separation control is supplied. Their expected custom lengths are 1,280 for
PAL 2D, 6,144 for PAL 3D, and 12,288 for NTSC 3D; a supplied table is checked
at filter construction time.

## Development contract

```sh
cargo fmt --all --check
cargo check --all-targets
cargo clippy --all-targets -- -D warnings
cargo test --lib --tests
cargo test --lib --tests --release
cargo build --release
cargo bench --bench core
```

CI compiles every Cargo target and executes the library and integration tests
on Linux, Windows, and macOS. The standalone benchmark target is run on Linux
in release mode; it is intentionally kept out of the ordinary test command so
benchmark execution cannot be mistaken for a correctness check. Performance
work must use release-mode measurements against the retained C reference on
the same machine and corpus.

## License

GPL-3.0-or-later. See `COPYING`.
