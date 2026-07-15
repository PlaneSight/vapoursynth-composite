/*
 * 3D Transform PAL chroma separation.
 *
 * Port of ld-chroma-decoder's TransformPal3D: the 2D tile filter gains
 * a temporal axis. Tiles are 16 samples by 32 frame lines by 8 fields,
 * half-overlapped in every axis; rows belonging to the other field are
 * filled with black, so the FFT sees the interlaced lattice directly.
 * Bins are kept only if symmetric with their reflection about the
 * chroma carrier at (fsc, 72 c/aph, 18.75 Hz). The reference reflects
 * the temporal axis with offset ZTILE/4 although its own comment says
 * 3*ZTILE/4; the shipped behaviour is ported unchanged.
 */

#include <math.h>
#include <pthread.h>
#include <string.h>

#include "transform3d.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define XTILE COMP_T3D_XTILE
#define YTILE COMP_T3D_YTILE
#define ZTILE COMP_T3D_ZTILE
#define XC COMP_T3D_XCOMPLEX
#define TILE_REAL (ZTILE * YTILE * XTILE)
#define TILE_CPLX (ZTILE * YTILE * XC)

#define LEVEL_BLACK 16384.0f

/* FFTW's planner is not thread-safe; execute-with-new-arrays is */
static pthread_mutex_t planner_lock = PTHREAD_MUTEX_INITIALIZER;

static double compute_window(int element, int limit)
{
    return 0.5 - 0.5 * cos((2.0 * M_PI * (element + 0.5)) / limit);
}

int comp_transform3d_init(comp_transform3d_t *t, double threshold)
{
    if (!(threshold > 0.0 && threshold <= 1.0))
        return -1;
    t->threshold_sq = (float)(threshold * threshold);

    for (int z = 0; z < ZTILE; z++)
        for (int y = 0; y < YTILE; y++)
            for (int x = 0; x < XTILE; x++)
                t->window[z][y][x] = (float)(compute_window(z, ZTILE)
                                             * compute_window(y, YTILE)
                                             * compute_window(x, XTILE));

    float real[TILE_REAL];
    fftwf_complex cplx[TILE_CPLX];
    pthread_mutex_lock(&planner_lock);
    t->forward = fftwf_plan_dft_r2c_3d(ZTILE, YTILE, XTILE, real, cplx,
                                       FFTW_ESTIMATE | FFTW_UNALIGNED);
    t->inverse = fftwf_plan_dft_c2r_3d(ZTILE, YTILE, XTILE, cplx, real,
                                       FFTW_ESTIMATE | FFTW_UNALIGNED);
    pthread_mutex_unlock(&planner_lock);
    if (!t->forward || !t->inverse) {
        comp_transform3d_free(t);
        return -1;
    }
    return 0;
}

void comp_transform3d_free(comp_transform3d_t *t)
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

static void apply_filter(const comp_transform3d_t *t,
                         const fftwf_complex *in, fftwf_complex *out)
{
    memset(out, 0, sizeof(fftwf_complex) * TILE_CPLX);

    for (int z = 0; z < ZTILE; z++) {
        const int z_ref = ((ZTILE / 4) + ZTILE - z) % ZTILE;
        for (int y = 0; y < YTILE; y++) {
            const int y_ref = ((YTILE / 4) + YTILE - y) % YTILE;
            const fftwf_complex *bi = in + (z * YTILE + y) * XC;
            const fftwf_complex *bi_ref = in + (z_ref * YTILE + y_ref) * XC;
            fftwf_complex *bo = out + (z * YTILE + y) * XC;
            fftwf_complex *bo_ref = out + (z_ref * YTILE + y_ref) * XC;

            for (int x = XTILE / 8; x <= XTILE / 4; x++) {
                const int x_ref = (XTILE / 2) - x;

                if (x == x_ref && y == y_ref && z == z_ref) {
                    bo[x][0] = bi[x][0];
                    bo[x][1] = bi[x][1];
                    continue;
                }

                const float m_in_sq = bi[x][0] * bi[x][0] + bi[x][1] * bi[x][1];
                const float m_ref_sq = bi_ref[x_ref][0] * bi_ref[x_ref][0]
                                     + bi_ref[x_ref][1] * bi_ref[x_ref][1];

                if (m_in_sq < m_ref_sq * t->threshold_sq ||
                    m_ref_sq < m_in_sq * t->threshold_sq)
                    continue;

                bo[x][0] = bi[x][0];
                bo[x][1] = bi[x][1];
                bo_ref[x_ref][0] = bi_ref[x_ref][0];
                bo_ref[x_ref][1] = bi_ref[x_ref][1];
            }
        }
    }
}

