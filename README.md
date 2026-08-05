# vapoursynth-composite

PAL/NTSC composite-video encode, decode, and restoration for VapourSynth.
This branch is a Zig rewrite of the original C signal-processing path and
uses [`vapoursynth-zig`](https://github.com/dnjulek/vapoursynth-zig) as its
typed ZAPI boundary.

The plugin models the composite channel that a poor decoder saw, then
separates that signal again with a deterministic comb or transform path. It
is intended for cross-colour, cross-luma, dot-crawl, and chroma-bandwidth
artifacts in captured PAL and NTSC material.

The Zig branch is VapourSynth-only. AviSynth compatibility and its C ABI are
not part of this implementation; ZAPI is the sole host boundary.

## Quick start

```python
import vapoursynth as vs

core = vs.core
clean = core.composite.Restore(source, standard="ntsc")
clean.set_output()
```

`standard` is `"pal"` or `"ntsc"`. The default `Restore` path is PAL 3D
transform or NTSC 3D hybrid separation, with the built-in trained soft-gain
tables enabled when no explicit threshold mode is selected.

## Public filters

### `Encode`

```python
composite = core.composite.Encode(source, standard="pal", precomb=0)
```

Converts a constant-format YUV clip to the active 4×fsc raster as GRAY16:

| Standard | Raster | Accepted picture height |
| --- | ---: | ---: |
| PAL | 928 × 576 | 576 |
| NTSC | 758 × 480/486 | 480 or 486 |

Input width and YUV format are normalized through the VapourSynth resize
plugin. `setup=1` selects the NTSC 7.5 IRE pedestal. `precomb=1` applies the
encoder's vertical `[1, 2, 1]` chroma prefilter.

### `Decode`

```python
picture = core.composite.Decode(composite, standard="ntsc", width=720)
```

Accepts a GRAY16 composite raster and returns YUV444P16. `width=0` keeps the
raw 4×fsc raster; otherwise the result is resized to the requested width.

Useful controls:

- `dimensions=1`, `2`, or `3`: crude notch, fast 2D, or temporal 3D path.
- `transform=0`, `1`, or `2`: NTSC comb, transform, or motion-routed hybrid.
- `eq=0`, `1`, or `2`: off, fixed, or confidence-weighted chroma equalizer.
- `threshold`, `thresholds`, `level`, and `lut`: explicit separation policy.
- `evidence`: PAL luma-guided chroma prior.
- `cti=1`: luma-guided chroma transient improvement for graphics-like input.
- `mask="motion"` or `"confidence"`: return a second mask clip.

`Decode` adds diagnostic frame properties when the selected path computes
them: `CompositeSeparationConfidenceMean`,
`CompositeSeparationConfidenceStdDev`, `CompositeMotionFraction`,
`CompositeRefineResidual`, and `CompositeRefineCorrection`.

### `Restore`

```python
picture, motion = core.composite.Restore(
    source,
    standard="ntsc",
    mask="motion",
    refine=1,
)
```

`Restore` composes `Encode` and `Decode` in the graph, preserving VapourSynth
frame caching. `refine` adds a bounded Y-only Landweber correction against the
pre-encode luma; set it to `0` for a conservative restoration. The optional
mask is white where the requested condition is strongest: motion for the
NTSC hybrid, or low separation confidence for the confidence mask.

## Architecture

The signal-processing modules are independent of VapourSynth:

| Module | Responsibility |
| --- | --- |
| `encode.zig` | fixed-point YUV-to-carrier modulation |
| `decode.zig` | PAL demodulation, NTSC combs, hybrid routing, equalization, refinement |
| `transform2d.zig` | overlap-tiled 2D FFT separation |
| `transform3d.zig` | overlap-tiled PAL/NTSC temporal FFT separation |
| `fft.zig` | allocation-free radix-2 1D/2D/3D transforms |
| `geometry.zig` | BT.601 ↔ 4×fsc sample anchors |
| `plugin.zig` | the narrow ZAPI/Zig FFI boundary and graph construction |

Frame-request scratch is allocated once per decoder instance and selected
with a bounded atomic pool. Per-line processing uses fixed-width staging
buffers, integer Q15/Q16 arithmetic, and no allocator traffic. The large
transform tiles are stack-local and bounded by the PAL/NTSC constants.

## Build and test

The repository uses Zig 0.16.0 and pins the ZAPI dependency to a commit of
`dnjulek/vapoursynth-zig` in `build.zig.zon`.

```sh
zig build test
zig build check
zig build -Doptimize=ReleaseFast
```

`zig build` installs `zig-out/lib/composite.so` (or the platform equivalent).
Load it with `core.std.LoadPlugin` or place it in the VapourSynth autoload
directory.

The test suite covers the PAL/NTSC configuration matrix, subcarrier and
geometry invariants, FFT round trips, encoder fixed-point behavior, and a
neutral composite decode. The CI workflow runs both the unit tests and the
plugin compilation check.

## License

GPL-3.0-or-later. See [COPYING](COPYING).
