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

/* response of a symmetric FIR (Q15 taps) at normalized angular freq w */
static double fir_response_q15(const int16_t *taps, int n, double w)
{
    double sum = 0.0;
    for (int k = 0; k < n; k++)
        sum += taps[k] / 32768.0 * cos(w * (k - (n - 1) / 2.0));
    return sum;
}

/* Design the chroma cascade equalizer: the inverse of the known
 * encode-filter x decode-filter chroma response, boost capped at 12 dB,
 * Hann-windowed to COMP_EQ_TAPS taps with an exactly unity cascade at
 * DC. dec_taps is the decoder's post-demod profile in 1/32768 units. */
static void design_eq(comp_decode_t *d, const int16_t *enc_taps, int enc_n,
                      const double *dec_taps, int dec_n)
{
    enum { M = 256 };
    double heq[M / 2 + 1];
    double h[COMP_EQ_TAPS];
    const int c = COMP_EQ_TAPS / 2;

    for (int m = 0; m <= M / 2; m++) {
        const double w = 2.0 * M_PI * m / M;
        double dec = 0.0;
        for (int k = 0; k < dec_n; k++)
            dec += dec_taps[k] * cos(w * (k - (dec_n - 1) / 2.0));
        const double g = fir_response_q15(enc_taps, enc_n, w) * dec;
        heq[m] = g > 0.25 ? 1.0 / g : 4.0;
        if (heq[m] > 4.0)
            heq[m] = 4.0;
        if (heq[m] < 0.0)
            heq[m] = 0.0;
    }

    double dc = 0.0;
    for (int j = 0; j < COMP_EQ_TAPS; j++) {
        double sum = heq[0];
        for (int m = 1; m < M / 2; m++)
            sum += 2.0 * heq[m] * cos(2.0 * M_PI * m * (j - c) / M);
        sum += heq[M / 2] * cos(M_PI * (j - c));
        const double hann = 0.5 + 0.5 * cos(M_PI * (j - c) / (c + 1));
        h[j] = sum / M * hann;
        dc += h[j];
    }
    for (int j = 0; j < COMP_EQ_TAPS; j++)
        d->eq_q15[j] = (int32_t)lrint(h[j] * (heq[0] / dc) * 32768.0);
}

