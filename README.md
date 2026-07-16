# vapoursynth-composite

Composite PAL/NTSC encoding and decoding for VapourSynth.

Round-tripping component Y'CbCr through a composite encode and a good
decode removes cross-luma and cross-color artifacts baked in by a bad
hardware decoder: the re-encode reconstructs the composite signal the
bad decoder saw, and a Transform/comb decode re-separates it properly.
The signal processing follows
[ld-decode](https://github.com/happycube/ld-decode)'s ld-chroma-encoder
and ld-chroma-decoder, after Clarke, *Colour encoding and decoding
techniques for line-locked sampled PAL and NTSC television signals*,
BBC RD 1986/2, with separation modes from GB 2365247 A (Easterbrook)
and US 7,872,689 (Weston), and decoder adaptivity after Faroudja
(*NTSC and Beyond*, 1988).

## Usage

### Encode

```python
comp = core.composite.Encode(clip, standard="pal", setup=0, precomb=0)
```

Y'CbCr (any constant YUV format) to composite GRAY16 on the 4×fsc
active raster: PAL 928×576 (levels per EBU Tech 3280), NTSC
758×480/486 (levels per SMPTE 244M). Subcarrier phase is an exact
integer function of frame and line, so a later `Decode` demodulates
with the same sequence.

- `standard` — `"pal"` (default) or `"ntsc"`.
- `setup` — NTSC only: add the 7.5 IRE pedestal (default off).
- `precomb` — vertically low-pass U/V across same-field lines before
  modulation (Poynton's precombing). Useful when encoding clean
  sources for comb decoders; leave it off in the noise-reduction round
  trip, where it measurably hurts.

A 480-line NTSC clip occupies the 486-line BFF raster according to its
`_FieldBased` frame prop: BFF/DV content at rows 4..483, TFF/RP 202 at
rows 5..484. 486-line input is used as-is.

### Decode

```python
out = core.composite.Decode(comp, standard="pal", width=720, dimensions=3,
                            setup=0, level=1, eq=2)
```

Composite GRAY16 back to YUV444P16, resampled to `width`. PAL uses
Transform PAL chroma separation with PALcolour-style demodulation;
NTSC uses an adaptive line comb, a Transform NTSC separation, or a
motion-routed hybrid of the two via `transform`. The defaults shown
are the measured best general configuration; unset parameters adapt
to the chosen path (picking `dimensions=1` or an NTSC comb quietly
drops `level`/`eq` back to their supported values, and an explicit
`threshold`, `thresholds`, or `lut` selects its own separation mode).

- `width` — output width (default 720); the resample inverts Encode's
  BT.601↔4×fsc mapping exactly.
- `dimensions` — `3` (default): spatio-temporal separation (3D
  Transform PAL / adaptive 3D comb) drawing on neighboring frames;
  clip-edge frames lean on black temporal padding, and it costs
  several times 2D's compute. `2`: the fast 2D separation (spatial
  tiles / line comb). `1`: a deliberately crude notch decoder — a
  worst-case reference, and `Restore`'s degradation model.
- `transform` — NTSC with `dimensions=3` only. `1` replaces the 3D
  comb with a Transform NTSC separation (stronger on motion). `2`
  (default there) runs both and routes per sample on a
  chroma-transparent motion detector: still neighborhoods take the
  comb (near-exact on static content), moving ones take the
  transform. `0`: the plain adaptive comb.
- `threshold` — the transform's bin-symmetry ratio (0.4 is the
  ld-decode reference value); higher demands more symmetry to call a
  bin chroma. Passing it selects threshold mode (`level=0`).
- `thresholds` — per-bin override of the symmetry test (80 values for
  `dimensions=2`, 768 for 3). For Transform NTSC the values feed the
  shaped-threshold exponent instead. Calibrated sets trained on the
  VQEG corpora ship in `test/`: `thresholds_pal_2d.txt` (beats any
  uniform threshold), `thresholds_pal_3d.txt` (experimental),
  `thresholds_ntsc.txt`.
