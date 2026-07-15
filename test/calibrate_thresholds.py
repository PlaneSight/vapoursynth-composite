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
# usage: calibrate_thresholds.py composite.so corpus_dir [frames_per_clip] [2d|3d]

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
H, WR = 576, 928


def gray_clip(arr):
    n, h, w = arr.shape
    base = core.std.BlankClip(format=vs.GRAY16, width=w, height=h, length=n,
                              fpsnum=25, fpsden=1)

    def fill(n, f):
        fout = f.copy()
        np.asarray(fout[0])[:] = arr[n]
        return fout

    return core.std.ModifyFrame(base, base, fill)


def load_frames(path, count):
    # 2D can sample scattered frames; 3D needs consecutive fields
    vf = ('select=gte(n\\,80),' if MODE == '3d' else 'select=not(mod(n\\,40)),') \
         + 'format=yuv444p16le'
    r = subprocess.run([metrics.FFMPEG or 'ffmpeg', '-v', 'error', '-i', path,
                        '-vf', vf, '-frames:v', str(count), '-f', 'rawvideo', '-'],
                       capture_output=True)
    a = np.frombuffer(r.stdout, np.uint16)
    n = a.size // (3 * H * W601)
    return a[:n * 3 * H * W601].reshape(n, 3, H, W601).copy()


def encode(arr):
    return to_array(core.composite.Encode(clip_from(arr), standard='pal'))


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


def tile_stats_3d(comp_nr, comp_lum, comp_chr, hist_lum, hist_chr):
    # 3D tiles in frame-line space: rows of the other field are black,
    # exactly as the 3D transform builds them
    def window(nn):
        return 0.5 - 0.5 * np.cos(2 * np.pi * (np.arange(nn) + 0.5) / nn)

    win = window(Z3)[:, None, None] * window(Y3)[None, :, None] * window(X3)[None, None, :]
    refs = [(z, y, x,
             ((Z3 // 4) + Z3 - z) % Z3, ((Y3 // 4) + Y3 - y) % Y3, X3 // 2 - x)
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
                if 0 <= fl < H and (fl & 1) == (g & 1):
                    t[z, y] = f[fl >> 1, tx:tx + X3]
        return t * win

    for tz in range(0, nfields - Z3 + 1, Z3 // 2):
        for ty in range(0, H - Y3 + 1, Y3):
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


def optimal_thresholds(hist_lum, hist_chr, alpha):
    # keep iff r >= t^2: cost(t) = kept luma + alpha * discarded chroma
    th = np.full(NBINS, 0.4)
    for b in range(NBINS):
        lum, chr_ = hist_lum[b], hist_chr[b]
        # cost when the cut is placed before bucket k (keep buckets >= k)
        kept_lum = np.concatenate(([lum.sum()], lum.sum() - np.cumsum(lum)))
        lost_chr = np.concatenate(([0.0], np.cumsum(chr_)))
        cost = kept_lum + alpha * lost_chr
        k = int(np.argmin(cost))
        th[b] = np.sqrt(max(k, 0) / RBUCKETS)
    return np.clip(th, 0.05, 0.999)


def evaluate(name, comp_nr, clean, **kw):
    out = to_array(core.composite.Decode(gray_clip(comp_nr), standard='pal', **kw))
    res = score(out, clean)
    print(f"  {name:18s} PSNR Y {res['psnr_Y']:6.2f}  U {res['psnr_U']:6.2f}  "
          f"V {res['psnr_V']:6.2f}   chromaHF {res['chf']:7.1f}  flicker {res['flick']:7.1f}")


clips = sorted(glob.glob(os.path.join(CORPUS, '*625*.y4m')))
assert clips, 'no 625-line clips found'
train, held = clips[:-3], clips[-3:]
print(f'{len(train)} training clips, {len(held)} held out; {FRAMES} frames each')

hist_lum = np.zeros((NBINS, RBUCKETS))
hist_chr = np.zeros((NBINS, RBUCKETS))
flat = np.empty((1, 3, H, W601), np.uint16)

for path in train:
    clean = load_frames(path, FRAMES)
    lum_only = clean.copy()
    lum_only[:, 1:] = 32768
    chr_only = clean.copy()
    chr_only[:, 0] = 32128

    comp = encode(clean)
    degraded = to_array(raster_to_601(bad_decode(comp, 'pal', 0), 'pal', H))
    comp_nr = encode(degraded)
    stats = tile_stats if MODE == '2d' else tile_stats_3d
    stats(comp_nr, encode(lum_only), encode(chr_only), hist_lum, hist_chr)
    print(f'  trained on {os.path.basename(path)}')

np.savez(f'threshold_hists_{MODE}.npz', lum=hist_lum, chr=hist_chr)
cands = {a: optimal_thresholds(hist_lum, hist_chr, a) for a in (0.5, 1.0, 2.0, 4.0)}
th = cands[ALPHA]
if MODE == '2d':
    print(f'\ncalibrated thresholds, alpha={ALPHA} (y rows 0-15, x bins fsc/2..fsc):')
    for y in range(YTILE):
        print('  ' + ' '.join(f'{v:.3f}' for v in th[y * len(BIN_X):(y + 1) * len(BIN_X)]))
else:
    print(f'\ncalibrated 3D thresholds, alpha={ALPHA}: '
          f'min {th.min():.3f} max {th.max():.3f} mean {th.mean():.3f}')

print('\nvalidation (held-out clips):')
for path in held:
    clean = load_frames(path, FRAMES)
    comp = encode(clean)
    degraded = to_array(raster_to_601(bad_decode(comp, 'pal', 0), 'pal', H))
    comp_nr = encode(degraded)
    print(os.path.basename(path) + ':')
    dims = 2 if MODE == '2d' else 3
    evaluate('uniform 0.4', comp_nr, clean, dimensions=dims)
    evaluate('uniform 0.7', comp_nr, clean, dimensions=dims, threshold=0.7)
    for a, tha in cands.items():
        evaluate(f'calibrated a={a}', comp_nr, clean, dimensions=dims, thresholds=list(tha))

np.savetxt(f'thresholds_pal_{MODE}.txt', th, fmt='%.4f')
print(f'\nsaved thresholds_pal_{MODE}.txt and threshold_hists_{MODE}.npz')
