#include <stdint.h>
#include <string.h>

#include <checkasm/checkasm.h>
#include <checkasm/test.h>
#include <checkasm/utils.h>

#include "encode.h"

#define MAXW 928

static CHECKASM_ALIGN(int32_t luma[MAXW]);
static CHECKASM_ALIGN(int32_t uf[MAXW]);
static CHECKASM_ALIGN(int32_t vf[MAXW]);
static CHECKASM_ALIGN(uint16_t out_c[MAXW]);
static CHECKASM_ALIGN(uint16_t out_a[MAXW]);
static CHECKASM_ALIGN(int32_t s4[4]);
static CHECKASM_ALIGN(int32_t c4[4]);

/* the production widths plus ragged and tiny ones; 758 has a phase-2
 * tail (w % 4 != 0), the trap the fixed-pattern main loop must not hit */
static const int widths[] = { 1, 3, 7, 8, 15, 16, 100, 758, 928 };
#define NWIDTHS (int)(sizeof(widths) / sizeof(widths[0]))

/* the two standards' chroma levels (PAL ku/kv, NTSC ku/kv) */
static const struct { int32_t ku, kv; } levels[] = {
    { 18752, 26449 }, { 17859, 25189 },
};
#define NLEV (int)(sizeof(levels) / sizeof(levels[0]))

/* uf/vf come from the Q15 FIR: |value| <= 2^15; luma is a level-mapped
 * Y in roughly [0x100, 0xFEFF]; s4/c4 are +-sin_q15 (|.| <= 2^15). The
 * combined chroma stays under 2^31, matching the kernel's int32 range. */
static void fill_inputs(void)
{
    for (int i = 0; i < MAXW; i++) {
        luma[i] = (int32_t)(checkasm_rand() % 0x10000);
        /* uf/vf are the Q15 FIR output; src - 32768 tops at +32767, so
         * with unity-sum taps the magnitude stays in [-32768, 32767] --
         * the int16 range the scaling vpmaddwd depends on */
        uf[i] = (int32_t)(checkasm_rand() % 65536) - 32768;
        vf[i] = (int32_t)(checkasm_rand() % 65536) - 32768;
    }
    /* a valid carrier pattern: { sn, cs, -sn, -cs } and its V-switched
     * cosine, with sn/cs the sine table's quadrature pair */
    const int32_t sn = (int32_t)(checkasm_rand() % 65537) - 32768;
    const int32_t cs = (int32_t)(checkasm_rand() % 65537) - 32768;
    const int32_t vsw = (checkasm_rand() & 1) ? 1 : -1;
    s4[0] = sn; s4[1] = cs; s4[2] = -sn; s4[3] = -cs;
    c4[0] = vsw * cs; c4[1] = -vsw * sn; c4[2] = -vsw * cs; c4[3] = vsw * sn;
}

void checkasm_test_encode_mod(void)
{
    checkasm_declare(void, uint16_t *, const int32_t *, const int32_t *,
                     const int32_t *, int32_t, int32_t, const int32_t *,
                     const int32_t *, int);
    if (checkasm_check_func(comp_get_encode_mod_row_fn(checkasm_get_cpu_flags()),
                            "encode_mod_row")) {
        for (int l = 0; l < NLEV; l++) {
            for (int i = 0; i < NWIDTHS; i++) {
                fill_inputs();
                memset(out_c, 0xaa, sizeof(out_c));
                memset(out_a, 0x55, sizeof(out_a));
                checkasm_call_ref(out_c, luma, uf, vf, levels[l].ku,
                                  levels[l].kv, s4, c4, widths[i]);
                checkasm_call_new(out_a, luma, uf, vf, levels[l].ku,
                                  levels[l].kv, s4, c4, widths[i]);
                checkasm_check1d(uint16_t, out_c, out_a, widths[i], "out");
            }
        }
        fill_inputs();
        checkasm_bench_new(out_a, luma, uf, vf, levels[0].ku, levels[0].kv,
                           s4, c4, MAXW);
    }
    checkasm_report("encode_mod");
}