int comp_decode_init(comp_decode_t *d, int standard, double threshold,
                     int nscratch, int setup, int dimensions, int eq, int refine,
                     int use_transform, int level)
{
    if (nscratch < 1 || dimensions < 1 || dimensions > 3 || refine < 0)
        return -1;
    if (use_transform && (standard != COMP_STD_NTSC || dimensions != 3))
        return -1;
    if (level && (dimensions < 2 || (standard == COMP_STD_NTSC && !use_transform)))
        return -1;
    if (eq < 0 || eq > 2)
        return -1;
    if (eq == 2 && !(standard == COMP_STD_PAL ? dimensions >= 2 : use_transform))
        return -1;

    memset(d, 0, sizeof(*d));

    /* levels and chroma scales must match the encoder exactly; the
     * encoder is also the resynthesis step of the refine loop */
    comp_encode_t enc;
    if (comp_encode_init(&enc, standard, setup, 0))
        return -1;
    d->enc = enc;
    d->refine = refine;
    d->standard = standard;
    d->dimensions = dimensions;
    d->use_transform = !!use_transform;
    d->eq = eq;
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

    if (standard == COMP_STD_PAL && dimensions > 1) {
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

        if (eq) {
            /* the filter's horizontal profile: for vertically flat
             * chroma the U path reduces to these mirrored taps */
            double prof[2 * FS + 1];
            for (int f = -FS; f <= FS; f++) {
                const int a = f < 0 ? -f : f;
                prof[FS + f] = (cfilt[a][0] + 2 * cfilt[a][1]
                              + 2 * cfilt[a][2] + 2 * cfilt[a][3]) / cdiv
                             * (f == 0 ? 2.0 : 1.0);
            }
            design_eq(d, enc.uv_taps, enc.uv_ntaps, prof, 2 * FS + 1);
        }

        if (dimensions == 2 ? comp_transform2d_init(&d->transform, threshold, level)
                            : comp_transform3d_init(&d->transform3, threshold, standard, level))
            return -1;
    } else if (standard == COMP_STD_PAL) {
        /* dimensions=1 uses the crude path only */
        if (eq) {
            double prof[COLORLP_TAPS];
            for (int k = 0; k < COLORLP_TAPS; k++)
                prof[k] = colorlp_q15[k] / 32768.0;
            design_eq(d, enc.uv_taps, enc.uv_ntaps, prof, COLORLP_TAPS);
        }
    } else {
        /* the comb's adaptivity range: 45 IRE of the encoded span */
        d->comb_krange = (int32_t)lrint(45.0 * (0xC800 - d->level_black) / 100.0);

        if (use_transform && comp_transform3d_init(&d->transform3, threshold, standard, level))
            return -1;

        if (eq) {
            double prof[COLORLP_TAPS];
            for (int k = 0; k < COLORLP_TAPS; k++)
                prof[k] = colorlp_q15[k] / 32768.0;
            design_eq(d, enc.uv_taps, enc.uv_ntaps, prof, COLORLP_TAPS);
        }
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
        if (standard == COMP_STD_NTSC && dimensions > 1) {
            d->scratch[i].tmp3 = malloc(sizeof(float) * d->width * d->height
                                        * (dimensions == 3 ? 6 : 1));
            if (!d->scratch[i].tmp3) {
                comp_decode_free(d);
                return -1;
            }
        }
        if (eq == 2) {
            d->scratch[i].conf = malloc(sizeof(float) * d->width * d->height);
            if (!d->scratch[i].conf) {
                comp_decode_free(d);
                return -1;
            }
        }
        if (refine > 0) {
            /* YUV estimate (3 planes), recomposite, crude Y, spare */
            d->scratch[i].refine = malloc(sizeof(uint16_t) * d->width * d->height * 6);
            if (!d->scratch[i].refine) {
                comp_decode_free(d);
                return -1;
            }
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
            free(d->scratch[i].conf);
            free(d->scratch[i].tmp3);
            free(d->scratch[i].refine);
        }
        free(d->scratch);
        d->scratch = NULL;
        pthread_mutex_destroy(&d->lock);
        pthread_cond_destroy(&d->cond);
    }
    if (d->standard == COMP_STD_PAL || d->use_transform) {
        if (d->dimensions == 2)
            comp_transform2d_free(&d->transform);
        else if (d->dimensions == 3)
            comp_transform3d_free(&d->transform3);
    }
}

int comp_decode_set_thresholds(comp_decode_t *d, const double *t, int n)
{
    if (d->standard != COMP_STD_PAL && !d->use_transform)
        return -1;
    if (d->dimensions == 2 && n == COMP_T2D_NTHRESH) {
        for (int i = 0; i < n; i++)
            d->transform.threshold_sq[i] = (float)(t[i] * t[i]);
        return 0;
    }
    if (d->dimensions == 3 && n == COMP_T3D_NTHRESH) {
        /* for NTSC these feed the shaped-threshold exponent */
        for (int i = 0; i < n; i++)
            d->transform3.threshold_sq[i] = (float)(t[i] * t[i]);
        return 0;
    }
    return -1;
}

int comp_decode_set_lut(comp_decode_t *d, const double *v, int n)
{
    if (d->standard != COMP_STD_PAL && !d->use_transform)
        return -1;
    if (d->standard == COMP_STD_PAL && d->dimensions == 2
        && n == COMP_T2D_NTHRESH * COMP_LUT_K) {
        comp_transform2d_set_lut(&d->transform, v);
        return 0;
    }
    if (d->dimensions == 3 && n == COMP_T3D_NTHRESH * COMP_LUT_K) {
        comp_transform3d_set_lut(&d->transform3, v);
        return 0;
    }
    return -1;
}

