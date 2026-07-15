/*
 * Composite decoding.
 *
 * PAL: 2D Transform PAL chroma separation followed by PALcolour-style
 * demodulation (ports of ld-chroma-decoder's transformpal2d.cpp and
 * palcolour.cpp transform path).
 *
 * NTSC: 2D line comb separation followed by product demodulation and
 * the color low-pass (port of comb.cpp split1D/split2D/filterIQ, with
 * luma reconstructed from resynthesized filtered chroma as in adjustY).
 *
 * The references recover line phase from the color burst or field
 * metadata; this raster carries neither, and both ends of the round
 * trip share the same deterministic subcarrier sequence, so the decoder
 * substitutes the exact values the encoder used. Separation heuristics
 * run in float; demodulation, filtering, and level mapping are fixed
 * point.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "decode.h"
#include "encode.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS COMP_DECODE_FILTER_SIZE
#define WIDTH COMP_ACTIVE_WIDTH_PAL

/* all-zero chroma used for lines beyond the edges */
static const int16_t zero_line[WIDTH];
static const float zero_line_f[WIDTH];

/* color low-pass for the demodulated NTSC products (deemp.h
 * c_colorlp_b rounded to Q15; the reference's +0.2% DC gain is kept) */
#define COLORLP_TAPS 17
static const int16_t colorlp_q15[COLORLP_TAPS] = {
    73, 317, 200, -682, -1597, -503, 3726, 9094, 11579,
    9094, 3726, -503, -1597, -682, 200, 317, 73,
};

int comp_decode_init(comp_decode_t *d, int standard, double threshold,
                     int nscratch, int setup)
{
    comp_encode_t enc;

    if (nscratch < 1)
        return -1;

    memset(d, 0, sizeof(*d));

    /* levels and chroma scales must match the encoder exactly */
    if (comp_encode_init(&enc, standard, setup))
        return -1;
    d->standard = standard;
    d->width = enc.width;
    d->height = standard == COMP_STD_PAL ? COMP_ACTIVE_HEIGHT_PAL
                                         : COMP_ACTIVE_HEIGHT_NTSC;
    d->den = enc.den;
    d->ku = enc.ku;
    d->kv = enc.kv;
    d->level_black = enc.level_black;
    d->luma_num = enc.luma_den;  /* the inverse slope */
    d->luma_den = enc.luma_num;
    memcpy(d->sin_q15, enc.sin_q15, sizeof(d->sin_q15));

    if (standard == COMP_STD_PAL) {
        /* PALcolour's 2D chroma filter: raised-cosine, ~1.18 MHz,
         * horizontal taps 0..FS mirrored, vertical taps at 0/±2/±1/±3
         * field lines (in that array order). Double at init, Q16 use. */
        const double fs_hz = 4.0 * 4433618.75;
        const double bw_hz = 1100000.0 / 0.93;
        const double ca = 0.5 * fs_hz / bw_hz;
        double cfilt[FS + 1][4];
        double cdiv = 0.0;

        for (int f = 0; f <= FS; f++) {
            const double fc   = fmin(ca, (double)f);
            const double ff   = fmin(ca, sqrt((double)f * f + 2 * 2));
            const double fff  = fmin(ca, sqrt((double)f * f + 4 * 4));
            const double ffff = fmin(ca, sqrt((double)f * f + 6 * 6));
            const int div = (f == 0) ? 2 : 1;

            cfilt[f][0] = (1.0 + cos(M_PI * fc   / ca)) / div;
            cfilt[f][2] = (1.0 + cos(M_PI * ff   / ca)) / div;
            cfilt[f][1] = (1.0 + cos(M_PI * fff  / ca)) / div;
            cfilt[f][3] = (1.0 + cos(M_PI * ffff / ca)) / div;

            cdiv += 2 * (cfilt[f][0] + 2 * cfilt[f][2] + 2 * cfilt[f][1] + 2 * cfilt[f][3]);
        }
        for (int f = 0; f <= FS; f++)
            for (int k = 0; k < 4; k++)
                d->cfilt_q16[f][k] = (int32_t)lrint(cfilt[f][k] / cdiv * 65536.0);

        if (comp_transform2d_init(&d->transform, threshold))
            return -1;
    } else {
        /* the comb's adaptivity range: 45 IRE of the encoded span */
        d->comb_krange = (int32_t)lrint(45.0 * (0xC800 - d->level_black) / 100.0);
    }

    d->scratch = calloc(nscratch, sizeof(*d->scratch));
    if (!d->scratch) {
        comp_transform2d_free(&d->transform);
        return -1;
    }
    d->nscratch = nscratch;
    for (int i = 0; i < nscratch; i++) {
        d->scratch[i].chroma_f = malloc(sizeof(float) * d->width * d->height);
        d->scratch[i].chroma = malloc(sizeof(int16_t) * d->width * d->height);
        if (!d->scratch[i].chroma_f || !d->scratch[i].chroma) {
            comp_decode_free(d);
            return -1;
        }
    }
    pthread_mutex_init(&d->lock, NULL);
    pthread_cond_init(&d->cond, NULL);
    return 0;
}

