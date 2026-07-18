#include <stdint.h>
#include <string.h>

#include <checkasm/checkasm.h>
#include <checkasm/test.h>
#include <checkasm/utils.h>

#include "decode.h"

#define MAXW 768

static CHECKASM_ALIGN(int32_t p[MAXW]);
static CHECKASM_ALIGN(int32_t q[MAXW]);
static CHECKASM_ALIGN(int32_t u_c[MAXW]), u_a[MAXW];
static CHECKASM_ALIGN(int32_t v_c[MAXW]), v_a[MAXW];

void checkasm_test_demod_rotate(void)
{
    checkasm_declare(void, int32_t *, int32_t *, const int32_t *,
                     const int32_t *, const int32_t *, int);
    if (checkasm_check_func(comp_get_demod_rotate_row_fn(checkasm_get_cpu_flags()),
                            "demod_rotate_row")) {
        static const int widths[] = { 24, 40, 122, 360, 758, 768 };
        const int nw = (int)(sizeof(widths) / sizeof(widths[0]));
        for (int i = 0; i < nw; i++) {
            const int w = widths[i];
            /* p, q are colorlp FIR outputs, |.| <= ~2^15.4; bp, bq are
             * Q15 burst-phase, |.| <= 2^15. Cover the full range. */
            for (int x = 0; x < MAXW; x++) {
                p[x] = (int32_t)(checkasm_rand() % (1 << 17)) - (1 << 16);
                q[x] = (int32_t)(checkasm_rand() % (1 << 17)) - (1 << 16);
            }
            int32_t bpq[2] = {
                (int32_t)(checkasm_rand() % 65536) - 32768,
                (int32_t)(checkasm_rand() % 65536) - 32768,
            };
            memset(u_c, 0x55, sizeof(u_c));
            memset(u_a, 0x55, sizeof(u_a));
            memset(v_c, 0x55, sizeof(v_c));
            memset(v_a, 0x55, sizeof(v_a));
            checkasm_call_ref(u_c, v_c, p, q, bpq, w);
            checkasm_call_new(u_a, v_a, p, q, bpq, w);
            checkasm_check1d(int32_t, u_c, u_a, w, "u");
            checkasm_check1d(int32_t, v_c, v_a, w, "v");
        }
        {
            int32_t bpq[2] = { -12345, -23456 };
            checkasm_bench_new(u_a, v_a, p, q, bpq, MAXW);
        }
    }
    checkasm_report("demod_rotate");
}