int comp_decode_look(const comp_decode_t *d)
{
    if (d->dimensions != 3)
        return 0;
    return (d->standard == COMP_STD_PAL || d->use_transform) ? COMP_T3D_LOOK : 1;
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

/* equalize one row of demodulated chroma levels in place */
static void eq_row(const comp_decode_t *d, const int32_t *in, int32_t *out, int w)
{
    const int half = COMP_EQ_TAPS / 2;

    for (int x = 0; x < w; x++) {
        int64_t acc = 0;
        for (int j = 0; j < COMP_EQ_TAPS; j++) {
            const int k = x + j - half;
            if (k >= 0 && k < w)
                acc += (int64_t)d->eq_q15[j] * in[k];
        }
        out[x] = (int32_t)((acc + 16384) >> 15);
    }
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
 * pointers are frame planes, written at rows 2*fieldline + field.
 * conf (a field view at chroma_stride, may be NULL) is the transform's
 * chroma-confidence map, which scales the eq=2 boost per sample. */
static void pal_decode_field(const comp_decode_t *d, int frame, int field,
                             const uint16_t *comp, ptrdiff_t comp_stride,
                             const int16_t *chroma, ptrdiff_t chroma_stride,
                             const float *conf,
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
        int32_t u_row[WIDTH], v_row[WIDTH], u_eq[WIDTH], v_eq[WIDTH];

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
            u_row[x] = (int32_t)(-((int64_t)pu0 * bp + (int64_t)qu0 * bq + 8192) >> 14);
            v_row[x] = sc.vswitch *
                (int32_t)(-((int64_t)qv0 * bp - (int64_t)pv0 * bq + 8192) >> 14);

            /* luma is the composite minus the separated chroma */
            const int32_t yl = (int32_t)comp_row[x] - in0[x];
            outy[x] = clamp_u16(4096 + rdiv((int64_t)(yl - d->level_black) * d->luma_num, d->luma_den));
        }

        const int32_t *u_out = u_row, *v_out = v_row;
        if (d->eq) {
            eq_row(d, u_row, u_eq, w);
            eq_row(d, v_row, v_eq, w);
            if (d->eq == 2 && conf) {
                /* scale the boost by the separation's confidence: full
                 * where the kept pairs were symmetric (real chroma),
                 * none where they were marginal or absent. Real chroma
                 * measures ~0.94 mean ratio and leak ~0.3-0.8, so the
                 * fourth power widens the gap (0.79 vs under 0.1). */
                const float *cw = conf + fr * chroma_stride;
                for (int x = 0; x < w; x++) {
                    const float c2 = cw[x] * cw[x];
                    int32_t wq = (int32_t)lrintf(c2 * c2 * 32768.0f);
                    wq = wq < 0 ? 0 : wq > 32768 ? 32768 : wq;
                    u_eq[x] = u_row[x]
                        + (int32_t)(((int64_t)wq * (u_eq[x] - u_row[x])) >> 15);
                    v_eq[x] = v_row[x]
                        + (int32_t)(((int64_t)wq * (v_eq[x] - v_row[x])) >> 15);
                }
            }
            u_out = u_eq;
            v_out = v_eq;
        }

        /* invert the encoder's level mappings */
        for (int x = 0; x < w; x++) {
            outu[x] = clamp_u16(32768 + rdiv((int64_t)u_out[x] * 32768, d->ku));
            outv[x] = clamp_u16(32768 + rdiv((int64_t)v_out[x] * 32768, d->kv));
        }
    }
}

/* NTSC 1D bandpass [-0.25, 0, 0.5, 0, -0.25] centered on fsc
 * (comb.cpp split1D) */
static void ntsc_comb1d(const comp_decode_t *d, int rows,
                        const uint16_t *comp, ptrdiff_t comp_stride, float *c1)
{
    const int w = d->width;

    for (int r = 0; r < rows; r++) {
        const uint16_t *line = comp + r * comp_stride;
        float *out = c1 + r * w;
        out[0] = out[1] = out[w - 2] = out[w - 1] = 0.0f;
        for (int x = 2; x < w - 2; x++)
            out[x] = (2.0f * line[x] - line[x - 2] - line[x + 2]) / 4.0f;
    }
}

/* NTSC 3-line adaptive comb on the 1D chroma (comb.cpp split2D): blend
 * the differences against the lines ±2 frame rows away (the same
 * field's neighboring lines, 180 degrees out of chroma phase),
 * weighted by similarity. */
