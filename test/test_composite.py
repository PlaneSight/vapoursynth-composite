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


# ---- NTSC: geometry, both heights, field-order handling
ntsc420 = core.std.BlankClip(format=vs.YUV420P8, width=720, height=480, length=3,
                             fpsnum=30000, fpsden=1001, color=[126, 128, 128])
out = core.composite.Encode(ntsc420, standard='ntsc')
assert (out.width, out.height, out.format.id) == (758, 480, vs.GRAY16)
out486 = core.composite.Encode(
    core.std.BlankClip(format=vs.YUV420P8, width=720, height=486, length=1), standard='ntsc')
assert (out486.width, out486.height) == (758, 486)

# TFF and BFF content occupy different raster rows, so the composites differ
flat480 = core.std.BlankClip(format=vs.YUV444P16, width=720, height=480, length=3,
                             fpsnum=30000, fpsden=1001, color=[32128, 40960, 28672])
bff = core.std.SetFrameProps(flat480, _FieldBased=1)
tff = core.std.SetFrameProps(flat480, _FieldBased=2)
enc_bff = core.composite.Encode(bff, standard='ntsc')
enc_tff = core.composite.Encode(tff, standard='ntsc')
assert bytes(enc_bff.get_frame(0)[0]) != bytes(enc_tff.get_frame(0)[0])

# ---- NTSC round trip, both field orders and with setup
for enc_clip in (enc_bff, enc_tff):
    dec = core.composite.Decode(enc_clip, standard='ntsc')
    assert (dec.width, dec.height, dec.format.id) == (720, 480, vs.YUV444P16)
    fr = dec.get_frame(1)
    for p, want in enumerate((32128, 40960, 28672)):
        vals = [fr[p][r, x] for r in (30, 240, 445) for x in range(64, 656, 8)]
        worst = max(abs(v - want) for v in vals)
        assert worst <= 96, (p, want, worst)

dec = core.composite.Decode(core.composite.Encode(bff, standard='ntsc', setup=1),
                            standard='ntsc', setup=1)
fr = dec.get_frame(1)
for p, want in enumerate((32128, 40960, 28672)):
    vals = [fr[p][r, x] for r in (30, 240, 445) for x in range(64, 656, 8)]
    worst = max(abs(v - want) for v in vals)
    assert worst <= 96, ('setup', p, want, worst)

# ---- NTSC argument validation
for bad_args, needle in (
    (dict(clip=core.std.BlankClip(format=vs.YUV420P8, width=720, height=482),
          standard='ntsc'), '480 or 486'),
):
    try:
        core.composite.Encode(**bad_args)
    except vs.Error as e:
        assert needle in str(e), (needle, str(e))
    else:
        assert False, f'expected error: {needle}'
try:
    core.composite.Decode(core.std.BlankClip(format=vs.GRAY16, width=928, height=576),
                          standard='ntsc')
except vs.Error as e:
    assert '758x480 or 758x486' in str(e)
else:
    assert False, 'wrong ntsc composite size accepted'


# ---- 3D decoders: round trip both standards, static content
pal_src = flat([32128, 40960, 28672], length=9)
dec3 = core.composite.Decode(core.composite.Encode(pal_src), dimensions=3)
fr = dec3.get_frame(4)
for pl, want in enumerate((32128, 40960, 28672)):
    vals = [fr[pl][r, x] for r in (40, 288, 535) for x in range(64, 656, 8)]
    worst = max(abs(v - want) for v in vals)
    assert worst <= 64, ('pal3d', pl, want, worst)

dec3n = core.composite.Decode(core.composite.Encode(bff, standard='ntsc'),
                              standard='ntsc', dimensions=3)
fr = dec3n.get_frame(1)
for pl, want in enumerate((32128, 40960, 28672)):
    vals = [fr[pl][r, x] for r in (30, 240, 445) for x in range(64, 656, 8)]
    worst = max(abs(v - want) for v in vals)
    assert worst <= 96, ('ntsc3d', pl, want, worst)

# ---- eq: on by default; disabling changes chroma on textured content
tex = core.std.StackHorizontal([flat([30000, 40960, 28672], w=360),
                                flat([30000, 24576, 36864], w=360)])
enc_t = core.composite.Encode(tex)
assert bytes(core.composite.Decode(enc_t).get_frame(0)[1]) != \
       bytes(core.composite.Decode(enc_t, eq=0).get_frame(0)[1])

try:
    core.composite.Decode(core.std.BlankClip(format=vs.GRAY16, width=928, height=576),
                          dimensions=4)
except vs.Error as e:
    assert 'dimensions' in str(e)
else:
    assert False, 'dimensions=4 accepted'

# ---- dimensions=1 (crude) decode and Restore() end to end
crude = core.composite.Decode(enc_t, dimensions=1)
assert (crude.width, crude.height, crude.format.id) == (720, 576, vs.YUV444P16)

rest = core.composite.Restore(tex)
assert (rest.width, rest.height, rest.format.id) == (720, 576, vs.YUV444P16)
# refine only touches luma: chroma must match the plain round trip
plain = core.composite.Decode(core.composite.Encode(tex))
assert bytes(rest.get_frame(0)[1]) == bytes(plain.get_frame(0)[1])
assert bytes(rest.get_frame(0)[0]) != bytes(plain.get_frame(0)[0])
# refine=0 Restore is exactly the plain round trip
rest0 = core.composite.Restore(tex, refine=0)
for pl in range(3):
    assert bytes(rest0.get_frame(0)[pl]) == bytes(plain.get_frame(0)[pl])
