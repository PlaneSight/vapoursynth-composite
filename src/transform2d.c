/*
 * 2D Transform PAL chroma separation.
 *
 * Port of ld-chroma-decoder's TransformPal2D, itself based on Jim
 * Easterbrook's pyctools-pal (http://www.jim-easterbrook.me.uk/pal/):
 * windowed overlapping tiles are moved to the frequency domain, where
 * bins are kept only if they are symmetric with their reflection about
 * the chroma carrier, then overlap-added back. Runs in single-precision
 * float; everything downstream is fixed point.
 */

#include <math.h>
#include <pthread.h>
#include <string.h>

#include "transform2d.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define XTILE COMP_T2D_XTILE
#define YTILE COMP_T2D_YTILE
#define XCOMPLEX COMP_T2D_XCOMPLEX
#define YCOMPLEX COMP_T2D_YCOMPLEX

/* level padding tiles see beyond the raster edges: black, 0x4000 (the
 * raster carries no blanking/burst samples, unlike a full stored line) */
#define LEVEL_BLACK 16384.0f

/* FFTW's planner is not thread-safe; execute-with-new-arrays is */
static pthread_mutex_t planner_lock = PTHREAD_MUTEX_INITIALIZER;

static double compute_window(int element, int limit)
{
    return 0.5 - 0.5 * cos((2.0 * M_PI * (element + 0.5)) / limit);
}

int comp_transform2d_init(comp_transform2d_t *t, double threshold, int level,
                          double evidence)
{
    if (!(threshold > 0.0 && threshold <= 1.0) || evidence < 0.0)
        return -1;
    t->level = !!level;
    t->use_lut = 0;
    t->evidence = (float)evidence;
    for (int i = 0; i < COMP_T2D_NTHRESH; i++)
        t->threshold_sq[i] = (float)(threshold * threshold);

    for (int y = 0; y < YTILE; y++)
        for (int x = 0; x < XTILE; x++)
            t->window[y][x] = (float)(compute_window(y, YTILE) * compute_window(x, XTILE));

    /* FFTW_UNALIGNED lets per-call tile buffers live on the stack, which
     * keeps frame processing allocation-free under fmParallel */
    float real[YTILE * XTILE];
    fftwf_complex cplx[YCOMPLEX * XCOMPLEX];
    pthread_mutex_lock(&planner_lock);
    t->forward = fftwf_plan_dft_r2c_2d(YTILE, XTILE, real, cplx,
                                       FFTW_ESTIMATE | FFTW_UNALIGNED);
    t->inverse = fftwf_plan_dft_c2r_2d(YTILE, XTILE, cplx, real,
                                       FFTW_ESTIMATE | FFTW_UNALIGNED);
    pthread_mutex_unlock(&planner_lock);
    if (!t->forward || !t->inverse) {
        comp_transform2d_free(t);
        return -1;
    }
    return 0;
}

void comp_transform2d_set_lut(comp_transform2d_t *t, const double *v)
{
    for (int b = 0; b < COMP_T2D_NTHRESH; b++)
        for (int k = 0; k < COMP_LUT_K; k++)
            t->lut[b][k] = (float)v[b * COMP_LUT_K + k];
    t->use_lut = 1;
}

void comp_transform2d_free(comp_transform2d_t *t)
{
    pthread_mutex_lock(&planner_lock);
    if (t->forward)
        fftwf_destroy_plan(t->forward);
    if (t->inverse)
        fftwf_destroy_plan(t->inverse);
    pthread_mutex_unlock(&planner_lock);
    t->forward = NULL;
    t->inverse = NULL;
}

/* gain from a per-bin LUT row: linear interpolation over the
 * pair-symmetry ratio, knots uniform on [0, 1] */
static inline float lut_gain(const float *row, float lo, float hi)
{
    const float r = hi > 0.0f ? lo / hi : 1.0f;
    const float pos = r * (COMP_LUT_K - 1);
    int k = (int)pos;
    if (k > COMP_LUT_K - 2)
        k = COMP_LUT_K - 2;
    return row[k] + (row[k + 1] - row[k]) * (pos - k);
}

