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

int comp_transform2d_init(comp_transform2d_t *t, double threshold, int level)
{
    if (!(threshold > 0.0 && threshold <= 1.0))
        return -1;
    t->level = !!level;
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

/* Keep only bins that look like chroma: modulated chroma is symmetric
 * about the carrier at (fsc, 72 c/aph) = (XTILE/4, YTILE/4). In
 * threshold mode, compare each candidate bin's squared magnitude with
 * its reflection and keep the pair only if they match within the
 * threshold ratio. In level mode, set the larger of the pair to the
 * smaller, phase preserved (GB 2365247 A): true chroma pairs have
 * equal magnitudes and pass unchanged, so only asymmetric (luma)
 * energy is reduced. */
static void apply_filter(const comp_transform2d_t *t,
                         const fftwf_complex *in, fftwf_complex *out)
{
    const float *tsq = t->threshold_sq;

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
                continue;
            }

            const float m_in_sq = bi[x][0] * bi[x][0] + bi[x][1] * bi[x][1];
            const float m_ref_sq = bi_ref[x_ref][0] * bi_ref[x_ref][0]
                                 + bi_ref[x_ref][1] * bi_ref[x_ref][1];

            if (t->level) {
                float f_in = 1.0f, f_ref = 1.0f;
                if (m_in_sq > m_ref_sq)
                    f_in = sqrtf(m_ref_sq / m_in_sq);
                else if (m_ref_sq > m_in_sq)
                    f_ref = sqrtf(m_in_sq / m_ref_sq);
                bo[x][0] = bi[x][0] * f_in;
                bo[x][1] = bi[x][1] * f_in;
                bo_ref[x_ref][0] = bi_ref[x_ref][0] * f_ref;
                bo_ref[x_ref][1] = bi_ref[x_ref][1] * f_ref;
                continue;
            }

            if (m_in_sq < m_ref_sq * threshold_sq ||
                m_ref_sq < m_in_sq * threshold_sq)
                continue;  /* asymmetric: probably not chroma */

            bo[x][0] = bi[x][0];
            bo[x][1] = bi[x][1];
            bo_ref[x_ref][0] = bi_ref[x_ref][0];
            bo_ref[x_ref][1] = bi_ref[x_ref][1];
        }
    }
}

void comp_transform2d_field(const comp_transform2d_t *t,
                            const uint16_t *comp, ptrdiff_t comp_stride,
                            int width, int rows,
                            float *chroma, ptrdiff_t chroma_stride)
{
    float real[YTILE * XTILE];
    fftwf_complex cplx_in[YCOMPLEX * XCOMPLEX];
    fftwf_complex cplx_out[YCOMPLEX * XCOMPLEX];

    for (int r = 0; r < rows; r++)
        memset(chroma + r * chroma_stride, 0, sizeof(float) * width);

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

            apply_filter(t, cplx_in, cplx_out);

            /* inverse FFT; overlap-add the active part, normalized */
            fftwf_execute_dft_c2r(t->inverse, cplx_out, real);
            for (int y = start_y; y < end_y; y++) {
                float *b = chroma + (tile_y + y) * chroma_stride;
                for (int x = start_x; x < end_x; x++)
                    b[tile_x + x] += real[y * XTILE + x] / (YTILE * XTILE);
            }
        }
    }
}
