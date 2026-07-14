#!/usr/bin/env python3
# Plugin shell: registration, argument validation, passthrough stubs.
# usage: test_composite.py path/to/composite.so

import sys

import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(sys.argv[1])

src = core.std.BlankClip(format=vs.YUV420P8, width=720, height=576, length=5,
                         fpsnum=25, fpsden=1, color=[81, 90, 240])

for fn in (core.composite.Encode, core.composite.Decode):
    for kwargs in ({}, {'standard': 'pal'}, {'standard': 'ntsc'}):
        out = fn(src, **kwargs)
        assert out.num_frames == src.num_frames
        assert (out.format.id, out.width, out.height) == (src.format.id, src.width, src.height)
        # passthrough stub: output frames identical to source
        for n in range(out.num_frames):
            a, b = out.get_frame(n), src.get_frame(n)
            for p in range(a.format.num_planes):
                assert bytes(a[p]) == bytes(b[p])

for fn, name in ((core.composite.Encode, 'Encode'), (core.composite.Decode, 'Decode')):
    try:
        fn(src, standard='secam')
    except vs.Error as e:
        assert name in str(e) and 'standard' in str(e)
    else:
        assert False, 'invalid standard accepted'

print('test_composite: all tests passed')
