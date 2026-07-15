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

Alpha. The PAL round trip works:

- `composite.Encode(clip[, standard])` — YCbCr (any constant YUV format,
  576 lines) to composite GRAY16 at the 4×fsc active raster (928×576),
  levels per EBU Tech 3280 (black 0x4000, white 0xD300).
- `composite.Decode(clip[, standard, width, threshold])` — composite back
  to YUV444P16 via 2D Transform PAL chroma separation and PALcolour-style
  demodulation, resampled to `width` (default 720).

NTSC is not implemented yet.

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
