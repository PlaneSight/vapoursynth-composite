/*
 * Q15 FIR row kernel: scalar reference and SIMD dispatch. The kernel is
 * pure integer (no float, so the encoder's contraction-on build and the
 * decoder's -ffp-contract=off build produce identical results).
 */

#include "cpu.h"
#include "fir_row.h"

static void fir_row_q15_c(int32_t *out, const int32_t *in,
                          const int32_t *coef, int taps, int w)
{
    for (int x = 0; x < w; x++) {
        int64_t acc = 0;
        for (int j = 0; j < taps; j++)
            acc += (int64_t)coef[j] * in[x + j];
        out[x] = (int32_t)((acc + 16384) >> 15);
    }
}

#if defined(__x86_64__)
#define FIR_ROW_Q15_ASM(isa)                                                \
    void comp_fir_row_q15_##isa(int32_t *out, const int32_t *in,            \
                                const int32_t *coef, int taps, int w)
FIR_ROW_Q15_ASM(sse4);
FIR_ROW_Q15_ASM(avx2);
FIR_ROW_Q15_ASM(avx512);
#endif

comp_fir_row_q15_fn comp_get_fir_row_q15_fn(unsigned cpu)
{
#if defined(__x86_64__)
    if (cpu & COMP_CPU_AVX512)
        return comp_fir_row_q15_avx512;
    if (cpu & COMP_CPU_AVX2)
        return comp_fir_row_q15_avx2;
    if (cpu & COMP_CPU_SSE41)
        return comp_fir_row_q15_sse4;
#endif
    (void)cpu;
    return fir_row_q15_c;
}