static void ntsc_comb2d(const comp_decode_t *d, int rows, const float *c1, float *c2)
{
    const int w = d->width;
    const float krange = (float)d->comb_krange;

    for (int r = 0; r < rows; r++) {
        const float *cur = c1 + r * w;
        const float *prev = r - 2 >= 0   ? c1 + (r - 2) * w : zero_line_f;
        const float *next = r + 2 < rows ? c1 + (r + 2) * w : zero_line_f;
        float *out = c2 + r * w;

        out[0] = 0.0f;
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

            out[x] = ((cur[x] - prev[x]) * kp * sc
                    + (cur[x] - next[x]) * kn * sc) / 4.0f;
        }
    }
}


/* line phase parity, matching the reference's getLinePhase: the NTSC
 * subcarrier advances half a cycle per broadcast line, so the parity of
 * the line count within the color sequence selects one of the two
 * phase classes */
static int ntsc_line_phase(int frame, int raster_row)
{
    const int frame_line = 39 + raster_row;
    const int field_id = ((frame % 2) * 2 + (frame_line & 1)) % 4;
    const int prev_lines = (field_id / 2) * 525 + (field_id % 2) * 263 + frame_line / 2;
    return prev_lines & 1;
}

/* NTSC adaptive 3D comb (comb.cpp split3D/getBestCandidate): pick the
 * most similar sample that should be 180 degrees out of chroma phase,
 * from this line, neighboring lines, or the neighboring fields and
 * frames, and comb against it; if a same-frame candidate wins, reuse
 * the 2D result. Buffers are indexed 0/1/2 = previous/current/next. */
static void ntsc_split3d(const comp_decode_t *d, int rows, int row_off,
                         const comp_frame_view_t *views, const int *view_frames,
                         const float *const c1[3], const float *const c2[3],
                         int16_t *chroma)
{
    const int w = d->width;
    const double irescale = d->comb_krange / 45.0;
    /* bias bonuses prefer frame over field over line candidates
     * (comb.cpp adaptThreshold = 1.0, chromaWeight = 1.0) */
    static const double LINE_BONUS = -2.0, FIELD_BONUS = -4.0, FRAME_BONUS = -6.0;

    for (int r = 0; r < rows; r++) {
        const int cur_lp = ntsc_line_phase(view_frames[1], r + row_off);
        const float *c1c = c1[1] + r * w;
        const float *c2c = c2[1] + r * w;
        int16_t *out = chroma + r * w;

        for (int x = 0; x < w; x++) {
            if (x < 3 || x >= w - 3) {
                out[x] = clamp_i16(lrintf(c2c[x]));
                continue;
            }

            const int want = (2 + 2 * cur_lp + x) % 4;

            /* candidate table: buffer, line, sample, bonus; the first
             * four are the same-frame (1D/2D) candidates */
            struct { int k, cr, ch; double bonus; } cand[8] = {
                { 1, r, x - 2, 0.0 },
                { 1, r, x + 2, 0.0 },
                { 1, r - 2, x, LINE_BONUS },
                { 1, r + 2, x, LINE_BONUS },
                { 1, r - 1, x, FIELD_BONUS },
                { 1, r + 1, x, FIELD_BONUS },
                { 0, r, x, FRAME_BONUS },
                { 2, r, x, FRAME_BONUS },
            };
            /* adjacent-field candidates come from this frame or the
             * neighbouring one, whichever gives the matching phase */
            if (cur_lp == ntsc_line_phase(view_frames[1], r + row_off - 1))
                cand[4].k = 0;
            else
                cand[5].k = 2;

            double best_penalty = 0.0;
            float best_sample = 0.0f;
            int best = -1;

            for (int i = 0; i < 8; i++) {
                const int k = cand[i].k, cr = cand[i].cr, ch = cand[i].ch;
                double penalty = 1000.0;
                float sample = 0.0f;

                if (cr >= 0 && cr < rows && ch >= 1 && ch < w - 1) {
                    sample = c1[k][cr * w + ch];
                    const int have = (2 * ntsc_line_phase(view_frames[k], cr + row_off) + ch) % 4;
                    if (want == have) {
                        const uint16_t *ref_line = views[1].data + r * views[1].stride;
                        const uint16_t *cand_line = views[k].data + cr * views[k].stride;
                        double ypen = 0.0, iqpen = 0.0;
                        static const double weights[3] = { 0.5, 1.0, 0.5 };
                        for (int o = -1; o <= 1; o++) {
                            const double ref_c = c2c[x + o];
                            const double cand_c = c2[k][cr * w + ch + o];
                            ypen += fabs((ref_line[x + o] - ref_c)
                                         - (cand_line[ch + o] - cand_c));
                            iqpen += fabs(ref_c + cand_c) * weights[o + 1];
                        }
                        penalty = ypen / 3.0 / irescale
                                + (iqpen / 2.0 / irescale) * 0.28
                                + cand[i].bonus;
                    }
                }

                if (best < 0 || penalty < best_penalty) {
                    best = i;
                    best_penalty = penalty;
                    best_sample = sample;
                }
            }

            const float tc = best < 4 ? c2c[x] : (c1c[x] - best_sample) / 2.0f;
            out[x] = clamp_i16(lrintf(tc));
        }
    }
}

