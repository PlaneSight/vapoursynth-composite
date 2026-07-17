#include <stdint.h>
#include <string.h>

#include <checkasm/checkasm.h>
#include <checkasm/test.h>
#include <checkasm/utils.h>

#include "subcarrier.h"
#include "transform3d.h"

#define NROWS COMP_T3D_NROWS
#define NBINS COMP_T3D_NTHRESH
#define TILEF (COMP_T3D_ZTILE * COMP_T3D_YTILE * COMP_T3D_XCOMPLEX * 2)

static CHECKASM_ALIGN(float tile[TILEF]);
static CHECKASM_ALIGN(float gains[NBINS + COMP_T3D_BINPAD]);
static CHECKASM_ALIGN(int32_t rows[NROWS][2]);
static CHECKASM_ALIGN(float mi_c[NBINS + COMP_T3D_BINPAD]);
static CHECKASM_ALIGN(float mi_a[NBINS + COMP_T3D_BINPAD]);
static CHECKASM_ALIGN(float mr_c[NBINS + COMP_T3D_BINPAD]);
static CHECKASM_ALIGN(float mr_a[NBINS + COMP_T3D_BINPAD]);
static CHECKASM_ALIGN(float out_c[TILEF]);
static CHECKASM_ALIGN(float out_a[TILEF]);

static const int standards[] = { COMP_STD_PAL, COMP_STD_NTSC };

static float randf(void)
{
    return (float)(checkasm_rand() & 0xFFFFFF) / 0x1000000;
}

static void fill_inputs(void)
{
    for (int i = 0; i < TILEF; i++)
        tile[i] = (randf() - 0.5f) * 2e5f;
    for (int i = 0; i < NBINS + COMP_T3D_BINPAD; i++)
        gains[i] = randf();
}

void checkasm_test_t3d_filter(void)
{
    {
        checkasm_declare(void, float *, float *, const float *,
                         const int32_t (*)[2], int);
        if (checkasm_check_func(comp_get_t3d_mag_fn(checkasm_get_cpu_flags()),
                                "t3d_mag")) {
            for (int s = 0; s < 2; s++) {
                const int pal = standards[s] == COMP_STD_PAL;
                const int nrows = pal ? NROWS / 2 : NROWS;
                comp_t3d_rowpair(standards[s], rows);
                fill_inputs();
                memset(mi_c, 0xaa, sizeof(mi_c));
                memset(mi_a, 0xaa, sizeof(mi_a));
                memset(mr_c, 0x55, sizeof(mr_c));
                memset(mr_a, 0x55, sizeof(mr_a));
                checkasm_call_ref(mi_c, mr_c, tile, rows, nrows);
                checkasm_call_new(mi_a, mr_a, tile, rows, nrows);
                /* the fma3 tier single-rounds the im^2 accumulation —
                 * one rounding fewer than the unfused C — so the
                 * magnitudes carry a small ULP budget (they are sums
                 * of squares: no cancellation to magnify it) */
                for (int j = 0; j < 3 * nrows; j++) {
                    if (!checkasm_float_near_abs_eps_ulp(mi_c[j], mi_a[j],
                                                         1e-6f, 4)) {
                        checkasm_fail_func("m_in[%d]: %.9g vs %.9g",
                                           j, mi_c[j], mi_a[j]);
                        break;
                    }
                    if (!checkasm_float_near_abs_eps_ulp(mr_c[j], mr_a[j],
                                                         1e-6f, 4)) {
                        checkasm_fail_func("m_ref[%d]: %.9g vs %.9g",
                                           j, mr_c[j], mr_a[j]);
                        break;
                    }
                }
            }
            checkasm_bench_new(mi_a, mr_a, tile, rows, NROWS);
        }
    }
    {
        checkasm_declare(void, float *, const float *, const float *,
                         const int32_t (*)[2], int);
        if (checkasm_check_func(comp_get_t3d_apply_fn(checkasm_get_cpu_flags()),
                                "t3d_apply")) {
            for (int s = 0; s < 2; s++) {
                const int pal = standards[s] == COMP_STD_PAL;
                const int nrows = pal ? NROWS / 2 : NROWS;
                comp_t3d_rowpair(standards[s], rows);
                fill_inputs();
                memset(out_c, 0, sizeof(out_c));
                memset(out_a, 0, sizeof(out_a));
                checkasm_call_ref(out_c, tile, gains, rows, nrows);
                checkasm_call_new(out_a, tile, gains, rows, nrows);
                /* whole tile: untouched positions must stay zero too */
                checkasm_check1d(uint32_t, (const uint32_t *)out_c,
                                 (const uint32_t *)out_a, TILEF, "out");
            }
            checkasm_bench_new(out_a, tile, gains, rows, NROWS);
        }
    }
    checkasm_report("t3d_filter");
}