void comp_decode_free(comp_decode_t *d)
{
    if (d->scratch) {
        for (int i = 0; i < d->nscratch; i++) {
            free(d->scratch[i].chroma_f);
            free(d->scratch[i].chroma);
        }
        free(d->scratch);
        d->scratch = NULL;
        pthread_mutex_destroy(&d->lock);
        pthread_cond_destroy(&d->cond);
    }
    if (d->standard == COMP_STD_PAL)
        comp_transform2d_free(&d->transform);
}

static comp_decode_scratch_t *scratch_acquire(comp_decode_t *d)
{
    pthread_mutex_lock(&d->lock);
    for (;;) {
        for (int i = 0; i < d->nscratch; i++) {
            if (!d->scratch[i].used) {
                d->scratch[i].used = 1;
                pthread_mutex_unlock(&d->lock);
                return &d->scratch[i];
            }
        }
        pthread_cond_wait(&d->cond, &d->lock);
    }
}

static void scratch_release(comp_decode_t *d, comp_decode_scratch_t *s)
{
    pthread_mutex_lock(&d->lock);
    s->used = 0;
    pthread_cond_signal(&d->cond);
    pthread_mutex_unlock(&d->lock);
}

static inline int32_t rdiv(int64_t num, int32_t den)
{
    return (int32_t)((num >= 0 ? num + den / 2 : num - den / 2) / den);
}

static inline uint16_t clamp_u16(int32_t v)
{
    return (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v);
}