# NTSC Restore with field-order detection
rest_n = core.composite.Restore(bff, standard='ntsc', dimensions=3)
assert (rest_n.width, rest_n.height) == (720, 480)
rest_n.get_frame(1)
# ---- Transform NTSC 3D: round-trips, param validation
bff9 = core.std.SetFrameProps(
    core.std.BlankClip(format=vs.YUV444P16, width=720, height=480, length=9,
                       fpsnum=30000, fpsden=1001, color=[32128, 40960, 28672]),
    _FieldBased=1)
ntf = core.composite.Decode(core.composite.Encode(bff9, standard='ntsc'),
                            standard='ntsc', dimensions=3, transform=1)
fr = ntf.get_frame(4)
for pl, want in enumerate((32128, 40960, 28672)):
    vals = [fr[pl][r, x] for r in (30, 240, 445) for x in range(64, 656, 8)]
    worst = max(abs(v - want) for v in vals)
    assert worst <= 96, ('ntsc-transform3d', pl, want, worst)
try:
    core.composite.Decode(core.composite.Encode(flat([126, 128, 128], fmt=vs.YUV420P8)),
                          transform=1, dimensions=3)
except vs.Error as e:
    assert 'ntsc' in str(e)
else:
    assert False, 'transform=1 accepted for pal'
try:
    core.composite.Decode(core.composite.Encode(bff, standard='ntsc'),
                          standard='ntsc', transform=1)
except vs.Error as e:
    assert 'dimensions=3' in str(e)
else:
    assert False, 'transform without dimensions=3 accepted'

# ---- precomb: flat chroma unchanged, vertical chroma detail differs
flat_pc = core.composite.Encode(flat([30000, 40960, 28672]))
flat_pc1 = core.composite.Encode(flat([30000, 40960, 28672]), precomb=1)
assert bytes(flat_pc.get_frame(0)[0]) == bytes(flat_pc1.get_frame(0)[0])
vstripes = core.std.StackVertical([flat([30000, 40960, 28672], h=288),
                                   flat([30000, 24576, 36864], h=288)])
assert bytes(core.composite.Encode(vstripes).get_frame(0)[0]) != \
       bytes(core.composite.Encode(vstripes, precomb=1).get_frame(0)[0])
core.composite.Restore(tex, precomb=1).get_frame(0)

# ---- per-bin thresholds: uniform list matches the scalar exactly
tl = core.composite.Decode(enc_t, thresholds=[0.4] * 80)
ts = core.composite.Decode(enc_t, threshold=0.4)
for pl in range(3):
    assert bytes(tl.get_frame(0)[pl]) == bytes(ts.get_frame(0)[pl])
assert bytes(core.composite.Decode(enc_t, thresholds=[0.9] * 80).get_frame(0)[1]) != \
       bytes(ts.get_frame(0)[1])
for bad_kw, needle in ((dict(thresholds=[0.4] * 79), '80'),
                       (dict(thresholds=[0.4] * 80, dimensions=3), '768'),
                       (dict(thresholds=[1.5] * 80), '(0, 1]')):
    try:
        core.composite.Decode(enc_t, **bad_kw)
    except vs.Error as e:
        assert needle in str(e), (needle, str(e))
    else:
        assert False, f'expected error: {needle}'

try:
    core.composite.Restore(tex, refine=99)
except vs.Error as e:
    assert 'refine' in str(e)
else:
    assert False, 'refine=99 accepted'

# ---- level mode: amplitude limiting instead of the threshold test
lv = core.composite.Decode(enc_t, level=1)
fr = lv.get_frame(0)
for pl, want in enumerate((30000, 40960, 28672)):
    vals = [fr[pl][r, x] for r in (100, 288, 475) for x in range(48, 312, 8)]
    worst = max(abs(v - want) for v in vals)
    assert worst <= 96, ('pal2d-level', pl, want, worst)
assert bytes(lv.get_frame(0)[0]) != bytes(ts.get_frame(0)[0])
core.composite.Decode(enc_t, level=1, dimensions=3).get_frame(0)
core.composite.Restore(tex, level=1).get_frame(0)
ntf_lv = core.composite.Decode(core.composite.Encode(bff9, standard='ntsc'),
                               standard='ntsc', dimensions=3, transform=1,
                               level=1)
fr = ntf_lv.get_frame(4)
for pl, want in enumerate((32128, 40960, 28672)):
    vals = [fr[pl][r, x] for r in (30, 240, 445) for x in range(64, 656, 8)]
    worst = max(abs(v - want) for v in vals)
    assert worst <= 96, ('ntsc-transform3d-level', pl, want, worst)
for bad_kw, needle in ((dict(level=1, dimensions=1), 'dimensions 2 or 3'),
                       (dict(level=1, standard='ntsc', dimensions=3), 'transform=1'),
                       (dict(level=1, thresholds=[0.4] * 80), 'level=0')):
    try:
        core.composite.Decode(core.composite.Encode(bff, standard='ntsc')
                              if bad_kw.get('standard') == 'ntsc' else enc_t,
                              **bad_kw)
    except vs.Error as e:
        assert needle in str(e), (needle, str(e))
    else:
        assert False, f'expected error: {needle}'

print('test_composite: all tests passed')