/* Demodulate one NTSC line: product demod against the trivial 4xfsc
 * carriers, the reference's color low-pass, rotation onto U/V, and
 * luma as composite minus the resynthesized filtered chroma. conf
 * (may be NULL) is the transform's confidence row for the eq=2 blend. */
static void ntsc_demod_line(const comp_decode_t *d, int frame, int raster_row,
                            const uint16_t *comp_row, const int16_t *chroma_row,
                            const float *conf,
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

    int32_t u_row[COMP_ACTIVE_WIDTH_NTSC], v_row[COMP_ACTIVE_WIDTH_NTSC];
    int32_t u_eq[COMP_ACTIVE_WIDTH_NTSC], v_eq[COMP_ACTIVE_WIDTH_NTSC];

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
        u_row[x] = ul;
        v_row[x] = vl;

        /* comb mode follows comb.cpp adjustY (subtract the filtered
         * chroma resynthesized on the carrier, using the values before
         * equalization); transform mode subtracts the separated chroma
         * directly, as Transform PAL does */
        int32_t yl;
        if (d->use_transform) {
            yl = (int32_t)comp_row[x] - chroma_row[x];
        } else {
            const int32_t re = (ul * s4[x & 3] + vl * c4[x & 3] + 16384) >> 15;
            yl = (int32_t)comp_row[x] - re;
        }
        outy[x] = clamp_u16(4096 + rdiv((int64_t)(yl - d->level_black) * d->luma_num, d->luma_den));
    }

    const int32_t *u_out = u_row, *v_out = v_row;
    if (d->eq) {
        eq_row(d, u_row, u_eq, w);
        eq_row(d, v_row, v_eq, w);
        if (d->eq == 2 && conf) {
            /* the PAL blend: boost scaled by the fourth power of the
             * transform's pair-symmetry confidence */
            for (int x = 0; x < w; x++) {
                const float c2 = conf[x] * conf[x];
                int32_t wq = (int32_t)lrintf(c2 * c2 * 32768.0f);
                wq = wq < 0 ? 0 : wq > 32768 ? 32768 : wq;
                u_eq[x] = u_row[x]
                    + (int32_t)(((int64_t)wq * (u_eq[x] - u_row[x])) >> 15);
                v_eq[x] = v_row[x]
                    + (int32_t)(((int64_t)wq * (v_eq[x] - v_row[x])) >> 15);
            }
        }
        u_out = u_eq;
        v_out = v_eq;
    }

    for (int x = 0; x < w; x++) {
        outu[x] = clamp_u16(32768 + rdiv((int64_t)u_out[x] * 32768, d->ku));
        outv[x] = clamp_u16(32768 + rdiv((int64_t)v_out[x] * 32768, d->kv));
    }
}


/* Crude 1D notch decode (a classic cheap decoder, and the degradation
 * model of the refine loop): bandpass chroma estimate, product demod
 * with the color low-pass, luma = composite minus the estimate. */