/* Keep only bins that look like chroma: modulated chroma is symmetric
 * about the carrier at (fsc, 72 c/aph) = (XTILE/4, YTILE/4). In
 * threshold mode, compare each candidate bin's squared magnitude with
 * its reflection and keep the pair only if they match within the
 * threshold ratio. In level mode, set the larger of the pair to the
 * smaller, phase preserved (GB 2365247 A): true chroma pairs have
 * equal magnitudes and pass unchanged, so only asymmetric (luma)
 * energy is reduced. With a trained LUT, each pair gets a per-bin
 * gain looked up from its symmetry ratio (US 7,872,689).
 *
 * Returns the tile's chroma confidence: the kept output energy's mean
 * pair-symmetry ratio, 0 when nothing was kept. */
static float apply_filter(const comp_transform2d_t *t,
                          const fftwf_complex *in, fftwf_complex *out)
{
    const float *tsq = t->threshold_sq;
    float conf_num = 0.0f, conf_den = 0.0f;

    memset(out, 0, sizeof(fftwf_complex) * YCOMPLEX * XCOMPLEX);

    for (int y = 0; y < YTILE; y++) {
        const int y_ref = ((YTILE / 2) + YTILE - y) % YTILE;
        const fftwf_complex *bi = in + y * XCOMPLEX;
        const fftwf_complex *bi_ref = in + y_ref * XCOMPLEX;
        fftwf_complex *bo = out + y * XCOMPLEX;
        fftwf_complex *bo_ref = out + y_ref * XCOMPLEX;

        /* horizontal frequencies that might be chroma: 0.5fsc to 1.5fsc */
        for (int x = XTILE / 8; x <= XTILE / 4; x++) {
            const int x_ref = (XTILE / 2) - x;
            const float threshold_sq = *tsq++;

            if (x == x_ref && y == y_ref) {
                /* the bin is its own reflection: it is a carrier */
                bo[x][0] = bi[x][0];
                bo[x][1] = bi[x][1];
                conf_num += bi[x][0] * bi[x][0] + bi[x][1] * bi[x][1];
                conf_den += bi[x][0] * bi[x][0] + bi[x][1] * bi[x][1];
                continue;
            }

            const float m_in_sq = bi[x][0] * bi[x][0] + bi[x][1] * bi[x][1];
            const float m_ref_sq = bi_ref[x_ref][0] * bi_ref[x_ref][0]
                                 + bi_ref[x_ref][1] * bi_ref[x_ref][1];
            const float lo = m_in_sq < m_ref_sq ? m_in_sq : m_ref_sq;
            const float hi = m_in_sq < m_ref_sq ? m_ref_sq : m_in_sq;
            const float r = hi > 0.0f ? lo / hi : 1.0f;

            /* LF-luma evidence at the pair's baseband difference
             * frequency k - c (both members map to one stored bin by
             * conjugate symmetry); no LF partner marks cross-color.
             * U and V carriers are the two fixed points of the pair
             * reflection (y = YTILE/4 and 3*YTILE/4), so take the
             * stronger of the two difference positions. */
            float g_e = 1.0f;
            if (t->evidence > 0.0f) {
                const fftwf_complex *lf1 =
                    in + (((YTILE / 4) - y + YTILE) % YTILE) * XCOMPLEX
                       + (XTILE / 4) - x;
                const fftwf_complex *lf2 =
                    in + ((3 * (YTILE / 4) - y + YTILE) % YTILE) * XCOMPLEX
                       + (XTILE / 4) - x;
                const float e1 = (*lf1)[0] * (*lf1)[0] + (*lf1)[1] * (*lf1)[1];
                const float e2 = (*lf2)[0] * (*lf2)[0] + (*lf2)[1] * (*lf2)[1];
                const float e = e1 > e2 ? e1 : e2;
                const float den = e + t->evidence * hi;
                if (den > 0.0f)
                    g_e = e / den;
            }

            if (t->use_lut) {
                const int bin = (int)(tsq - t->threshold_sq) - 1;
                const float g = lut_gain(t->lut[bin], lo, hi) * g_e;
                bo[x][0] = bi[x][0] * g;
                bo[x][1] = bi[x][1] * g;
                bo_ref[x_ref][0] = bi_ref[x_ref][0] * g;
                bo_ref[x_ref][1] = bi_ref[x_ref][1] * g;
                const float e = g * g * (m_in_sq + m_ref_sq);
                conf_num += e * r;
                conf_den += e;
                continue;
            }

            if (t->level) {
                float f_in = g_e, f_ref = g_e;
                if (m_in_sq > m_ref_sq)
                    f_in *= sqrtf(m_ref_sq / m_in_sq);
                else if (m_ref_sq > m_in_sq)
                    f_ref *= sqrtf(m_in_sq / m_ref_sq);
                bo[x][0] = bi[x][0] * f_in;
                bo[x][1] = bi[x][1] * f_in;
                bo_ref[x_ref][0] = bi_ref[x_ref][0] * f_ref;
                bo_ref[x_ref][1] = bi_ref[x_ref][1] * f_ref;
                conf_num += 2.0f * lo * g_e * g_e * r;
                conf_den += 2.0f * lo * g_e * g_e;
                continue;
            }

            if (m_in_sq < m_ref_sq * threshold_sq ||
                m_ref_sq < m_in_sq * threshold_sq)
                continue;  /* asymmetric: probably not chroma */

            bo[x][0] = bi[x][0] * g_e;
            bo[x][1] = bi[x][1] * g_e;
            bo_ref[x_ref][0] = bi_ref[x_ref][0] * g_e;
            bo_ref[x_ref][1] = bi_ref[x_ref][1] * g_e;
            conf_num += (m_in_sq + m_ref_sq) * g_e * g_e * r;
            conf_den += (m_in_sq + m_ref_sq) * g_e * g_e;
        }
    }

    return conf_den > 0.0f ? conf_num / conf_den : 0.0f;
}

