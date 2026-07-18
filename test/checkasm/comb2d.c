#include <stdint.h>
#include <string.h>

#include <checkasm/checkasm.h>
#include <checkasm/test.h>
#include <checkasm/utils.h>

#include "decode.h"

#define MAXW 758

static CHECKASM_ALIGN(float cur[MAXW]);
static CHECKASM_ALIGN(float prev[MAXW]);
static CHECKASM_ALIGN(float next[MAXW]);
static CHECKASM_ALIGN(float out_c[MAXW]);
static CHECKASM_ALIGN(float out_a[MAXW]);

/* the driver runs the interior on 758 (NTSC) and never anything else;
 * the smaller widths exercise the overlapping-final-vector tail */
static const int widths[] = { 20, 37, 122, 359, 758 };
#define NWIDTHS (int)(sizeof(widths) / sizeof(widths[0]))

/* chroma-scale values, matching split3d's generator */
static float chroma_val(void)
{
    return (float)((int32_t)(checkasm_rand() % 65535) - 32767)
         * (1.0f / 16.0f) * (float)(checkasm_rand() % 16);
}

/* mode 0: independent rows (mostly inactive / large penalties)
 * mode 1: prev/next near cur, driving the coefficients toward the
 *         active branches and the 3x zeroing and sc clamps
 * mode 2: prev == next region, exercising the inactive fallback */
static void fill_inputs(int mode)
{
    for (int x = 0; x < MAXW; x++) {
        const float c = chroma_val();
        cur[x] = c;
        if (mode == 1) {
            const float jp = (float)((int)(checkasm_rand() % 64) - 32);
            const float jn = (float)((int)(checkasm_rand() % 64) - 32);
            prev[x] = c + jp;
            next[x] = c + jn;
        } else if (mode == 2) {
            const float base = chroma_val();
            prev[x] = base;
            next[x] = base;   /* |prev|==|next| feeds the fallback test */
        } else {
            prev[x] = chroma_val();
            next[x] = chroma_val();
        }
    }
}

void checkasm_test_comb2d(void)
{
    checkasm_declare(void, float *, const float *, const float *,
                     const float *, const float *, int);
    if (checkasm_check_func(comp_get_ntsc_comb2d_row_fn(checkasm_get_cpu_flags()),
                            "ntsc_comb2d_row")) {
        for (int i = 0; i < 3 * NWIDTHS; i++) {
            const int w = widths[i % NWIDTHS];
            /* comb_krange is a modest positive integer cast to float */
            const float krange = (float)(1 + checkasm_rand() % 400);
            fill_inputs(i % 3);
            /* out[0] is the driver's job; seed both identically so the
             * whole span compares and any stray write is caught */
            memset(out_c, 0x55, sizeof(out_c));
            memset(out_a, 0x55, sizeof(out_a));
            checkasm_call_ref(out_c, cur, prev, next, &krange, w);
            checkasm_call_new(out_a, cur, prev, next, &krange, w);
            /* byte-identical is the contract (this feeds split3d's
             * candidate compare), so compare the raw bits -- an exact
             * uint32 match also pins down -0.0 and any NaN lane that a
             * ULP compare would wave through */
            checkasm_check1d(uint32_t, (const uint32_t *)(out_c + 1),
                             (const uint32_t *)(out_a + 1), w - 1, "out");
        }
        {
            const float kb = 90.0f;
            fill_inputs(1);
            checkasm_bench_new(out_a, cur, prev, next, &kb, MAXW);
        }
    }
    checkasm_report("comb2d");
}
