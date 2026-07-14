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

Pre-alpha. `composite.Encode()` and `composite.Decode()` are registered
but are passthrough stubs; the signal processing is under development.

## Building

    meson setup build
    ninja -C build

Requires Meson, a C99 compiler, and VapourSynth (V4 API) with the
Python module available for header discovery.

## Testing

    python test/test_composite.py build/composite.so

## License

GPL-3.0-or-later. See COPYING.
