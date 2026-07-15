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
- `composite.Decode(clip[, standard, width, threshold, setup])` —
  composite back to YUV444P16, resampled to `width` (default 720). PAL
  uses 2D Transform PAL chroma separation with PALcolour-style
  demodulation (`threshold` is the transform's bin-symmetry ratio);
  NTSC uses a 2D adaptive line comb.

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
