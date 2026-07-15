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
#define COMP_T2D_YCOMPLEX COMP_T2D_YTILE

typedef struct comp_transform2d_t comp_transform2d_t;

struct comp_transform2d_t {
    fftwf_plan forward;
    fftwf_plan inverse;
    float window[COMP_T2D_YTILE][COMP_T2D_XTILE];
    float threshold_sq;
};

/* threshold is the bin-symmetry ratio, (0,1]; 0.4 is the reference default */
int comp_transform2d_init(comp_transform2d_t *t, double threshold);
void comp_transform2d_free(comp_transform2d_t *t);

/* Extract the chroma component of one field of active-picture composite.
 * comp/chroma are field views (strides in samples); chroma is overwritten. */
void comp_transform2d_field(const comp_transform2d_t *t,
                            const uint16_t *comp, ptrdiff_t comp_stride,
                            int width, int rows,
                            float *chroma, ptrdiff_t chroma_stride);

#endif