- `level` — amplitude limiting instead of the keep/discard test
  (GB 2365247 A's preferred embodiment): each bin pair's larger
  magnitude is reduced to the smaller, phase preserved. Measurably
  better on moving content and real footage; slightly softer on
  static synthetic detail. Default on wherever a transform separation
  is present and no `threshold`/`thresholds`/`lut` was given.
- `lut` — trained soft separation (after US 7,872,689): per frequency
  bin, a gain over the pair-symmetry ratio, 16 knots per bin (1280
  values for `dimensions=2`, 12288 for 3; transform separations only).
  `test/calibrate_thresholds.py` derives tables from a clean corpus in
  closed form; trained sets ship as `test/lut_pal_{2d,3d}.txt` and
  `test/lut_ntsc.txt`. Strongest on natural content; the untrained
  modes are safer on synthetic extremes.
- `eq` — chroma equalization of the known encode+decode filter
  cascade. `0` off; `1` the fixed inverse, +1.7 dB chroma PSNR on
  color detail but it amplifies separation leak on near-monochrome
  content; `2` (default on transform paths) steers total chroma
  bandwidth per sample by the transform's own pair-symmetry
  confidence, from a sub-nominal low-pass where the kept chroma is
  suspect up to the full boosted inverse where it is confirmed — the
  robust choice for unknown or mixed content. Paths without a
  transform default to `1`.
- `evidence` — the low-frequency luma prior of US 7,872,689 (PAL
  transforms, default 0 = off): pairs whose baseband difference
  frequency has no LF luma partner are attenuated by
  `e/(e + evidence*b)`. Around 0.5–1 it cuts residual chroma flicker
  on artifact-heavy footage with flat and quiet regions untouched, but
  can soften saturated color edges whose luma is flat.
- `setup` — must match the encode.

### Restore

```python
out = core.composite.Restore(clip, standard="pal", refine=1, ...)
```

The whole noise-reduction round trip in one call: resample to the
raster, re-encode, decode, resample back. Takes every `Decode`
parameter plus:

- `refine` — analysis-by-synthesis iterations (default 1): a Y-only
  Landweber loop deconvolves a crude-decoder model against the input,
  recovering luma detail the original decoder attenuated; chroma is
  untouched. Higher values help static detailed content; the gain
  scales with how closely the source's decoder resembled a notch.
- `precomb` — passed to the internal encoder (see `Encode`).

## Choosing a decoder mode

Measured on the supervised harness (`test/metrics.py`), VQEG held-out
clips, and real footage; artifact numbers are chroma HF energy and
temporal flicker in the worst-artifact regions:

- The defaults are the measured best general configuration
  (`dimensions=3, level=1, eq=2`, NTSC additionally `transform=2`) —
  on the reference clip they remove ~42% of hot-spot chroma HF and
  ~50% of flicker versus doing nothing, where a plain 2D threshold
  decode manages 25%/20%. Add `evidence=1.0` for artifact-heavy
  material.
- `dimensions=2` is the fast path, and the right one for stills or
  very short clips (the 3D window spans ±3 frames).
- `lut` with the shipped trained tables wins on natural content
  (best-in-class artifact scores on every held-out VQEG clip) but can
  overreach on synthetic extremes such as zone plates.
- NTSC: the default hybrid is never the worst; `transform=0` (comb)
  is near-exact on fully static content, `transform=1` strongest on
  motion.

## Recipes

### Motion-compensated chroma cleanup

The residual the spectral separators cannot touch — an
amplitude-modulated pattern symmetric about the carrier *is* chroma —
is phase-incoherent along motion trajectories: rainbows rotate in hue
frame to frame while real chroma stays put. A motion-compensated
chroma degrain after the decode cancels exactly that. With
[vapoursynth-mvutensils](https://pypi.org/project/vapoursynth-mvutensils/):

```python
dec = core.composite.Restore(clip)
sup = core.mvu.Super(dec, blksize=16, overlap=8, pel=2)
vec = core.mvu.AnalyseMany(sup, radius=2)
out = core.mvu.Degrain(dec, sup, vec, planes=[1, 2], thsad=[400, 1600])
```

Vectors come from the already-clean decoded luma; `planes=[1, 2]`
touches chroma only; the SAD limit falls back to the unprocessed pixel
where vectors fail. Measured on the reference clip this takes hot-spot
flicker from −50% to −63% and chroma HF to −46%, with quiet and flat
regions untouched — and on full-reference tests it *improves* moving
chroma fidelity (V +0.8 dB), because trajectory averaging also cancels
residual separation noise. Strength saturates around
`thsad=[400, 1600]`, `radius=2`; the pre-decode placement is inferior
(the round trip must see the artifacts untouched).

### Clean test composites

```python
comp = core.composite.Encode(clip, standard="ntsc", precomb=1)
```

`precomb=1` nulls line-alternating chroma exactly, the friendly choice
when the consumer is a comb decoder. Keep it off for noise reduction.

## Building

```sh
meson setup build
ninja -C build
meson test -C build
```

Requires Meson, a C99 compiler, FFTW3 (single precision), and
VapourSynth (V4 API) with the Python module available for header
discovery.

End-to-end plugin tests:

```sh
python test/test_composite.py build/composite.so
```

## License

GPL-3.0-or-later. See COPYING.
