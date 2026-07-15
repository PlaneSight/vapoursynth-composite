#ifndef COMP_TRANSFORM3D_H
#define COMP_TRANSFORM3D_H

#include <stddef.h>
#include <stdint.h>

#include <fftw3.h>

/* Overlapping-tile geometry: XTILE samples by YTILE frame lines by
 * ZTILE fields, advancing by half a tile in each direction. */
#define COMP_T3D_XTILE    16
#define COMP_T3D_YTILE    32
#define COMP_T3D_ZTILE    8
#define COMP_T3D_XCOMPLEX (COMP_T3D_XTILE / 2 + 1)

/* fields the tiles covering one output frame can span, each side of it */
#define COMP_T3D_LOOK 3

/* bins the symmetry filter considers: z, y outer, x fsc/2..fsc inner */
#define COMP_T3D_NTHRESH (COMP_T3D_ZTILE * COMP_T3D_YTILE \
                          * (COMP_T3D_XTILE / 4 - COMP_T3D_XTILE / 8 + 1))

typedef struct comp_field_view_t comp_field_view_t;

struct comp_field_view_t {
    const uint16_t *data;
    ptrdiff_t stride;
};

typedef struct comp_transform3d_t comp_transform3d_t;

struct comp_transform3d_t {
    fftwf_plan forward;
    fftwf_plan inverse;
    float window[COMP_T3D_ZTILE][COMP_T3D_YTILE][COMP_T3D_XTILE];
    float threshold_sq[COMP_T3D_NTHRESH];
};

int comp_transform3d_init(comp_transform3d_t *t, double threshold);
void comp_transform3d_free(comp_transform3d_t *t);

/* Extract the chroma of the two fields of output frame `frame`.
 * fields[] holds views of the absolute field indices [z0, z0 + nfields);
 * out-of-clip fields must be supplied clamped with parity preserved.
 * chroma0/chroma1 are field buffers (first/second field of the frame). */
void comp_transform3d_frame(const comp_transform3d_t *t,
                            const comp_field_view_t *fields, int z0, int nfields,
                            int frame, int width, int field_rows,
                            float *chroma0, float *chroma1, ptrdiff_t chroma_stride);

#endif
