#!/usr/bin/env python3
# Self-calibrate the Transform PAL 2D per-bin thresholds from a corpus.
#
# Ground truth comes from linearity of the encoder: encoding luma-only
# and chroma-only versions of a clean clip labels every FFT bin's energy
# as luma or chroma. The bin-symmetry ratio each bin would be judged by
# is measured on the composite the decoder actually sees (the re-encoded
# bad-decode of the clean clip). The optimal threshold per bin then
# minimizes kept-luma (cross-color) plus alpha * discarded-chroma
# (color loss) by a scan over ratio histograms.
#
# The same histograms also yield the trained soft LUT (Decode's lut
# parameter): per (bin, ratio cell), the gain minimizing
# L*g^2 + alpha*C*(1-g)^2 is the Wiener gain alpha*C / (L + alpha*C) —
# closed form, no iteration.
#
# usage: calibrate_thresholds.py composite.so corpus_dirs [frames_per_clip] [2d|3d|ntsc]
# 2d/3d calibrate Transform PAL on the 625-line corpus; ntsc calibrates
# Transform NTSC 3D on the 525-line corpus (per-bin t0 values for the
# shaped threshold, by inverting the shaping, plus the soft LUT).
# corpus_dirs is a colon-separated directory list; the last 3 clips of
# each directory are held out for validation. Clips may mix 486-line
# (full raster, assumed BFF) and 480-line material; 480-line clips are
# placed on the raster per their y4m interlacing flag (TFF at row 5,
# else row 4), matching Encode's _FieldBased handling.

import glob
import os
import subprocess
import sys

import numpy as np
import vapoursynth as vs

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import metrics
from metrics import clip_from, to_array, bad_decode, raster_to_601, score, W601

core = vs.core
core.std.LoadPlugin(sys.argv[1])
CORPUS = sys.argv[2]
FRAMES = int(sys.argv[3]) if len(sys.argv) > 3 else 6
MODE = sys.argv[4] if len(sys.argv) > 4 else '2d' 