void comp_transform2d_field(const comp_transform2d_t *t,
                            const uint16_t *comp, ptrdiff_t comp_stride,
                            int width, int rows,
                            float *chroma, ptrdiff_t chroma_stride,
                            float *conf, ptrdiff_t conf_stride)
{
    float real[YTILE * XTILE];
    fftwf_complex cplx_in[YCOMPLEX * XCOMPLEX];
    fftwf_complex cplx_out[YCOMPLEX * XCOMPLEX];

    for (int r = 0; r < rows; r++) {
        memset(chroma + r * chroma_stride, 0, sizeof(float) * width);
        if (conf)
            memset(conf + r * conf_stride, 0, sizeof(float) * width);
    }

    for (int tile_y = -YTILE / 2; tile_y < rows; tile_y += YTILE / 2) {
        const int start_y = tile_y < 0 ? -tile_y : 0;
        const int end_y = rows - tile_y < YTILE ? rows - tile_y : YTILE;

        for (int tile_x = -XTILE / 2; tile_x < width; tile_x += XTILE / 2) {
            const int start_x = tile_x < 0 ? -tile_x : 0;
            const int end_x = width - tile_x < XTILE ? width - tile_x : XTILE;

            /* windowed forward FFT; samples beyond the raster are black */
            for (int y = 0; y < YTILE; y++) {
                const int row_valid = y >= start_y && y < end_y;
                const uint16_t *b = row_valid ? comp + (tile_y + y) * comp_stride : NULL;
                for (int x = 0; x < XTILE; x++) {
                    const float v = (!row_valid || x < start_x || x >= end_x)
                                    ? LEVEL_BLACK : (float)b[tile_x + x];
                    real[y * XTILE + x] = v * t->window[y][x];
                }
            }
            fftwf_execute_dft_r2c(t->forward, real, cplx_in);

            const float w = apply_filter(t, cplx_in, cplx_out);

            /* inverse FFT; overlap-add the active part, normalized */
            fftwf_execute_dft_c2r(t->inverse, cplx_out, real);
            for (int y = start_y; y < end_y; y++) {
                float *b = chroma + (tile_y + y) * chroma_stride;
                for (int x = start_x; x < end_x; x++)
                    b[tile_x + x] += real[y * XTILE + x] / (YTILE * XTILE);
            }

            /* the half-overlapped windows sum to unity, so accumulating
             * confidence with the same weights yields a [0,1] map */
            if (conf) {
                for (int y = start_y; y < end_y; y++) {
                    float *b = conf + (tile_y + y) * conf_stride;
                    for (int x = start_x; x < end_x; x++)
                        b[tile_x + x] += w * t->window[y][x];
                }
            }
        }
    }
}