static void crude_decode_frame(const comp_decode_t *d, int frame, int rows,
                               int row_off, int use_eq,
                               const uint16_t *comp, ptrdiff_t comp_stride,
                               uint16_t *dsty, ptrdiff_t ystride,
                               uint16_t *dstu, ptrdiff_t ustride,
                               uint16_t *dstv, ptrdiff_t vstride)
{
    const int w = d->width;
    const int half = COLORLP_TAPS / 2;

    for (int r = 0; r < rows; r++) {
        const uint16_t *line = comp + r * comp_stride;
        int32_t est[COMP_ACTIVE_WIDTH_PAL];
        int32_t m[COMP_ACTIVE_WIDTH_PAL + COLORLP_TAPS];
        int32_t n[COMP_ACTIVE_WIDTH_PAL + COLORLP_TAPS];
        int32_t u_row[COMP_ACTIVE_WIDTH_PAL], v_row[COMP_ACTIVE_WIDTH_PAL];
        int32_t u_eq[COMP_ACTIVE_WIDTH_PAL], v_eq[COMP_ACTIVE_WIDTH_PAL];

        est[0] = est[1] = est[w - 2] = est[w - 1] = 0;
        for (int x = 2; x < w - 2; x++)
            est[x] = (2 * (int32_t)line[x] - line[x - 2] - line[x + 2]) / 4;

        memset(m, 0, sizeof(int32_t) * half);
        memset(n, 0, sizeof(int32_t) * half);
        memset(&m[half + w], 0, sizeof(int32_t) * half);
        memset(&n[half + w], 0, sizeof(int32_t) * half);
        for (int x = 0; x < w; x++) {
            const int32_t sn = (x & 3) == 1 ? 1 : (x & 3) == 3 ? -1 : 0;
            const int32_t cs = (x & 3) == 0 ? 1 : (x & 3) == 2 ? -1 : 0;
            m[half + x] = est[x] * sn;
            n[half + x] = est[x] * cs;
        }

        const comp_sc_line_t sc = comp_sc_line(d->standard, frame, r + row_off);
        const int32_t sn0 = d->sin_q15[sc.phase];
        const int32_t cs0 = d->sin_q15[(sc.phase + d->den / 4) % d->den];

        for (int x = 0; x < w; x++) {
            int64_t p = 0, q = 0;
            for (int j = 0; j < COLORLP_TAPS; j++) {
                p += (int64_t)colorlp_q15[j] * m[x + j];
                q += (int64_t)colorlp_q15[j] * n[x + j];
            }
            const int32_t p0 = (int32_t)((p + 16384) >> 15);
            const int32_t q0 = (int32_t)((q + 16384) >> 15);
            u_row[x] = (int32_t)(((int64_t)p0 * cs0 + (int64_t)q0 * sn0 + 8192) >> 14);
            v_row[x] = sc.vswitch *
                (int32_t)(((int64_t)q0 * cs0 - (int64_t)p0 * sn0 + 8192) >> 14);

            const int32_t yl = (int32_t)line[x] - est[x];
            dsty[r * ystride + x] =
                clamp_u16(4096 + rdiv((int64_t)(yl - d->level_black) * d->luma_num, d->luma_den));
        }

        const int32_t *u_out = u_row, *v_out = v_row;
        if (use_eq && d->eq) {
            eq_row(d, u_row, u_eq, w);
            eq_row(d, v_row, v_eq, w);
            u_out = u_eq;
            v_out = v_eq;
        }
        for (int x = 0; x < w; x++) {
            dstu[r * ustride + x] = clamp_u16(32768 + rdiv((int64_t)u_out[x] * 32768, d->ku));
            dstv[r * vstride + x] = clamp_u16(32768 + rdiv((int64_t)v_out[x] * 32768, d->kv));
        }
    }
}

/* Y-only Landweber refinement, anchored to the pre-encode original:
 * iterate the luma estimate so the crude decode of its recomposite
 * converges to the original degraded luma, i.e. deconvolve the crude
 * decoder model. Chroma stays fixed. */
