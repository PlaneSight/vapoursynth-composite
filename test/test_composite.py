#!/usr/bin/env python3
# Plugin: registration, Encode() PAL behaviour, Decode() passthrough stub,
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

# ---- Decode: still a passthrough stub
src = flat([126, 128, 128], fmt=vs.YUV420P8, length=3)
for kwargs in ({}, {'standard': 'pal'}, {'standard': 'ntsc'}):
    out = core.composite.Decode(src, **kwargs)
    assert out.num_frames == src.num_frames
    assert (out.format.id, out.width, out.height) == (src.format.id, src.width, src.height)
    for n in range(out.num_frames):
        a, b = out.get_frame(n), src.get_frame(n)
        for p in range(a.format.num_planes):
            assert bytes(a[p]) == bytes(b[p])

try:
    core.composite.Decode(src, standard='secam')
except vs.Error as e:
    assert 'Decode' in str(e) and 'standard' in str(e)
else:
    assert False, 'invalid standard accepted'

print('test_composite: all tests passed')