void comp_transform3d_frame(const comp_transform3d_t *t,
                            const comp_field_view_t *fields, int z0, int nfields,
                            int frame, int width, int field_rows,
                            float *chroma0, float *chroma1, ptrdiff_t chroma_stride)
{
    float real[TILE_REAL];
    fftwf_complex cplx_in[TILE_CPLX];
    fftwf_complex cplx_out[TILE_CPLX];
    float *chroma[2] = { chroma0, chroma1 };
    const int fout = frame * 2;
    const int frame_lines = field_rows * 2;

    for (int f = 0; f < 2; f++)
        for (int r = 0; r < field_rows; r++)
            memset(chroma[f] + r * chroma_stride, 0, sizeof(float) * width);

    /* the two half-overlapped z positions whose tiles cover this frame's
     * fields; the grid is anchored at absolute field 0 */
    const int tz_hi = ((fout + 1) / (ZTILE / 2)) * (ZTILE / 2);

    for (int tz = tz_hi - ZTILE / 2; tz <= tz_hi; tz += ZTILE / 2) {
        for (int tile_y = -YTILE / 2; tile_y < frame_lines; tile_y += YTILE / 2) {
            const int start_y = tile_y < 0 ? -tile_y : 0;
            const int end_y = frame_lines - tile_y < YTILE ? frame_lines - tile_y : YTILE;

            for (int tile_x = -XTILE / 2; tile_x < width; tile_x += XTILE / 2) {
                const int start_x = tile_x < 0 ? -tile_x : 0;
                const int end_x = width - tile_x < XTILE ? width - tile_x : XTILE;

                /* windowed forward FFT: a row is real data only when it
                 * is inside the picture and belongs to field tz+z */
                for (int z = 0; z < ZTILE; z++) {
                    const int g = tz + z;
                    const comp_field_view_t *fv =
                        (g >= z0 && g < z0 + nfields) ? &fields[g - z0] : NULL;
                    if (fv && !fv->data)
                        fv = NULL;
                    for (int y = 0; y < YTILE; y++) {
                        const int fl = tile_y + y;
                        const int usable = fv && y >= start_y && y < end_y
                                           && (fl & 1) == (g & 1);
                        const uint16_t *b = usable
                            ? fv->data + (fl >> 1) * fv->stride : NULL;
                        float *dst = &real[(z * YTILE + y) * XTILE];
                        for (int x = 0; x < XTILE; x++) {
                            const float v = (!usable || x < start_x || x >= end_x)
                                            ? LEVEL_BLACK : (float)b[tile_x + x];
                            dst[x] = v * t->window[z][y][x];
                        }
                    }
                }
                fftwf_execute_dft_r2c(t->forward, real, cplx_in);

                apply_filter(t, cplx_in, cplx_out);

                fftwf_execute_dft_c2r(t->inverse, cplx_out, real);

                /* overlap-add only the parts landing on our two fields */
                for (int z = 0; z < ZTILE; z++) {
                    const int g = tz + z;
                    if (g != fout && g != fout + 1)
                        continue;
                    float *cb = chroma[g - fout];
                    for (int y = start_y; y < end_y; y++) {
                        const int fl = tile_y + y;
                        if ((fl & 1) != (g & 1))
                            continue;
                        float *b = cb + (fl >> 1) * chroma_stride;
                        for (int x = start_x; x < end_x; x++)
                            b[tile_x + x] += real[(z * YTILE + y) * XTILE + x]
                                             / (ZTILE * YTILE * XTILE);
                    }
                }
            }
        }
    }
}