XTILE, YTILE = 32, 16                       # 2D: x samples, y field lines
X3, Y3, Z3 = 16, 32, 8                      # 3D: x samples, y frame lines, z fields
BIN_X = list(range(XTILE // 8, XTILE // 4 + 1))
BIN_X3 = list(range(X3 // 8, X3 // 4 + 1))
NBINS = (YTILE * len(BIN_X)) if MODE == '2d' else (Z3 * Y3 * len(BIN_X3))
ALPHA = 2.0
RBUCKETS = 200
STANDARD = 'ntsc' if MODE == 'ntsc' else 'pal'
H, WR = (486, 758) if MODE == 'ntsc' else (576, 928)
# 3D reflection offsets; the raster parity of even (temporally first)
# fields depends on each clip's raster placement for NTSC
ZOFF, YOFF = (Z3 // 2, Y3 // 2) if MODE == 'ntsc' else (Z3 // 4, Y3 // 4)
PARITY = 0  # PAL; NTSC clips carry their own parity in geom()


def geom(path):
    # per-clip geometry from the y4m header: (rows, row_off, parity, tff)
    with open(path, 'rb') as f:
        hdr = f.readline().decode('ascii', 'replace')
    h = int([t for t in hdr.split() if t.startswith('H')][0][1:])
    tff = ' It' in hdr
    if STANDARD == 'pal' or h == 486:
        return h, 0, (0 if STANDARD == 'pal' else 1), False
    assert h == 480, (path, h)
    row_off = 5 if tff else 4
    return h, row_off, (row_off + 1) & 1, tff


def gray_clip(arr, tff=False):
    n, h, w = arr.shape
    base = core.std.BlankClip(format=vs.GRAY16, width=w, height=h, length=n,
                              fpsnum=25, fpsden=1)

    def fill(n, f):
        fout = f.copy()
        np.asarray(fout[0])[:] = arr[n]
        return fout

    c = core.std.ModifyFrame(base, base, fill)
    return core.std.SetFrameProps(c, _FieldBased=2) if tff else c


def load_frames(path, count, h):
    # 2D can sample scattered frames; 3D needs consecutive fields
    vf = ('select=not(mod(n\\,40)),' if MODE == '2d' else 'select=gte(n\\,80),') \
         + 'format=yuv444p16le'
    r = subprocess.run([metrics.FFMPEG or 'ffmpeg', '-v', 'error', '-i', path,
                        '-vf', vf, '-frames:v', str(count), '-f', 'rawvideo', '-'],
                       capture_output=True)
    a = np.frombuffer(r.stdout, np.uint16)
    n = a.size // (3 * h * W601)
    return a[:n * 3 * h * W601].reshape(n, 3, h, W601).copy()


def encode(arr, tff=False):
    c = clip_from(arr)
    if tff:
        c = core.std.SetFrameProps(c, _FieldBased=2)
    return to_array(core.composite.Encode(c, standard=STANDARD))


def tile_stats(comp_nr, comp_lum, comp_chr, hist_lum, hist_chr):
    # windowed tile FFTs of each field; accumulate per-bin histograms of
    # the observed symmetry ratio, weighted by true luma/chroma energy
    wy = 0.5 - 0.5 * np.cos(2 * np.pi * (np.arange(YTILE) + 0.5) / YTILE)
    wx = 0.5 - 0.5 * np.cos(2 * np.pi * (np.arange(XTILE) + 0.5) / XTILE)
    win = np.outer(wy, wx)

    ys = [(y, ((YTILE // 2) + YTILE - y) % YTILE) for y in range(YTILE)]

    for t in range(comp_nr.shape[0]):
        for field in range(2):
            planes = [c[t, field::2].astype(np.float64) for c in (comp_nr, comp_lum, comp_chr)]
            rows = planes[0].shape[0]
            for ty in range(0, rows - YTILE + 1, YTILE // 2):
                for tx in range(0, WR - XTILE + 1, XTILE // 2):
                    specs = [np.fft.rfft2(p[ty:ty + YTILE, tx:tx + XTILE] * win)
                             for p in planes]
                    pw = [np.abs(s) ** 2 for s in specs]
                    b = 0
                    for y, yref in ys:
                        for x in BIN_X:
                            xref = XTILE // 2 - x
                            if x == xref and y == yref:
                                b += 1
                                continue
                            pin, pref = pw[0][y, x], pw[0][yref, xref]
                            hi = max(pin, pref)
                            r = (min(pin, pref) / hi) if hi > 0 else 1.0
                            bucket = min(int(r * RBUCKETS), RBUCKETS - 1)
                            hist_lum[b, bucket] += pw[1][y, x] + pw[1][yref, xref]
                            hist_chr[b, bucket] += pw[2][y, x] + pw[2][yref, xref]
                            b += 1


def tile_stats_3d(comp_nr, comp_lum, comp_chr, hist_lum, hist_chr,
                  rows=None, parity=PARITY):
    if rows is None:
        rows = H
    # 3D tiles in frame-line space: rows of the other field are black,
    # exactly as the 3D transform builds them
    def window(nn):
        return 0.5 - 0.5 * np.cos(2 * np.pi * (np.arange(nn) + 0.5) / nn)

    win = window(Z3)[:, None, None] * window(Y3)[None, :, None] * window(X3)[None, None, :]
    refs = [(z, y, x,
             (ZOFF + Z3 - z) % Z3, (YOFF + Y3 - y) % Y3, X3 // 2 - x)
            for z in range(Z3) for y in range(Y3) for x in BIN_X3]

    nfields = comp_nr.shape[0] * 2

    def field(c, g):
        return c[g // 2, g % 2::2].astype(np.float64)

    def tile(c, tz, ty, tx):
        t = np.full((Z3, Y3, X3), 16384.0)
        for z in range(Z3):
            g = tz + z
            if g < 0 or g >= nfields:
                continue
            f = field(c, g)
            for y in range(Y3):
                fl = ty + y
                if 0 <= fl < rows and ((fl + parity) & 1) == (g & 1):
                    t[z, y] = f[fl >> 1, tx:tx + X3]
        return t * win

    for tz in range(0, nfields - Z3 + 1, Z3 // 2):
        for ty in range(0, rows - Y3 + 1, Y3):
            for tx in range(16, WR - X3 - 16, X3 * 2):
                pw = [np.abs(np.fft.rfftn(tile(c, tz, ty, tx))) ** 2
                      for c in (comp_nr, comp_lum, comp_chr)]
                for b, (z, y, x, zr, yr, xr) in enumerate(refs):
                    if z == zr and y == yr and x == xr:
                        continue
                    pin, pref = pw[0][z, y, x], pw[0][zr, yr, xr]
                    hi = max(pin, pref)
                    r = (min(pin, pref) / hi) if hi > 0 else 1.0
                    bucket = min(int(r * RBUCKETS), RBUCKETS - 1)
                    hist_lum[b, bucket] += pw[1][z, y, x] + pw[1][zr, yr, xr]
                    hist_chr[b, bucket] += pw[2][z, y, x] + pw[2][zr, yr, xr]


LUT_K = 16

def wiener_lut(hist_lum, hist_chr, alpha):
    # resample the fine ratio buckets onto LUT_K uniform knots and take
    # the per-cell Wiener gain; cells with no observed energy inherit
    # the nearest observed knot
    lut = np.empty((NBINS, LUT_K))
    centers = (np.arange(RBUCKETS) + 0.5) / RBUCKETS
    knot = np.clip(np.round(centers * (LUT_K - 1)).astype(int), 0, LUT_K - 1)
    for b in range(NBINS):
        L = np.bincount(knot, weights=hist_lum[b], minlength=LUT_K)
        C = np.bincount(knot, weights=hist_chr[b], minlength=LUT_K)
        seen = (L + C) > 0
        g = np.where(seen, alpha * C / np.maximum(L + alpha * C, 1e-30), 0.0)
        if seen.any():
            idx = np.arange(LUT_K)
            nearest = idx[seen][np.abs(idx[seen][None, :] - idx[:, None]).argmin(axis=1)]
            g = g[nearest]
        else:
            g[:] = 1.0
        lut[b] = g
    return np.clip(lut, 0.0, 1.0)


def ntsc_shaped_bases():
    # base = k_chroma/(k_luma+k_chroma) per bin, with the runtime's
    # interlace diamond mapping; the shaped threshold is base**(10*t0^2)
    bases = []
    for z in range(Z3):
        kz0 = z / Z3
        for y in range(Y3):
            ky0 = y / Y3
            ky, kz = ky0, kz0
            if kz0 + ky0 < 0.5:
                kz, ky = kz0 + 0.5, ky0 + 0.5
            elif kz0 + ky0 > 1.5:
                kz, ky = kz0 - 0.5, ky0 - 0.5
            elif kz0 - ky0 > 0.5:
                kz, ky = kz0 - 0.5, ky0 + 0.5
            elif ky0 - kz0 > 0.5:
                kz, ky = kz0 + 0.5, ky0 - 0.5
            if kz + ky > 1.0:
                kz, ky = 1.0 - kz, 1.0 - ky
            for x in BIN_X3:
                kx = x / X3
                k_luma = (kz - 0.5) ** 2 + (ky - 0.5) ** 2 + kx ** 2
                k_chroma = (kz - 0.25) ** 2 + (ky - 0.25) ** 2 + (kx - 0.25) ** 2
                bases.append(k_chroma / (k_luma + k_chroma))
    return np.array(bases)


def optimal_thresholds(hist_lum, hist_chr, alpha):
    # keep iff r >= cut: cost(cut) = kept luma + alpha * discarded chroma.
    # PAL: cut = t^2, report t. NTSC: cut = base**(10*t0^2), report t0
    # (the luma-evidence tightening is ignored here).
    th = np.full(NBINS, 0.4)
    bases = ntsc_shaped_bases() if MODE == 'ntsc' else None
    for b in range(NBINS):
        lum, chr_ = hist_lum[b], hist_chr[b]
        # cost when the cut is placed before bucket k (keep buckets >= k)
        kept_lum = np.concatenate(([lum.sum()], lum.sum() - np.cumsum(lum)))
        lost_chr = np.concatenate(([0.0], np.cumsum(chr_)))
        cost = kept_lum + alpha * lost_chr
        k = int(np.argmin(cost))
        cut = max(k, 0) / RBUCKETS
        if MODE == 'ntsc':
            if cut <= 0.0:
                th[b] = 0.999          # keep everything: huge exponent
            elif bases[b] >= 1.0 or cut >= 1.0:
                th[b] = 0.05
            else:
                th[b] = np.sqrt(np.log(cut) / (10.0 * np.log(bases[b])))
        else:
            th[b] = np.sqrt(cut)
    return np.clip(th, 0.05, 0.999)


def evaluate(name, comp_nr, clean, tff=False, **kw):
    # the rows compare separation modes at fixed eq/level; the plugin's
    # adaptive defaults would otherwise change what each label means
    kw.setdefault('eq', 1)
    kw.setdefault('level', 0)
    out = to_array(core.composite.Decode(gray_clip(comp_nr, tff), standard=STANDARD, **kw))
    res = score(out, clean)
    print(f"  {name:18s} PSNR Y {res['psnr_Y']:6.2f}  U {res['psnr_U']:6.2f}  "
          f"V {res['psnr_V']:6.2f}   chromaHF {res['chf']:7.1f}  flicker {res['flick']:7.1f}")


pat = f"*{'525' if MODE == 'ntsc' else '625'}*.y4m"
train, held = [], []
for d in CORPUS.split(':'):
    group = sorted(glob.glob(os.path.join(d, pat)))
    assert group, f'no corpus clips found in {d}'
    train += group[:-3]
    held += group[-3:]
print(f'{len(train)} training clips, {len(held)} held out; {FRAMES} frames each')

hist_lum = np.zeros((NBINS, RBUCKETS))
hist_chr = np.zeros((NBINS, RBUCKETS))
flat = np.empty((1, 3, H, W601), np.uint16)

for path in train:
    rows, row_off, parity, tff = geom(path)
    clean = load_frames(path, FRAMES, rows)
    if clean.shape[0] < FRAMES:
        print(f'  skipped {os.path.basename(path)} (too short)')
        continue
    lum_only = clean.copy()
    lum_only[:, 1:] = 32768
    chr_only = clean.copy()
    chr_only[:, 0] = 32128

    comp = encode(clean, tff)
    degraded = to_array(raster_to_601(bad_decode(comp, STANDARD, row_off), STANDARD, rows))
    comp_nr = encode(degraded, tff)
    if MODE == '2d':
        tile_stats(comp_nr, encode(lum_only, tff), encode(chr_only, tff),
                   hist_lum, hist_chr)
    else:
        tile_stats_3d(comp_nr, encode(lum_only, tff), encode(chr_only, tff),
                      hist_lum, hist_chr, rows=rows, parity=parity)
    print(f'  trained on {os.path.basename(path)}')

np.savez(f'threshold_hists_{MODE}.npz', lum=hist_lum, chr=hist_chr)
cands = {a: optimal_thresholds(hist_lum, hist_chr, a) for a in (0.5, 1.0, 2.0, 4.0)}
luts = {a: wiener_lut(hist_lum, hist_chr, a) for a in (0.5, 1.0, 2.0, 4.0)}
th = cands[ALPHA]
if MODE == '2d':
    print(f'\ncalibrated thresholds, alpha={ALPHA} (y rows 0-15, x bins fsc/2..fsc):')
    for y in range(YTILE):
        print('  ' + ' '.join(f'{v:.3f}' for v in th[y * len(BIN_X):(y + 1) * len(BIN_X)]))
else:
    print(f'\ncalibrated 3D {"t0" if MODE == "ntsc" else "thresholds"}, alpha={ALPHA}: '
          f'min {th.min():.3f} max {th.max():.3f} mean {th.mean():.3f}')

print('\nvalidation (held-out clips):')
for path in held:
    rows, row_off, parity, tff = geom(path)
    clean = load_frames(path, FRAMES, rows)
    if clean.shape[0] < FRAMES:
        print(f'  skipped {os.path.basename(path)} (too short)')
        continue
    comp = encode(clean, tff)
    degraded = to_array(raster_to_601(bad_decode(comp, STANDARD, row_off), STANDARD, rows))
    comp_nr = encode(degraded, tff)
    print(os.path.basename(path) + ':')
    dims = 2 if MODE == '2d' else 3
    tf = dict(transform=1) if MODE == 'ntsc' else {}
    evaluate('uniform 0.4', comp_nr, clean, tff, dimensions=dims, **tf)
    evaluate('uniform 0.7', comp_nr, clean, tff, dimensions=dims, threshold=0.7, **tf)
    evaluate('level', comp_nr, clean, tff, dimensions=dims, level=1, **tf)
    if MODE == 'ntsc':
        evaluate('comb 3D', comp_nr, clean, tff, dimensions=3, transform=0)
        evaluate('hybrid', comp_nr, clean, tff, dimensions=3, transform=2)
    for a, tha in cands.items():
        evaluate(f'calibrated a={a}', comp_nr, clean, tff, dimensions=dims,
                 thresholds=list(tha), **tf)
    for a, la in luts.items():
        evaluate(f'lut a={a}', comp_nr, clean, tff, dimensions=dims,
                 lut=list(la.ravel()), **tf)

prefix = 'ntsc' if MODE == 'ntsc' else f'pal_{MODE}'
np.savetxt(f'thresholds_{prefix}.txt' if MODE == 'ntsc' else f'thresholds_pal_{MODE}.txt',
           th, fmt='%.4f')
np.savetxt(f'lut_{prefix}.txt' if MODE == 'ntsc' else f'lut_pal_{MODE}.txt',
           luts[ALPHA].ravel(), fmt='%.4f')
print(f'\nsaved calibrated thresholds and lut (alpha={ALPHA}) '
      f'and threshold_hists_{MODE}.npz')