static inline int16_t clamp_i16(long v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* Demodulate one PAL field. comp and chroma are field views; the output
 * pointers are frame planes, written at rows 2*fieldline + field. */
static void pal_decode_field(const comp_decode_t *d, int frame, int field,
                             const uint16_t *comp, ptrdiff_t comp_stride,
                             const int16_t *chroma, ptrdiff_t chroma_stride,
                             uint16_t *dsty, ptrdiff_t ystride,
                             uint16_t *dstu, ptrdiff_t ustride,
                             uint16_t *dstv, ptrdiff_t vstride)
{
    const int w = d->width;
    const int rows = d->height / 2;

    /* products of chroma and the quadrature reference carriers, which at
     * 4xfsc are {0,1,0,-1} and {1,0,-1,0}: no multiplies. Padded by FS
     * zeros each side so the filter loop needs no bounds checks.
     * Vertical taps: [0] = line, [2] = ±1 line, [1] = ±2, [3] = ±3, with
     * the sign pattern of the reference implementation. */
    int32_t m[4][WIDTH + 2 * FS], n[4][WIDTH + 2 * FS];

    for (int fr = 0; fr < rows; fr++) {
        const int16_t *in0 = chroma + fr * chroma_stride;
        const int16_t *in1 = fr - 1 >= 0   ? chroma + (fr - 1) * chroma_stride : zero_line;
        const int16_t *in2 = fr + 1 < rows ? chroma + (fr + 1) * chroma_stride : zero_line;
        const int16_t *in3 = fr - 2 >= 0   ? chroma + (fr - 2) * chroma_stride : zero_line;
        const int16_t *in4 = fr + 2 < rows ? chroma + (fr + 2) * chroma_stride : zero_line;
        const int16_t *in5 = fr - 3 >= 0   ? chroma + (fr - 3) * chroma_stride : zero_line;
        const int16_t *in6 = fr + 3 < rows ? chroma + (fr + 3) * chroma_stride : zero_line;

        for (int k = 0; k < 4; k++) {
            memset(&m[k][0], 0, sizeof(int32_t) * FS);
            memset(&m[k][FS + w], 0, sizeof(int32_t) * FS);
            memset(&n[k][0], 0, sizeof(int32_t) * FS);
            memset(&n[k][FS + w], 0, sizeof(int32_t) * FS);
        }
        for (int x = 0; x < w; x++) {
            const int32_t sn = (x & 3) == 1 ? 1 : (x & 3) == 3 ? -1 : 0;
            const int32_t cs = (x & 3) == 0 ? 1 : (x & 3) == 2 ? -1 : 0;
            const int i = FS + x;
            m[0][i] = in0[x] * sn;
            m[2][i] = in1[x] * sn - in2[x] * sn;
            m[1][i] = -in3[x] * sn - in4[x] * sn;
            m[3][i] = -in5[x] * sn + in6[x] * sn;
            n[0][i] = in0[x] * cs;
            n[2][i] = in1[x] * cs - in2[x] * cs;
            n[1][i] = -in3[x] * cs - in4[x] * cs;
            n[3][i] = -in5[x] * cs + in6[x] * cs;
        }

        /* line phase: (bp,bq) is the unit vector the reference derives
         * from the burst (at -U); here it is exact */
        const int row = fr * 2 + field;
        const comp_sc_line_t sc = comp_sc_line(d->standard, frame, row);
        const int32_t bp = -d->sin_q15[(sc.phase + d->den / 4) % d->den];
        const int32_t bq = -d->sin_q15[sc.phase];

        const uint16_t *comp_row = comp + fr * comp_stride;
        uint16_t *outy = dsty + row * ystride;
        uint16_t *outu = dstu + row * ustride;
        uint16_t *outv = dstv + row * vstride;

        for (int x = 0; x < w; x++) {
            int64_t pu = 0, qu = 0, pv = 0, qv = 0;

            for (int b = 0; b <= FS; b++) {
                const int l = FS + x - b;
                const int r = FS + x + b;
                const int32_t *cf = d->cfilt_q16[b];

                const int32_t m0 = m[0][r] + m[0][l], n0 = n[0][r] + n[0][l];
                const int32_t m1 = m[1][r] + m[1][l], n1 = n[1][r] + n[1][l];
                const int32_t m2 = m[2][r] + m[2][l], n2 = n[2][r] + n[2][l];
                const int32_t m3 = m[3][r] + m[3][l], n3 = n[3][r] + n[3][l];

                pu += (int64_t)m0 * cf[0] + (int64_t)m1 * cf[1]
                    + (int64_t)n2 * cf[2] + (int64_t)n3 * cf[3];
                qu += (int64_t)n0 * cf[0] + (int64_t)n1 * cf[1]
                    - (int64_t)m2 * cf[2] - (int64_t)m3 * cf[3];
                pv += (int64_t)m0 * cf[0] + (int64_t)m1 * cf[1]
                    - (int64_t)n2 * cf[2] - (int64_t)n3 * cf[3];
                qv += (int64_t)n0 * cf[0] + (int64_t)n1 * cf[1]
                    + (int64_t)m2 * cf[2] + (int64_t)m3 * cf[3];
            }

            /* Q16 filter -> Q0 products (at half chroma amplitude) */
            const int32_t pu0 = (int32_t)((pu + 32768) >> 16);
            const int32_t qu0 = (int32_t)((qu + 32768) >> 16);
            const int32_t pv0 = (int32_t)((pv + 32768) >> 16);
            const int32_t qv0 = (int32_t)((qv + 32768) >> 16);

            /* rotate onto the U/V axes and double (the filter recovers
             * chroma at half amplitude); bp/bq are Q15, so >> 14 */
            const int32_t ul = (int32_t)(-((int64_t)pu0 * bp + (int64_t)qu0 * bq + 8192) >> 14);
            const int32_t vl = sc.vswitch *
                (int32_t)(-((int64_t)qv0 * bp - (int64_t)pv0 * bq + 8192) >> 14);

            /* luma is the composite minus the separated chroma */
            const int32_t yl = (int32_t)comp_row[x] - in0[x];

            /* invert the encoder's level mappings */
            outy[x] = clamp_u16(4096 + rdiv((int64_t)(yl - d->level_black) * d->luma_num, d->luma_den));
            outu[x] = clamp_u16(32768 + rdiv((int64_t)ul * 32768, d->ku));
            outv[x] = clamp_u16(32768 + rdiv((int64_t)vl * 32768, d->kv));
        }
    }
}

/* NTSC 2D line comb (comb.cpp split1D/split2D): a gentle 1D bandpass
 * centred on fsc, then a 3-line adaptive comb blending the differences
 * against the lines ±2 frame rows away (the same field's neighbouring
 * lines, 180 degrees out of chroma phase), weighted by similarity. */
static void ntsc_comb(const comp_decode_t *d, int rows,
                      const uint16_t *comp, ptrdiff_t comp_stride,
                      float *c1, int16_t *chroma)
{
    const int w = d->width;
    const float krange = (float)d->comb_krange;

    /* 1D bandpass [-0.25, 0, 0.5, 0, -0.25] centred on fsc */
    for (int r = 0; r < rows; r++) {
        const uint16_t *line = comp + r * comp_stride;
        float *out = c1 + r * w;
        out[0] = out[1] = out[w - 2] = out[w - 1] = 0.0f;
        for (int x = 2; x < w - 2; x++)
            out[x] = (2.0f * line[x] - line[x - 2] - line[x + 2]) / 4.0f;
    }

    for (int r = 0; r < rows; r++) {
        const float *cur = c1 + r * w;
        const float *prev = r - 2 >= 0   ? c1 + (r - 2) * w : zero_line_f;
        const float *next = r + 2 < rows ? c1 + (r + 2) * w : zero_line_f;
        int16_t *out = chroma + r * w;

        out[0] = 0;
        for (int x = 1; x < w; x++) {
            float kp, kn;

            kp  = fabsf(fabsf(cur[x]) - fabsf(prev[x]));
            kp += fabsf(fabsf(cur[x - 1]) - fabsf(prev[x - 1]));
            kp -= (fabsf(cur[x]) + fabsf(prev[x - 1])) * 0.10f;
            kn  = fabsf(fabsf(cur[x]) - fabsf(next[x]));
            kn += fabsf(fabsf(cur[x - 1]) - fabsf(next[x - 1]));
            kn -= (fabsf(cur[x]) + fabsf(next[x - 1])) * 0.10f;

            kp = 1.0f - kp / krange;
            kp = kp < 0.0f ? 0.0f : kp > 1.0f ? 1.0f : kp;
            kn = 1.0f - kn / krange;
            kn = kn < 0.0f ? 0.0f : kn > 1.0f ? 1.0f : kn;

            float sc = 1.0f;
            if (kn > 0.0f || kp > 0.0f) {
                if (kn > 3.0f * kp)
                    kp = 0.0f;
                else if (kp > 3.0f * kn)
                    kn = 0.0f;
                sc = 2.0f / (kn + kp);
                if (sc < 1.0f)
                    sc = 1.0f;
            } else if (fabsf(fabsf(prev[x]) - fabsf(next[x]))
                       - fabsf((next[x] + prev[x]) * 0.2f) <= 0.0f) {
                kn = kp = 1.0f;
            }

            const float tc = ((cur[x] - prev[x]) * kp * sc
                            + (cur[x] - next[x]) * kn * sc) / 4.0f;
            out[x] = clamp_i16(lrintf(tc));
        }
    }
}

/* Demodulate one NTSC line: product demod against the trivial 4xfsc
 * carriers, the reference's colour low-pass, rotation onto U/V, and
 * luma as composite minus the resynthesised filtered chroma. */
static void ntsc_demod_line(const comp_decode_t *d, int frame, int raster_row,
                            const uint16_t *comp_row, const int16_t *chroma_row,
                            uint16_t *outy, uint16_t *outu, uint16_t *outv)
{
    const int w = d->width;
    const int half = COLORLP_TAPS / 2;
    int32_t m[COMP_ACTIVE_WIDTH_NTSC + COLORLP_TAPS];
    int32_t n[COMP_ACTIVE_WIDTH_NTSC + COLORLP_TAPS];

    memset(m, 0, sizeof(m));
    memset(n, 0, sizeof(n));
    for (int x = 0; x < w; x++) {
        const int32_t sn = (x & 3) == 1 ? 1 : (x & 3) == 3 ? -1 : 0;
        const int32_t cs = (x & 3) == 0 ? 1 : (x & 3) == 2 ? -1 : 0;
        m[half + x] = chroma_row[x] * sn;
        n[half + x] = chroma_row[x] * cs;
    }

    const comp_sc_line_t sc = comp_sc_line(d->standard, frame, raster_row);
    const int32_t sn0 = d->sin_q15[sc.phase];
    const int32_t cs0 = d->sin_q15[(sc.phase + d->den / 4) % d->den];
    const int32_t bp = -cs0;
    const int32_t bq = -sn0;
    const int32_t s4[4] = { sn0, cs0, -sn0, -cs0 };
    const int32_t c4[4] = { cs0, -sn0, -cs0, sn0 };

    for (int x = 0; x < w; x++) {
        int64_t p = 0, q = 0;
        for (int j = 0; j < COLORLP_TAPS; j++) {
            p += (int64_t)colorlp_q15[j] * m[x + j];
            q += (int64_t)colorlp_q15[j] * n[x + j];
        }
        const int32_t p0 = (int32_t)((p + 16384) >> 15);
        const int32_t q0 = (int32_t)((q + 16384) >> 15);

        const int32_t ul = (int32_t)(-((int64_t)p0 * bp + (int64_t)q0 * bq + 8192) >> 14);
        const int32_t vl = (int32_t)(-((int64_t)q0 * bp - (int64_t)p0 * bq + 8192) >> 14);

        /* comb.cpp adjustY: subtract the filtered chroma, resynthesised
         * on the carrier, rather than the raw comb output */
        const int32_t re = (ul * s4[x & 3] + vl * c4[x & 3] + 16384) >> 15;
        const int32_t yl = (int32_t)comp_row[x] - re;

        outy[x] = clamp_u16(4096 + rdiv((int64_t)(yl - d->level_black) * d->luma_num, d->luma_den));
        outu[x] = clamp_u16(32768 + rdiv((int64_t)ul * 32768, d->ku));
        outv[x] = clamp_u16(32768 + rdiv((int64_t)vl * 32768, d->kv));
    }
}

void comp_decode_frame(comp_decode_t *d, int frame, int rows, int row_off,
                       const uint16_t *comp, ptrdiff_t comp_stride,
                       uint16_t *dsty, ptrdiff_t ystride,
                       uint16_t *dstu, ptrdiff_t ustride,
                       uint16_t *dstv, ptrdiff_t vstride)
{
    comp_decode_scratch_t *s = scratch_acquire(d);
    const int w = d->width;

    if (d->standard == COMP_STD_PAL) {
        const int frows = rows / 2;

        /* chroma separation per field; field views interleave rows */
        for (int field = 0; field < 2; field++) {
            comp_transform2d_field(&d->transform,
                                   comp + field * comp_stride, 2 * comp_stride,
                                   w, frows,
                                   s->chroma_f + field * w, 2 * w);
        }

        /* quantise the separated chroma once for the fixed-point demod */
        for (int i = 0; i < w * rows; i++)
            s->chroma[i] = clamp_i16(lrintf(s->chroma_f[i]));

        for (int field = 0; field < 2; field++) {
            pal_decode_field(d, frame, field,
                             comp + field * comp_stride, 2 * comp_stride,
                             s->chroma + field * w, 2 * w,
                             dsty, ystride, dstu, ustride, dstv, vstride);
        }
    } else {
        /* the comb works on frame rows directly: ±2 rows are the same
         * field's neighbouring lines */
        ntsc_comb(d, rows, comp, comp_stride, s->chroma_f, s->chroma);
        for (int r = 0; r < rows; r++)
            ntsc_demod_line(d, frame, r + row_off,
                            comp + r * comp_stride, s->chroma + r * w,
                            dsty + r * ystride, dstu + r * ustride,
                            dstv + r * vstride);
    }

    scratch_release(d, s);
}
