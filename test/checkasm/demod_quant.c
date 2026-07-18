#include <stdint.h>
#include <string.h>

#include <checkasm/checkasm.h>
#include <checkasm/test.h>
#include <checkasm/utils.h>

#include "decode.h"

#define MAXW 768

static CHECKASM_ALIGN(int32_t in[MAXW]);
static CHECKASM_ALIGN(uint16_t out_c[MAXW]);
static CHECKASM_ALIGN(uint16_t out_a[MAXW]);

/* (K, den, base) for the three quantize configs, both NTSC setups and
 * PAL: chroma K=32768 with den = ku/kv, luma K=luma_num/den=luma_den */
static const struct { int32_t k, den, base; } configs[] = {
    { 32768, 17859, 32768 }, { 32768, 25189, 32768 },  /* NTSC s0 ku/kv */
    { 32768, 16520, 32768 }, { 32768, 23300, 32768 },  /* NTSC s1 ku/kv */
    { 32768, 18752, 32768 }, { 32768, 26449, 32768 },  /* PAL ku/kv */
    { 140, 219, 4096 }, { 259, 438, 4096 }, { 147, 219, 4096 },  /* luma */
};
#define NCONF (int)(sizeof(configs) / sizeof(configs[0]))

/* checkasm only proves asm == the magic C reference (both use
 * comp_magic_init). This reimplements the original rdiv/clamp_u16 the
 * kernel replaced, so the reference itself is proven to reproduce the
 * round-half-away divide exhaustively over the tested range -- the
 * saturation-collapse contract from D048. */
static int32_t rdiv_ref(int64_t num, int32_t den)
{
    return (int32_t)((num >= 0 ? num + den / 2 : num - den / 2) / den);
}

static uint16_t quant_ref(int32_t n, int32_t k, int32_t den, int32_t base)
{
    const int32_t v = base + rdiv_ref((int64_t)n * k, den);
    return (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v);
}

/* fill in[] over the full +-2^20 contract range, with a dense band
 * straddling +-ku so the clamp_u16 saturation boundary is exercised (a
 * too-tight exact-range margin would surface there as a clamp flip) */
static void fill_inputs(int32_t ku)
{
    for (int x = 0; x < MAXW; x++) {
        const unsigned r = checkasm_rand();
        int32_t v;
        switch (r % 3) {
        case 0: v = (int32_t)(r >> 2) % (1 << 21) - (1 << 20); break;  /* full range */
        case 1: v = (int32_t)((r >> 2) % (4 * ku + 1)) - 2 * ku; break; /* near +-ku */
        default: v = (int32_t)((r >> 2) % 4096) - 2048; break;          /* small */
        }
        in[x] = v;
    }
}

void checkasm_test_demod_quant(void)
{
    checkasm_declare(void, uint16_t *, const int32_t *,
                     const comp_magic_t *, int);
    if (checkasm_check_func(comp_get_demod_quant_row_fn(checkasm_get_cpu_flags()),
                            "demod_quant_row")) {
        static const int widths[] = { 24, 40, 122, 360, 758, 768 };
        const int nw = (int)(sizeof(widths) / sizeof(widths[0]));
        for (int c = 0; c < NCONF; c++) {
            comp_magic_t mag;
            comp_magic_init(&mag, configs[c].k, configs[c].den, configs[c].base);
            for (int i = 0; i < nw; i++) {
                const int w = widths[i];
                fill_inputs(configs[c].den);
                memset(out_c, 0x55, sizeof(out_c));
                memset(out_a, 0x55, sizeof(out_a));
                checkasm_call_ref(out_c, in, &mag, w);
                checkasm_call_new(out_a, in, &mag, w);
                checkasm_check1d(uint16_t, out_c, out_a, w, "out");
                /* prove the magic reference itself matches the original
                 * rdiv over the tested inputs (checkasm alone only shows
                 * asm == magic-C, both fed by comp_magic_init) */
                for (int x = 0; x < w; x++)
                    if (out_c[x] != quant_ref(in[x], configs[c].k,
                                              configs[c].den, configs[c].base)) {
                        checkasm_fail_func("rdiv mismatch c=%d w=%d x=%d in=%d",
                                           c, w, x, in[x]);
                        break;
                    }
            }
        }
        {
            comp_magic_t mag;
            comp_magic_init(&mag, 32768, 16520, 32768);
            fill_inputs(16520);
            checkasm_bench_new(out_a, in, &mag, MAXW);
        }
    }
    checkasm_report("demod_quant");
}
