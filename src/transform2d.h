#ifndef COMP_TRANSFORM2D_H
#define COMP_TRANSFORM2D_H

#include <stddef.h>
#include <stdint.h>

#include <fftw3.h>

/* Overlapping-tile geometry: XTILE x YTILE tiles advancing by half a tile
 * in each direction, raised-cosine windowed so overlap-added inverse
 * tiles sum to unity. */
#define COMP_T2D_XTILE    32
#define COMP_T2D_YTILE    16
#define COMP_T2D_XCOMPLEX (COMP_T2D_XTILE / 2 + 1)

/* bins the symmetry filter considers: y 0..YTILE-1 outer, x fsc/2..fsc
 * inner (the reference's threshold ordering) */
#define COMP_T2D_NTHRESH (COMP_T2D_YCOMPLEX * (COMP_T2D_XTILE / 4 - COMP_T2D_XTILE / 8 + 1))
#define COMP_T2D_YCOMPLEX COMP_T2D_YTILE

/* soft-gain LUT: per-bin gain over the pair-symmetry ratio, knots
 * uniformly spaced on [0, 1], linearly interpolated */
#define COMP_LUT_K 16

typedef struct comp_transform2d_t comp_transform2d_t;

struct comp_transform2d_t {
    fftwf_plan forward;
    fftwf_plan inverse;
    int level;
    int use_lut;
    float evidence;
    float window[COMP_T2D_YTILE][COMP_T2D_XTILE];
    float threshold_sq[COMP_T2D_NTHRESH];
    float lut[COMP_T2D_NTHRESH][COMP_LUT_K];
};

/* threshold is the bin-symmetry ratio, (0,1]; 0.4 is the reference
 * default. level selects amplitude limiting instead: each bin pair has
 * the larger magnitude set to the smaller (threshold then unused).
 * evidence > 0 scales every pair by e/(e + evidence*b): e the squared
 * magnitude of the low-frequency luma bin at the pair's baseband
 * difference frequency, b the pair's larger squared magnitude — the
 * scene-statistics prior of US 7,872,689 (true chroma detail
 * co-locates with LF luma detail; cross-color does not). */
int comp_transform2d_init(comp_transform2d_t *t, double threshold, int level,
                          double evidence);

/* install a trained per-bin soft-gain LUT (COMP_T2D_NTHRESH * COMP_LUT_K
 * values in [0,1], knots innermost), replacing the pair test */
void comp_transform2d_set_lut(comp_transform2d_t *t, const double *v);
void comp_transform2d_free(comp_transform2d_t *t);

/* Extract the chroma component of one field of active-picture composite.
 * comp/chroma are field views (strides in samples); chroma is
 * overwritten. conf, when non-NULL, receives a [0,1] chroma-confidence
 * map (the kept energy's mean pair-symmetry ratio, overlap-added). */
void comp_transform2d_field(const comp_transform2d_t *t,
                            const uint16_t *comp, ptrdiff_t comp_stride,
                            int width, int rows,
                            float *chroma, ptrdiff_t chroma_stride,
                            float *conf, ptrdiff_t conf_stride);

#endif