static void refine_luma(const comp_decode_t *d, comp_decode_scratch_t *s,
                        int frame, int rows, int row_off,
                        const uint16_t *orig_y, ptrdiff_t orig_stride,
                        uint16_t *dsty, ptrdiff_t ystride,
                        uint16_t *dstu, ptrdiff_t ustride,
                        uint16_t *dstv, ptrdiff_t vstride)
{
    const int w = d->width;
    const size_t plane = (size_t)w * d->height;
    uint16_t *est = s->refine;                   /* current YUV estimate */
    uint16_t *comp2 = s->refine + 3 * plane;     /* recomposite */
    uint16_t *cru = s->refine + 4 * plane;       /* crude Y of recomposite */

    for (int r = 0; r < rows; r++) {
        memcpy(est + r * w, dsty + r * ystride, sizeof(uint16_t) * w);
        memcpy(est + plane + r * w, dstu + r * ustride, sizeof(uint16_t) * w);
        memcpy(est + 2 * plane + r * w, dstv + r * vstride, sizeof(uint16_t) * w);
    }

    for (int it = 0; it < d->refine; it++) {
        for (int r = 0; r < rows; r++)
            comp_encode_line(&d->enc, comp2 + r * w,
                             est + r * w, est + plane + r * w, est + 2 * plane + r * w,
                             comp_sc_line(d->standard, frame, r + row_off));
        crude_decode_frame(d, frame, rows, row_off, 0, comp2, w,
                           cru, w, cru + plane, w, cru + plane, w);
        for (int r = 0; r < rows; r++) {
            const uint16_t *oy = orig_y + r * orig_stride;
            uint16_t *ey = est + r * w;
            const uint16_t *cy = cru + r * w;
            for (int x = 0; x < w; x++)
                ey[x] = clamp_u16((int32_t)ey[x] + (int32_t)oy[x] - (int32_t)cy[x]);
        }
    }

    for (int r = 0; r < rows; r++)
        memcpy(dsty + r * ystride, est + r * w, sizeof(uint16_t) * w);
}

