#include <stdint.h>
#include <string.h>

#include <checkasm/checkasm.h>
#include <checkasm/test.h>
#include <checkasm/utils.h>

#include "decode.h"

#define FS COMP_DECODE_FILTER_SIZE
#define MAXW 928
#define STRIDE1 (MAXW + 2 * FS)       /* the production layout */
#define STRIDE2 (MAXW + 2 * FS + 50)  /* catch stride bugs */
#define MAXSTRIDE STRIDE2

static CHECKASM_ALIGN(int32_t m_buf[4 * MAXSTRIDE]);
static CHECKASM_ALIGN(int32_t n_buf[4 * MAXSTRIDE]);
static CHECKASM_ALIGN(int32_t cf_buf[(FS + 1) * 4]);
static CHECKASM_ALIGN(int32_t u_c[MAXW]);
static CHECKASM_ALIGN(int32_t v_c[MAXW]);
static CHECKASM_ALIGN(int32_t u_a[MAXW]);
static CHECKASM_ALIGN(int32_t v_a[MAXW]);

/* w must be a multiple of 8 per the kernel contract */
static const int widths[] = { 8, 16, 24, 64, 200, 512, 720, 928 };
#define NWIDTHS (int)(sizeof(widths) / sizeof(widths[0]))

/* magnitudes the demodulator can produce: the m/n planes hold sums of
 * up to two int16 carrier products, the coefficients are Q16 fractions
 * of a distribution summing to 65536 */
static void fill_inputs(void)
{
    for (int i = 0; i < 4 * MAXSTRIDE; i++) {
        m_buf[i] = (int32_t)(checkasm_rand() % 131069) - 65534;
        n_buf[i] = (int32_t)(checkasm_rand() % 131069) - 65534;
    }
    for (int i = 0; i < (FS + 1) * 4; i++)
        cf_buf[i] = (int32_t)(checkasm_rand() % 16385) - 8192;
}

void checkasm_test_pal_demod(void)
{
    checkasm_declare(void, int32_t *, int32_t *, const int32_t *,
                     const int32_t *, ptrdiff_t, const int32_t *,
                     int, int32_t, int32_t, int32_t);
    if (checkasm_check_func(comp_get_pal_demod_fn(checkasm_get_cpu_flags()),
                            "pal_demod_row")) {
        for (int i = 0; i < NWIDTHS; i++) {
            const ptrdiff_t stride = i & 1 ? STRIDE2 : STRIDE1;
            const int32_t bp = (int32_t)(checkasm_rand() % 65535) - 32767;
            const int32_t bq = (int32_t)(checkasm_rand() % 65535) - 32767;
            const int32_t vsw = i & 2 ? -1 : 1;
            fill_inputs();
            memset(u_c, 0xaa, sizeof(u_c));
            memset(v_c, 0xaa, sizeof(v_c));
            memset(u_a, 0x55, sizeof(u_a));
            memset(v_a, 0x55, sizeof(v_a));
            checkasm_call_ref(u_c, v_c, m_buf, n_buf, stride, cf_buf,
                              widths[i], bp, bq, vsw);
            checkasm_call_new(u_a, v_a, m_buf, n_buf, stride, cf_buf,
                              widths[i], bp, bq, vsw);
            checkasm_check1d(int32_t, u_c, u_a, widths[i], "u");
            checkasm_check1d(int32_t, v_c, v_a, widths[i], "v");
        }
        fill_inputs();
        checkasm_bench_new(u_a, v_a, m_buf, n_buf, (ptrdiff_t)STRIDE1,
                           cf_buf, MAXW, 12345, -23456, 1);
    }
    checkasm_report("pal_demod");
}
