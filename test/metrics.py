#!/usr/bin/env python3
# Quality-metrics harness.
#
# Backbone: clean source -> simulated bad decoder (crude 1D notch) ->
# NR round trip -> score against the clean original. Round-trip-vs-input
# comparisons cannot measure artifact removal, so every full-reference
# score here is against the clean source.
#
# Metrics: per-plane PSNR (regression/engineering), chroma horizontal HF
# energy and temporal chroma flicker (the only numbers that see dot
# patterns and crawl). XPSNR (ffmpeg >= 7.1) and SSIMULACRA2 (vszip) are
# used when available.
#
# usage: metrics.py path/to/composite.so [pal|ntsc|both] [frames]

import os
import subprocess
import sys
import tempfile

import numpy as np
import vapoursynth as vs

core = vs.core
NFRAMES = 12

W601 = 720

# ---------------------------------------------------------------- helpers

def geometry(standard):
    # width, height at 601 and raster; row offset (480-line NTSC is BFF here)
    if standard == 'pal':
        return 576, 928, 0
    return 480, 758, 4


def clip_from(arr):
    n, _, h, w = arr.shape
    base = core.std.BlankClip(format=vs.YUV444P16, width=w, height=h, length=n,
                              fpsnum=25, fpsden=1)

    def fill(n, f):
        fout = f.copy()
        for p in range(3):
            np.asarray(fout[p])[:] = arr[n, p]
        return fout

    return core.std.ModifyFrame(base, base, fill)


def gray_clip_from(arr):
    n, h, w = arr.shape
    base = core.std.BlankClip(format=vs.GRAY16, width=w, height=h, length=n,
                              fpsnum=25, fpsden=1)

    def fill(n, f):
        fout = f.copy()
        np.asarray(fout[0])[:] = arr[n]
        return fout

    return core.std.ModifyFrame(base, base, fill)


def to_array(clip):
    planes = clip.format.num_planes
    out = np.empty((clip.num_frames, planes, clip.height, clip.width), np.uint16)
    for n in range(clip.num_frames):
        f = clip.get_frame(n)
        for p in range(planes):
            out[n, p] = np.asarray(f[p])
    return out if planes > 1 else out[:, 0]

# ---------------------------------------------------------------- sources

def make_sources(h):
    yy, xx = np.mgrid[0:h, 0:W601].astype(np.float64)
    srcs = {}

    # 75% color bars (chroma fidelity, flat regions)
    bars = np.array([[46080, 32768, 32768], [42322, 21587, 34701],
                     [34700, 39812, 22015], [30942, 28632, 23948],
                     [19234, 36904, 41588], [15476, 25724, 43521],
                     [7854, 43949, 30835], [4096, 32768, 32768]], np.float64)
    f = np.empty((3, h, W601))
    idx = (xx * 8 / W601).astype(int)
    for p in range(3):
        f[p] = bars[idx, p]
    srcs['bars'] = np.broadcast_to(f, (NFRAMES, 3, h, W601)).astype(np.uint16)

    # zone plate: luma frequency sweep on gray (pure cross-color bait)
    cx, cy = W601 / 2, h / 2
    r2 = (xx - cx) ** 2 + (yy - cy) ** 2
    z = 32128 + 12000 * np.cos(np.pi * r2 / (2.0 * W601))
    f = np.stack([z, np.full_like(z, 32768), np.full_like(z, 32768)])
    srcs['zone'] = np.broadcast_to(f, (NFRAMES, 3, h, W601)).astype(np.uint16)

    # moving diagonal grating over color blocks (temporal behavior)
    frames = np.empty((NFRAMES, 3, h, W601))
    blocks = bars[(yy * 4 / h).astype(int) % 8]
    for t in range(NFRAMES):
        g = 8000 * np.cos(2 * np.pi * (0.35 * xx + 0.08 * yy + 0.3 * t))
        frames[t, 0] = np.clip(blocks[..., 0] * 0.5 + 16384 + g, 4096, 60160)
        frames[t, 1] = blocks[..., 1]
        frames[t, 2] = blocks[..., 2]
    srcs['motion'] = frames.astype(np.uint16)

    return srcs

# ------------------------------------------------------- simulated bad TV