void comp_decode_frame(comp_decode_t *d, int frame, int nframes,
                       int rows, int row_off,
                       const comp_frame_view_t *views, const int *view_frames,
                       int look,
                       const uint16_t *orig_y, ptrdiff_t orig_stride,
                       uint16_t *dsty, ptrdiff_t ystride,
                       uint16_t *dstu, ptrdiff_t ustride,
                       uint16_t *dstv, ptrdiff_t vstride)
{
    comp_decode_scratch_t *s = scratch_acquire(d);
    const int w = d->width;
    const uint16_t *comp = views[look].data;
    const ptrdiff_t comp_stride = views[look].stride;

    if (d->dimensions == 1) {
        crude_decode_frame(d, frame, rows, row_off, 1, comp, comp_stride,
                           dsty, ystride, dstu, ustride, dstv, vstride);
    } else if (d->standard == COMP_STD_PAL) {
        const int frows = rows / 2;

        if (d->dimensions == 2) {
            /* chroma separation per field; field views interleave rows */
            for (int field = 0; field < 2; field++) {
                comp_transform2d_field(&d->transform,
                                       comp + field * comp_stride, 2 * comp_stride,
                                       w, frows,
                                       s->chroma_f + field * w, 2 * w,
                                       s->conf ? s->conf + field * w : NULL, 2 * w);
            }
        } else {
            /* build the field stack the covering 3D tiles need; the
             * views are edge-clamped frames, so out-of-clip fields
             * resolve to the nearest frame's field of the same parity */
            comp_field_view_t fields[COMP_T3D_ZTILE + COMP_T3D_ZTILE / 2];
            const int fout = frame * 2;
            const int tz_hi = ((fout + 1) / (COMP_T3D_ZTILE / 2)) * (COMP_T3D_ZTILE / 2);
            const int z0 = tz_hi - COMP_T3D_ZTILE / 2;
            const int nfields = COMP_T3D_ZTILE + COMP_T3D_ZTILE / 2;

            for (int i = 0; i < nfields; i++) {
                const int g = z0 + i;
                if (g < 0 || g >= 2 * nframes) {
                    /* outside the clip: black, as the reference pads --
                     * repeating real fields would break the temporal
                     * phase sequence the symmetry test relies on */
                    fields[i].data = NULL;
                    fields[i].stride = 0;
                    continue;
                }
                int k = g / 2 - (frame - look);
                k = k < 0 ? 0 : k > 2 * look ? 2 * look : k;
                fields[i].data = views[k].data + (g & 1) * views[k].stride;
                fields[i].stride = 2 * views[k].stride;
            }
            comp_transform3d_frame(&d->transform3, fields, z0, nfields,
                                   frame, 0, w, frows,
                                   s->chroma_f, s->chroma_f + w, 2 * w,
                                   s->conf, s->conf ? s->conf + w : NULL);
        }

        /* quantise the separated chroma once for the fixed-point demod */
        for (int i = 0; i < w * rows; i++)
            s->chroma[i] = clamp_i16(lrintf(s->chroma_f[i]));

        for (int field = 0; field < 2; field++) {
            pal_decode_field(d, frame, field,
                             comp + field * comp_stride, 2 * comp_stride,
                             s->chroma + field * w, 2 * w,
                             s->conf ? s->conf + field * w : NULL,
                             dsty, ystride, dstu, ustride, dstv, vstride);
        }
    } else {
        /* the comb works on frame rows directly: ±2 rows are the same
         * field's neighboring lines */
        if (d->use_transform) {
            comp_field_view_t fields[COMP_T3D_ZTILE + COMP_T3D_ZTILE / 2];
            const int fout = frame * 2;
            const int tz_hi = ((fout + 1) / (COMP_T3D_ZTILE / 2)) * (COMP_T3D_ZTILE / 2);
            const int z0 = tz_hi - COMP_T3D_ZTILE / 2;
            const int nfields = COMP_T3D_ZTILE + COMP_T3D_ZTILE / 2;

            const int parity = (row_off + 1) & 1;
            for (int i = 0; i < nfields; i++) {
                const int g = z0 + i;
                if (g < 0 || g >= 2 * nframes) {
                    fields[i].data = NULL;
                    fields[i].stride = 0;
                    continue;
                }
                int k = g / 2 - (frame - look);
                k = k < 0 ? 0 : k > 2 * look ? 2 * look : k;
                fields[i].data = views[k].data + ((g ^ parity) & 1) * views[k].stride;
                fields[i].stride = 2 * views[k].stride;
            }
            comp_transform3d_frame(&d->transform3, fields, z0, nfields,
                                   frame, parity, w, rows / 2,
                                   s->chroma_f, s->chroma_f + w, 2 * w,
                                   s->conf, s->conf ? s->conf + w : NULL);
            for (int i = 0; i < w * rows; i++)
                s->chroma[i] = clamp_i16(lrintf(s->chroma_f[i]));
        } else if (d->dimensions == 2) {
            ntsc_comb1d(d, rows, comp, comp_stride, s->chroma_f);
            ntsc_comb2d(d, rows, s->chroma_f, s->tmp3);
            for (int i = 0; i < w * rows; i++)
                s->chroma[i] = clamp_i16(lrintf(s->tmp3[i]));
        } else {
            const float *c1[3], *c2[3];
            for (int k = 0; k < 3; k++) {
                float *b1 = s->tmp3 + k * w * d->height;
                float *b2 = s->tmp3 + (3 + k) * w * d->height;
                ntsc_comb1d(d, rows, views[k].data, views[k].stride, b1);
                ntsc_comb2d(d, rows, b1, b2);
                c1[k] = b1;
                c2[k] = b2;
            }
            ntsc_split3d(d, rows, row_off, views, view_frames, c1, c2,
                         s->chroma);
        }
        for (int r = 0; r < rows; r++)
            ntsc_demod_line(d, frame, r + row_off,
                            comp + r * comp_stride, s->chroma + r * w,
                            s->conf ? s->conf + r * w : NULL,
                            dsty + r * ystride, dstu + r * ustride,
                            dstv + r * vstride);
    }

    if (d->refine > 0 && orig_y)
        refine_luma(d, s, frame, rows, row_off, orig_y, orig_stride,
                    dsty, ystride, dstu, ustride, dstv, vstride);

    scratch_release(d, s);
}
