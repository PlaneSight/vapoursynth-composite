#!/usr/bin/env python3
# The measured tables in the README's "Choosing a decoder mode" section.
# Measures the NR round trip against clean sources over synthetics (color
# bars, zone plate) and real corpora, per standard.
#
# VQEG rows use ONLY the held-out split (last 3 clips of the dir), the
# same split calibrate_thresholds.py trains on, so the default
# (trained-table) numbers are not scored on training data. The NTSC
# tables were retrained on VQEG+BT.802 combined (commit 3cf6748), so the
# BT.802 rows are the tuned-on corpus, not independent; they are split
# into stills (scenes 1-13) and motion (14+).
#
# Corpus locations override via env: COLORBARS_SO, VQEG_DIR, BT802_DIR.
#
# usage: readme_tables.py composite.so [pal|ntsc|both] [frames]

import glob
import os
import sys

import numpy as np
import vapoursynth as vs

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import metrics
from metrics import (clip_from, to_array, bad_decode, raster_to_601, score,
                     make_sources, W601)

core = vs.core
core.std.LoadPlugin(sys.argv[1])

# proper SMPTE EG-1 / EBU color bars from vapoursynth-colorbars, at 601
# 720xH; replicated to NFRAMES and scaled 12->16 bit.
if not hasattr(core, 'colorbars'):
    _CB = os.environ.get('COLORBARS_SO')
    if _CB:
        core.std.LoadPlugin(_CB)
    assert hasattr(core, 'colorbars'), \
        'colorbars plugin not loaded; set COLORBARS_SO to colorbars.so'


def colorbars(standard, h, nframes):
    res = 1 if standard == 'pal' else 0   # 601 PAL / NTSC, active = full line
    c = core.colorbars.ColorBars(resolution=res, format=vs.YUV444P12,
                                 compatability=2)
    c = core.resize.Point(c, format=vs.YUV444P16)   # 12->16 bit, 720xHfull
    # NTSC mode 0/1 is 486 lines; take the top h (480/486)
    if c.height != h:
        c = core.std.CropAbs(c, width=W601, height=h, left=0, top=0)
    c = c * nframes
    return to_array(c)
STANDARDS = ['pal', 'ntsc'] if len(sys.argv) < 3 or sys.argv[2] == 'both' else [sys.argv[2]]
FRAMES = int(sys.argv[3]) if len(sys.argv) > 3 else 9

VQEG = os.environ.get('VQEG_DIR', os.path.expanduser('~/test-videos/VQEG-422'))
BT802 = os.environ.get('BT802_DIR', '/mnt/x')

# The configurations the README table compares, per standard. The
# "default" row is the plugin's real shipping defaults (empty kwargs):
# eq=2, built-in trained tables, NTSC transform=2.
PAL_CONFIGS = [
    ('default (3D)',      dict()),
    ('trained 2D',        dict(dimensions=2)),
    ('level 2D',          dict(dimensions=2, level=1, eq=1)),
    ('threshold 2D',      dict(dimensions=2, threshold=0.4, eq=1)),
    ('trained 3D +ev',    dict(evidence=1.0)),
]
NTSC_CONFIGS = [
    ('default (3D hybrid)', dict()),
    ('comb 3D',             dict(dimensions=3, transform=0)),
    ('transform 3D',        dict(dimensions=3, transform=1)),
    ('level 3D transform',  dict(dimensions=3, transform=1, level=1, eq=1)),
    ('trained 2D comb',     dict(dimensions=2)),
]


def geom(path):
    # (rows, row_off, tff) from the y4m header
    with open(path, 'rb') as f:
        hdr = f.readline().decode('ascii', 'replace')
    h = int([t for t in hdr.split() if t.startswith('H')][0][1:])
    tff = ' It' in hdr
    if h == 486 or h == 576:
        return h, 0, tff
    assert h == 480, (path, h)
    return h, (5 if tff else 4), tff


def load_frames(path, count, h):
    import subprocess
    # consecutive frames (3D and flicker both need them)
    vf = 'select=gte(n\\,80),format=yuv444p16le'
    r = subprocess.run([metrics.FFMPEG or 'ffmpeg', '-v', 'error', '-i', path,
                        '-vf', vf, '-frames:v', str(count), '-f', 'rawvideo', '-'],
                       capture_output=True)
    a = np.frombuffer(r.stdout, np.uint16)
    n = a.size // (3 * h * W601)
    return a[:n * 3 * h * W601].reshape(n, 3, h, W601).copy()


def encode(arr, standard, tff):
    c = clip_from(arr)
    if tff:
        c = core.std.SetFrameProps(c, _FieldBased=2)
    return core.composite.Encode(c, standard=standard)