def subcarrier(standard, frame, row):
    # phase (radians) and V-switch at the first active sample (see
    # src/subcarrier.c; NTSC row is the raster row including the offset)
    if standard == 'pal':
        fl = 44 + row
        fid = (frame % 4) * 2 + (fl & 1)
        prev = (fid // 2) * 625 + (fid % 2) * 313 + fl // 2
        ph = (182 * 625 + (prev % 2500) * 1879) % 2500
        return 2 * np.pi * ph / 2500, (-1.0 if prev & 1 else 1.0)
    fl = 39 + row
    fid = ((frame % 2) * 2 + (fl & 1)) % 4
    prev = (fid // 2) * 525 + (fid % 2) * 263 + fl // 2
    ph = (130 * 180 + 654 + (prev % 720) * 360) % 720
    return 2 * np.pi * ph / 720, 1.0


def bad_decode(comp, standard, row_off):
    # deliberately crude decoder: 1D bandpass chroma separation (full
    # cross-color and dot crawl), boxcar chroma low-pass
    if standard == 'pal':
        black, span, ku, kv = 0x4000, 0x9300, 18752, 26449
    else:
        black, span, ku, kv = 0x3C00, 0x8C00, 17859, 25189

    n, h, w = comp.shape
    c = comp.astype(np.float64)
    est = np.zeros_like(c)
    est[..., 2:-2] = (2 * c[..., 2:-2] - c[..., :-4] - c[..., 4:]) / 4.0

    x = np.arange(w)
    sinx = np.array([0.0, 1.0, 0.0, -1.0])[x % 4]
    cosx = np.array([1.0, 0.0, -1.0, 0.0])[x % 4]
    box = np.ones(9) / 9.0

    out = np.empty((n, 3, h, w))
    for t in range(n):
        for r in range(h):
            th, vsw = subcarrier(standard, t, r + row_off)
            m = np.convolve(est[t, r] * sinx, box, mode='same')
            q = np.convolve(est[t, r] * cosx, box, mode='same')
            u = 2 * (m * np.cos(th) + q * np.sin(th))
            v = vsw * 2 * (q * np.cos(th) - m * np.sin(th))
            out[t, 0, r] = 4096 + (c[t, r] - est[t, r] - black) * (219 * 256) / span
            out[t, 1, r] = 32768 + u * 32768 / ku
            out[t, 2, r] = 32768 + v * 32768 / kv
    return np.clip(out, 0, 65535).astype(np.uint16)


def raster_to_601(arr, standard, h):
    # the exact inverse resample Decode() uses
    rho = 540000.0 / 709379.0 if standard == 'pal' else 33.0 / 35.0
    active0 = 182.0 if standard == 'pal' else 130.0 + 57.0 / 90.0
    anchor = 132.0 if standard == 'pal' else 122.0
    return core.resize.Spline36(clip_from(arr), width=W601, height=h,
                                src_left=anchor / rho - (active0 - 0.5) - 0.5 / rho,
                                src_width=W601 / rho)

# ---------------------------------------------------------------- metrics

def crop(a):
    return a[:, :, 24:-24, 32:-32].astype(np.float64)


def score(out, ref):
    o, r = crop(out), crop(ref)
    res = {}
    if HAVE_XPSNR:
        res['xpsnr'] = xpsnr(out, ref)
    for p, name in enumerate('YUV'):
        mse = np.mean((o[:, p] - r[:, p]) ** 2)
        res[f'psnr_{name}'] = 10 * np.log10(65535.0 ** 2 / mse) if mse else np.inf
    du = o[:, 1] - 32768
    dv = o[:, 2] - 32768
    res['chf'] = (np.abs(np.diff(du, axis=2)).mean() + np.abs(np.diff(dv, axis=2)).mean()) / 2
    res['flick'] = (np.abs(np.diff(du, axis=0)).mean() + np.abs(np.diff(dv, axis=0)).mean()) / 2
    return res


def fmt(name, res):
    s = (f"  {name:16s} PSNR Y {res['psnr_Y']:6.2f}  U {res['psnr_U']:6.2f}  "
         f"V {res['psnr_V']:6.2f}   chromaHF {res['chf']:7.1f}  flicker {res['flick']:7.1f}")
    x = res.get('xpsnr')
    if x:
        s += f"   XPSNR Y {x['y']:6.2f}  U {x['u']:6.2f}  V {x['v']:6.2f}"
    return s

# ------------------------------------------------------------------- main

def find_ffmpeg():
    # prefer an override, then the local newer build, then PATH
    for cand in (os.environ.get('FFMPEG'),
                 os.path.expanduser('~/ffmpeg-hevc/ffmpeg'), 'ffmpeg'):
        if not cand:
            continue
        try:
            r = subprocess.run([cand, '-hide_banner', '-filters'],
                               capture_output=True, text=True)
        except OSError:
            continue
        if ' xpsnr ' in r.stdout:
            return cand
    return None


FFMPEG = find_ffmpeg()
HAVE_XPSNR = FFMPEG is not None



def xpsnr(out, ref):
    n, _, h, w = out.shape
    with tempfile.NamedTemporaryFile(suffix='.raw', delete=False) as fa, \
         tempfile.NamedTemporaryFile(suffix='.raw', delete=False) as fb:
        out.astype('<u2').tofile(fa.name)
        ref.astype('<u2').tofile(fb.name)
        raw = ['-f', 'rawvideo', '-pix_fmt', 'yuv444p16le', '-s', f'{w}x{h}', '-r', '25']
        r = subprocess.run([FFMPEG, '-hide_banner', *raw, '-i', fa.name,
                            *raw, '-i', fb.name,
                            '-lavfi', 'xpsnr', '-f', 'null', '-'],
                           capture_output=True, text=True)
    os.unlink(fa.name)
    os.unlink(fb.name)
    for line in r.stderr.splitlines():
        if 'XPSNR' in line and 'y:' in line:
            parts = line.replace(':', ' ').split()
            vals = {}
            for key in ('y', 'u', 'v'):
                if key in parts:
                    vals[key] = float(parts[parts.index(key) + 1])
            return vals
    return None
HAVE_SSIMU2 = hasattr(core, 'vszip')


def main():
    global NFRAMES
    core.std.LoadPlugin(sys.argv[1])
    standards = [sys.argv[2]] if len(sys.argv) > 2 and sys.argv[2] != 'both' else ['pal', 'ntsc']
    if len(sys.argv) > 3:
        NFRAMES = int(sys.argv[3])
    if not HAVE_XPSNR:
        print('note: ffmpeg xpsnr filter unavailable, skipping XPSNR')
    if not HAVE_SSIMU2:
        print('note: vszip unavailable, skipping SSIMULACRA2')

    for standard in standards:
        h, wr, row_off = geometry(standard)
        print(f'\n=== {standard.upper()} ({W601}x{h}, {NFRAMES} frames) ===')

        for name, clean in make_sources(h).items():
            clean_clip = clip_from(clean)
            comp = to_array(core.composite.Encode(clean_clip, standard=standard))

            degraded = to_array(raster_to_601(bad_decode(comp, standard, row_off), standard, h))
            print(f'{name}:')
            print(fmt('degraded', score(degraded, clean)))

            recomp = core.composite.Encode(clip_from(degraded), standard=standard)
            if standard == 'pal':
                configs = [('NR 2D t=0.4', dict(dimensions=2, threshold=0.4)),
                           ('NR 2D eq=0', dict(dimensions=2, eq=0)),
                           ('NR 2D t=0.7', dict(dimensions=2, threshold=0.7)),
                           ('NR 2D level', dict(dimensions=2, level=1)),
                           ('NR 3D t=0.4', dict(dimensions=3, threshold=0.4)),
                           ('NR 3D t=0.7', dict(dimensions=3, threshold=0.7)),
                           ('NR 3D level', dict(dimensions=3, level=1))]
            else:
                configs = [('NR 2D', dict(dimensions=2)),
                           ('NR 2D eq=0', dict(dimensions=2, eq=0)),
                           ('NR 3D', dict(dimensions=3)),
                           ('NR 3D transform', dict(dimensions=3, transform=1)),
                           ('NR 3D tf level', dict(dimensions=3, transform=1, level=1))]
            for cname, kw in configs:
                nr = to_array(core.composite.Decode(recomp, standard=standard, **kw))
                print(fmt(cname, score(nr, clean)))

            for rn in (1, 2, 4):
                rest = to_array(core.composite.Restore(clip_from(degraded),
                                                       standard=standard, refine=rn))
                print(fmt(f'Restore r={rn}', score(rest, clean)))

            recomp_pc = core.composite.Encode(clip_from(degraded), standard=standard,
                                              precomb=1)
            nr_pc = to_array(core.composite.Decode(recomp_pc, standard=standard))
            print(fmt('NR 2D precomb', score(nr_pc, clean)))

            transparent = to_array(core.composite.Decode(
                core.composite.Encode(clean_clip, standard=standard), standard=standard))
            print(fmt('transparency 2D', score(transparent, clean)))

            if standard == 'pal':
                transparent_lv = to_array(core.composite.Decode(
                    core.composite.Encode(clean_clip, standard=standard),
                    standard=standard, level=1))
                print(fmt('transparency 2D level', score(transparent_lv, clean)))


if __name__ == '__main__':
    main()
