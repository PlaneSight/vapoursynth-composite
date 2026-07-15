# vapoursynth-composite

PAL/NTSC composite video encode/decode for VapourSynth.

Round-tripping component YCbCr through a composite encode and decode
removes cross-luma and cross-colour artifacts baked in by a bad
hardware PAL/NTSC decoder. The signal processing is based on
[ld-decode](https://github.com/happycube/ld-decode)'s ld-chroma-encoder
and ld-chroma-decoder, after Clarke, *Colour encoding and decoding
techniques for line-locked sampled PAL and NTSC television signals*,
BBC RD 1986/2.

## Status

Alpha. PAL and NTSC round trips work:

- `composite.Encode(clip[, standard, setup])` — YCbCr (any constant YUV
  format) to composite GRAY16 at the 4×fsc active raster: PAL 928×576
  (levels per EBU Tech 3280), NTSC 758×480/486 (levels per SMPTE 244M;
  `setup` adds the 7.5 IRE pedestal). A 480-line NTSC clip occupies the
  486-line BFF raster per its `_FieldBased` frame prop: BFF/DV at rows
  4..483, TFF/RP 202 at rows 5..484.
- `composite.Decode(clip[, standard, width, threshold, setup, dimensions, eq])`
  — composite back to YUV444P16, resampled to `width` (default 720). PAL
  uses Transform PAL chroma separation with PALcolour-style demodulation
  (`threshold` is the transform's bin-symmetry ratio); NTSC uses an
  adaptive line comb. `dimensions=3` selects the spatio-temporal
  variants (3D Transform PAL / adaptive 3D comb), which draw on
  neighbouring frames. `eq` (default 1) applies a chroma equalizer that
  inverts the known encode+decode filter cascade, sharpening recovered
  color; disable it for content that is essentially monochrome, where
  it can amplify chroma leakage instead. `dimensions=1` is a
  deliberately crude notch decoder, useful as a worst-case reference and
  as the degradation model of `Restore`. `thresholds` overrides the
  Transform PAL bin-symmetry test per frequency bin (80 values for
  dimensions=2, 768 for 3); test/thresholds_pal_2d.txt is a set
  calibrated on the VQEG 625-line corpus by test/calibrate_thresholds.py,
  and modestly outperforms any uniform threshold for dimensions=2. A 3D
  set (test/thresholds_pal_3d.txt) is provided for experimentation but
  did not consistently beat uniform 0.4 in validation.
- `composite.Restore(clip[, standard, width, threshold, setup, dimensions, eq, refine])`
  — the whole noise-reduction round trip in one call. `refine` (default
  1) runs that many analysis-by-synthesis iterations that deconvolve a
  crude-decoder model against the input picture, recovering luma detail
  the original decoder attenuated; chroma is untouched by the
  refinement. Higher values recover more detail on static, detailed
  content; the effect scales with how closely the footage's original
  decoder resembled a simple notch.

## Building

    meson setup build
    ninja -C build

Requires Meson, a C99 compiler, FFTW3 (single precision), and
VapourSynth (V4 API) with the Python module available for header
discovery.

## Testing

    python test/test_composite.py build/composite.so

## License

GPL-3.0-or-later. See COPYING.
