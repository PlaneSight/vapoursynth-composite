#ifndef COMP_FFT_H
#define COMP_FFT_H

#include "transform3d.h"

/* Internal fixed-size 3D real FFT for the transform tiles, pruned to
 * the x band the bin filter uses (columns XTILE/8 .. XTILE/4 + 2,
 * i.e. 2..6 of the 9 r2c columns).
 *
 * Forward: windowed real tile [ZTILE][ytile][XTILE] -> band tile
 * [ZTILE][ytile][COMP_FFT_ROWSTRIDE] complex-interleaved floats
 * (COMP_FFT_NCOL valid columns, the rest zero),
 * with the (ky, kz) axes in bit-reversed order (decimation in
 * frequency, no permutation pass; the bin filter's tables absorb the
 * numbering). Only the band columns are computed, which is valid for
 * the trained-LUT filter path.
 *
 * Inverse: packed band tile (consumed in place) -> real tile,
 * unnormalized by ZTILE * ytile * XTILE exactly as FFTW's c2r, so the
 * overlap-add's normalization stays unchanged. Band input outside the
 * columns is implicitly zero, which every filter mode satisfies.
 *
 * Both are deterministic (fixed code path, embedded twiddles), but
 * their rounding differs from FFTW's: the internal path's output is
 * not byte-identical to the FFTW path's. */
#define COMP_FFT_NCOL (COMP_T3D_XTILE / 4 - COMP_T3D_XTILE / 8 + 3)

/* band rows are padded to a 64-byte stride (16 floats, 8 complex) so
 * the vector passes work on whole registers; columns NCOL..7 are zero */
#define COMP_FFT_ROWSTRIDE COMP_T3D_XTILE

comp_fft_fwd_fn comp_get_fft_fwd_fn(int standard, unsigned cpu);
comp_fft_inv_fn comp_get_fft_inv_fn(int standard, unsigned cpu);

#endif
