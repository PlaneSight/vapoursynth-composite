/*
 * Transform PAL 2D: single-precision implementation vs a double-precision
 * reference that mirrors ld-chroma-decoder's TransformPal2D exactly.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fftw3.h>

#include "encode.h"
#include "transform2d.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define W COMP_ACTIVE_WIDTH_PAL
#define ROWS (COMP_ACTIVE_HEIGHT_PAL / 2)
#define XTILE COMP_T2D_XTILE
#define YTILE COMP_T2D_YTILE
#define XC COMP_T2D_XCOMPLEX

static int fail = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        fail = 1; \
    } \
} while (0)

/* double-precision reference, same tiling/window/filter as the port */
static void ref_field(const uint16_t *comp, double threshold, double *chroma)
{
    static double window[YTILE][XTILE];
    for (int y = 0; y < YTILE; y++)
        for (int x = 0; x < XTILE; x++)
            window[y][x] = (0.5 - 0.5 * cos(2.0 * M_PI * (y + 0.5) / YTILE))
                         * (0.5 - 0.5 * cos(2.0 * M_PI * (x + 0.5) / XTILE));

    double *real = fftw_alloc_real(YTILE * XTILE);
    fftw_complex *cin = fftw_alloc_complex(YTILE * XC);
    fftw_complex *cout = fftw_alloc_complex(YTILE * XC);
    fftw_plan fwd = fftw_plan_dft_r2c_2d(YTILE, XTILE, real, cin, FFTW_ESTIMATE);
    fftw_plan inv = fftw_plan_dft_c2r_2d(YTILE, XTILE, cout, real, FFTW_ESTIMATE);
    const double tsq = threshold * threshold;

    memset(chroma, 0, sizeof(double) * W * ROWS);

    for (int ty = -YTILE / 2; ty < ROWS; ty += YTILE / 2) {
        const int sy = ty < 0 ? -ty : 0;
        const int ey = ROWS - ty < YTILE ? ROWS - ty : YTILE;
        for (int tx = -XTILE / 2; tx < W; tx += XTILE / 2) {
            const int sx = tx < 0 ? -tx : 0;
            const int ex = W - tx < XTILE ? W - tx : XTILE;

            for (int y = 0; y < YTILE; y++)
                for (int x = 0; x < XTILE; x++) {
                    const int valid = y >= sy && y < ey && x >= sx && x < ex;
                    const double v = valid ? comp[(ty + y) * W + tx + x] : 16384.0;
                    real[y * XTILE + x] = v * window[y][x];
                }
            fftw_execute(fwd);

            memset(cout, 0, sizeof(fftw_complex) * YTILE * XC);
            for (int y = 0; y < YTILE; y++) {
                const int y_ref = ((YTILE / 2) + YTILE - y) % YTILE;
                for (int x = XTILE / 8; x <= XTILE / 4; x++) {
                    const int x_ref = (XTILE / 2) - x;
                    const fftw_complex *iv = &cin[y * XC + x];
                    const fftw_complex *rv = &cin[y_ref * XC + x_ref];
                    if (x == x_ref && y == y_ref) {
                        cout[y * XC + x][0] = (*iv)[0];
                        cout[y * XC + x][1] = (*iv)[1];
                        continue;
                    }
                    const double mi = (*iv)[0] * (*iv)[0] + (*iv)[1] * (*iv)[1];
                    const double mr = (*rv)[0] * (*rv)[0] + (*rv)[1] * (*rv)[1];
                    if (mi < mr * tsq || mr < mi * tsq)
                        continue;
                    cout[y * XC + x][0] = (*iv)[0];
                    cout[y * XC + x][1] = (*iv)[1];
                    cout[y_ref * XC + x_ref][0] = (*rv)[0];
                    cout[y_ref * XC + x_ref][1] = (*rv)[1];
                }
            }

            fftw_execute(inv);
            for (int y = sy; y < ey; y++)
                for (int x = sx; x < ex; x++)
                    chroma[(ty + y) * W + tx + x] += real[y * XTILE + x] / (YTILE * XTILE);
        }
    }

    fftw_destroy_plan(fwd);
    fftw_destroy_plan(inv);
    fftw_free(real);
    fftw_free(cin);
    fftw_free(cout);
}

static uint32_t lcg_state = 777;

static uint32_t lcg(void)
{
    lcg_state = lcg_state * 1664525 + 1013904223;
    return lcg_state >> 8;
}

int main(void)
{
    static uint16_t comp[W * ROWS];
    static float got[W * ROWS];
    static double want[W * ROWS];

    comp_transform2d_t t2d;
    CHECK(comp_transform2d_init(&t2d, 0.4, 0) == 0, "init");
    CHECK(comp_transform2d_init(&t2d, 0.0, 0) != 0, "zero threshold must be rejected");
    CHECK(comp_transform2d_init(&t2d, 0.4, 0) == 0, "re-init");

    /* realistic input: an encoded field of color, plus noise */
    comp_encode_t enc;
    comp_encode_init(&enc, COMP_STD_PAL, 0, 0);
    for (int fr = 0; fr < ROWS; fr++) {
        uint16_t y[W], u[W], v[W];
        for (int x = 0; x < W; x++) {
            y[x] = (uint16_t)(20000 + 20000.0 * sin(x * 0.013) + (lcg() % 2048));
            u[x] = (uint16_t)(32768 + 12000.0 * sin(x * 0.005 + fr * 0.02));
            v[x] = (uint16_t)(32768 - 9000.0 * cos(x * 0.004));
        }
        comp_encode_line(&enc, &comp[fr * W], y, u, v,
                         comp_sc_line(COMP_STD_PAL, 0, fr * 2));
    }

    comp_transform2d_field(&t2d, comp, W, W, ROWS, got, W);
    ref_field(comp, 0.4, want);

    double max_diff = 0.0;
    for (int i = 0; i < W * ROWS; i++) {
        const double diff = fabs(want[i] - got[i]);
        if (diff > max_diff)
            max_diff = diff;
    }
    /* float vs double across one FFT round trip of ~16-bit data */
    CHECK(max_diff <= 0.5, "float deviates from double reference by %f", max_diff);

    comp_transform2d_free(&t2d);

    if (!fail)
        printf("test_transform2d: all tests passed (max diff %f)\n", max_diff);
    return fail;
}
