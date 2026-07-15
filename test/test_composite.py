#!/usr/bin/env python3
# Plugin: registration, Encode()/Decode() PAL behavior, the round trip,
# argument validation.
# usage: test_composite.py path/to/composite.so

import sys

import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(sys.argv[1])


def flat(color, fmt=vs.YUV444P16, w=720, h=576, length=1):
    return core.std.BlankClip(format=fmt, width=w, height=h, length=length,
                              fpsnum=25, fpsden=1, color=color)


# ---- Encode: geometry, format, rate
out = core.composite.Encode(flat([126, 128, 128], fmt=vs.YUV420P8))
assert (out.width, out.height) == (928, 576)
assert out.format.id == vs.GRAY16
assert (out.fps.numerator, out.fps.denominator) == (25, 1)

# ---- Encode: flat mid-gray is exact and chroma-free
# Y16 = 4096 + 73*384 = 32128 -> level 0x4000 + 49*384 = 35200
out = core.composite.Encode(flat([32128, 32768, 32768]))
fr = out.get_frame(0)
for r in (0, 1, 100, 575):
    row = [fr[0][r, x] for x in range(928)]
    assert set(row) == {35200}, (r, set(row))

# ---- Encode: color modulates a subcarrier with the expected structure
col = core.composite.Encode(flat([32128, 40960, 28672], length=8))
frames = [col.get_frame(n) for n in range(8)]
row0 = [frames[0][0][0, x] for x in range(928)]
assert max(row0) - min(row0) > 8000, "expected chroma amplitude"
# samples two apart are half a subcarrier cycle apart: they sum to 2*luma
# (away from the edges, where the chroma filter tapers into the padding)
for x in range(6, 920):
    assert abs(row0[x] + row0[x + 2] - 2 * 35200) <= 2, (x, row0[x], row0[x + 2])
# the 8-field sequence repeats after 4 frames and not before
assert bytes(frames[4][0]) == bytes(frames[0][0])
assert bytes(frames[1][0]) != bytes(frames[0][0])
assert bytes(frames[2][0]) != bytes(frames[0][0])

# ---- Encode: argument validation
for bad_args, needle in (
    (dict(clip=flat([126, 128, 128], fmt=vs.YUV420P8), standard='secam'), 'standard'),
    (dict(clip=flat([126, 128, 128], fmt=vs.YUV420P8), standard='ntsc'), 'not implemented'),
    (dict(clip=flat([126, 128, 128], fmt=vs.YUV420P8, h=480)), '576'),
    (dict(clip=core.std.BlankClip(format=vs.GRAY8, width=720, height=576)), 'YUV'),
):
    try:
        core.composite.Encode(**bad_args)
    except vs.Error as e:
        assert 'Encode' in str(e) and needle in str(e), (needle, str(e))
    else:
        assert False, f'expected error: {needle}'

# ---- Decode: geometry and format invert Encode
enc = core.composite.Encode(flat([32128, 40960, 28672], length=5))
dec = core.composite.Decode(enc)
assert (dec.width, dec.height) == (720, 576)
assert dec.format.id == vs.YUV444P16
assert (dec.fps.numerator, dec.fps.denominator) == (25, 1)
assert core.composite.Decode(enc, width=928).width == 928

# ---- Round trip: flat color recovered within tolerance in the interior
src_val = (32128, 40960, 28672)
fr = dec.get_frame(2)
for p, want in enumerate(src_val):
    vals = [fr[p][r, x] for r in (40, 288, 535) for x in range(64, 656, 8)]
    worst = max(abs(v - want) for v in vals)
    assert worst <= 64, (p, want, worst)

# ---- Decode: argument validation
gray = core.std.BlankClip(format=vs.GRAY16, width=928, height=576, length=1)
for bad_args, needle in (
    (dict(clip=gray, standard='secam'), 'standard'),
    (dict(clip=gray, standard='ntsc'), 'not implemented'),
    (dict(clip=gray, threshold=0.0), 'threshold'),
    (dict(clip=gray, width=4), 'width'),
    (dict(clip=core.std.BlankClip(format=vs.GRAY16, width=720, height=576)), '928x576'),
    (dict(clip=flat([126, 128, 128], fmt=vs.YUV420P8)), 'GRAY16'),
):
    try:
        core.composite.Decode(**bad_args)
    except vs.Error as e:
        assert 'Decode' in str(e) and needle in str(e), (needle, str(e))
    else:
        assert False, f'expected error: {needle}'

print('test_composite: all tests passed')
