#include <stdint.h>
#include <string.h>

#include <checkasm/checkasm.h>
#include <checkasm/test.h>
#include <checkasm/utils.h>

#include "transform3d.h"

#define MAXBINS COMP_T3D_NTHRESH

static CHECKASM_ALIGN(float m_in[MAXBINS]);
static CHECKASM_ALIGN(float m_ref[MAXBINS]);
static CHECKASM_ALIGN(float lut[MAXBINS][COMP_LUT_K]);
static CHECKASM_ALIGN(float g_c[MAXBINS]), g_a[MAXBINS];
static CHECKASM_ALIGN(float r_c[MAXBINS]), r_a[MAXBINS];

/* the production bin counts (2D PAL, 3D PAL, 3D NTSC) plus the minimum */
static const int nbins[] = { 16, 80, COMP_T3D_NTHRESH_PAL, COMP_T3D_NTHRESH };
#define NN (int)(sizeof(nbins) / sizeof(nbins[0]))

static float randf(void)
{
    return (float)(checkasm_rand() & 0xFFFFFF) / 0x1000000;
}

static void fill_inputs(void)
{
    for (int i = 0; i < MAXBINS; i++) {
        /* tile magnitudes; force zero and equal pairs so the r = 1
         * lanes and the knot clamp are exercised */
        m_in[i] = randf() * 1e8f;
        m_ref[i] = randf() * 1e8f;
        if ((checkasm_rand() & 7) == 0)
            m_in[i] = m_ref[i] = 0.0f;
        else if ((checkasm_rand() & 7) == 1)
            m_ref[i] = m_in[i];
        for (int k = 0; k < COMP_LUT_K; k++)
            lut[i][k] = randf();
    }
}

void checkasm_test_lut_gain(void)
{
    checkasm_declare(void, float *, float *, const float *, const float *,
                     const float (*)[COMP_LUT_K], int);
    if (checkasm_check_func(comp_get_lut_gain_fn(checkasm_get_cpu_flags()),
                            "lut_gain")) {
        for (int i = 0; i < NN; i++) {
            fill_inputs();
            memset(g_c, 0xaa, sizeof(g_c));
            memset(g_a, 0x55, sizeof(g_a));
            memset(r_c, 0xaa, sizeof(r_c));
            memset(r_a, 0x55, sizeof(r_a));
            checkasm_call_ref(g_c, r_c, m_in, m_ref, lut, nbins[i]);
            checkasm_call_new(g_a, r_a, m_in, m_ref, lut, nbins[i]);
            /* the fma3 tier fuses the interpolation's mul+add — one
             * rounding fewer than the unfused C — so g carries a small
             * ULP budget; r has no fused path and stays bitwise */
            for (int j = 0; j < nbins[i]; j++)
                if (!checkasm_float_near_abs_eps_ulp(g_c[j], g_a[j], 1e-7f, 4)) {
                    checkasm_fail_func("g[%d]: %.9g vs %.9g", j, g_c[j], g_a[j]);
                    break;
                }
            checkasm_check1d(uint32_t, (const uint32_t *)r_c,
                             (const uint32_t *)r_a, nbins[i], "r");
        }
        fill_inputs();
        checkasm_bench_new(g_a, r_a, m_in, m_ref, lut, COMP_T3D_NTHRESH);
    }
    checkasm_report("lut_gain");
}
