/*
 * Q15 FIR row kernel, shared by the encoder's chroma low-pass and the
 * decoder's demodulator filters.
 */

#ifndef COMP_FIR_ROW_H
#define COMP_FIR_ROW_H

#include <stdint.h>

/* Round a row length up to the SIMD write bound: FIR-row tiers may
 * write out and read in up to this many samples, so row buffers are
 * sized with it. */
#define COMP_FIR_ROW_ALIGN(w) (((w) + 15) & ~15)

/* One Q15 FIR pass over a zero-padded row:
 * out[x] = (int32_t)((sum_j (int64_t)coef[j] * in[x + j] + 16384) >> 15)
 * for 0 <= x < w. in holds w + taps - 1 elements (the caller centers
 * its data and zeroes the wings, reproducing the C bounds check).
 * SIMD tiers write out and read in through COMP_FIR_ROW_ALIGN(w)
 * samples, so out has that capacity and in that plus taps - 1; the
 * padding lanes of out are scratch. Accumulator contract:
 * |acc| < 2^45 for any x in the padded span. */
typedef void (*comp_fir_row_q15_fn)(int32_t *out, const int32_t *in,
                                    const int32_t *coef, int taps, int w);

comp_fir_row_q15_fn comp_get_fir_row_q15_fn(unsigned cpu);

#endif