def measure(comp, clean, standard, configs):
    # returns {config_name: score dict}; comp is the re-encoded bad decode
    out = {}
    for name, kw in configs:
        nr = to_array(core.composite.Decode(comp, standard=standard, **kw))
        out[name] = score(nr, clean)
    return out


def run_clip(clean, standard, tff, configs):
    # clean: (n,3,h,W601) uint16 -> bad decode -> re-encode -> measure
    rows = clean.shape[2]
    comp = to_array(encode(clean, standard, tff))
    # bad decoder needs the raster row offset; recompute from tff/height
    if standard == 'pal':
        roff = 0
    else:
        roff = (5 if tff else 4) if rows == 480 else 0
    degraded = to_array(raster_to_601(bad_decode(comp, standard, roff), standard, rows))
    recomp = encode(degraded, standard, tff)
    res = {'degraded': score(degraded, clean)}
    res.update(measure(recomp, clean, standard, configs))
    # transparency ceiling: encode->decode clean (default decode)
    trans = to_array(core.composite.Decode(encode(clean, standard, tff),
                                           standard=standard))
    res['transparency'] = score(trans, clean)
    return res


def aggregate(per_clip):
    # mean each metric across clips
    keys = per_clip[0].keys()
    agg = {}
    for cfg in keys:
        metnames = per_clip[0][cfg].keys()
        agg[cfg] = {m: float(np.mean([c[cfg][m] for c in per_clip]))
                    for m in metnames if not isinstance(per_clip[0][cfg][m], dict)}
    return agg


def report(title, agg, configs):
    print(f'\n### {title}')
    hdr = f"  {'config':22s} {'PSNR Y':>7} {'U':>6} {'V':>6}  {'chromaHF':>9} {'flicker':>8}"
    print(hdr)
    order = ['degraded'] + [n for n, _ in configs] + ['transparency']
    base = agg['degraded']
    for name in order:
        r = agg[name]
        chf, fl = r['chf'], r['flick']
        extra = ''
        if name not in ('degraded', 'transparency'):
            extra = f"   chf {(1-chf/base['chf'])*100:+5.0f}%  fl {(1-fl/base['flick'])*100:+5.0f}%"
        print(f"  {name:22s} {r['psnr_Y']:7.2f} {r['psnr_U']:6.2f} {r['psnr_V']:6.2f}  "
              f"{chf:9.1f} {fl:8.1f}{extra}")


def synthetic_rows(standard, configs):
    h = 576 if standard == 'pal' else 480
    srcs = make_sources(h)
    bars = colorbars(standard, h, srcs['zone'].shape[0])
    report(f'{standard.upper()} synthetic: bars (SMPTE/EBU)',
           run_clip(bars, standard, False, configs), configs)
    report(f'{standard.upper()} synthetic: zone',
           run_clip(srcs['zone'], standard, False, configs), configs)


def corpus_rows(title, paths, standard, configs):
    per = []
    for p in paths:
        rows, row_off, tff = geom(p)
        clean = load_frames(p, FRAMES, rows)
        if clean.shape[0] < FRAMES:
            print(f'  skip {os.path.basename(p)} (short: {clean.shape[0]})', file=sys.stderr)
            continue
        per.append(run_clip(clean, standard, tff, configs))
        print(f'  done {os.path.basename(p)}', file=sys.stderr)
    if per:
        report(title, aggregate(per), configs)


def main():
    for standard in STANDARDS:
        configs = PAL_CONFIGS if standard == 'pal' else NTSC_CONFIGS
        synthetic_rows(standard, configs)
        if standard == 'pal':
            clips = sorted(glob.glob(os.path.join(VQEG, '*625*.y4m')))[-3:]
            corpus_rows('PAL VQEG (held-out)', clips, 'pal', configs)
        else:
            clips = sorted(glob.glob(os.path.join(VQEG, '*525*.y4m')))[-3:]
            corpus_rows('NTSC VQEG (held-out)', clips, 'ntsc', configs)
            # BT.802 scenes 1-13 are the STILLS class (single frame held);
            # 14+ are motion. Split the means: stills flatter every
            # temporal metric, so the motion mean is the real-footage
            # number.
            bt = sorted(glob.glob(os.path.join(BT802, 'itur525_*_original.y4m')))
            def scene_no(p):
                return int(os.path.basename(p).split('_')[1][:2])
            stills = [p for p in bt if scene_no(p) <= 13]
            motion = [p for p in bt if scene_no(p) > 13]
            corpus_rows('NTSC BT.802 stills (scenes 1-13)', stills, 'ntsc', configs)
            corpus_rows('NTSC BT.802 motion (scenes 14+)', motion, 'ntsc', configs)


if __name__ == '__main__':
    main()
