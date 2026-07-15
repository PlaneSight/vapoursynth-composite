#ifndef COMP_DECODE_H
#define COMP_DECODE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "subcarrier.h"
#include "transform2d.h"

#define COMP_DECODE_FILTER_SIZE 7

typedef struct comp_decode_scratch_t comp_decode_scratch_t;

struct comp_decode_scratch_t {
    int used;
    float *chroma_f;    /* one frame, active raster */
    int16_t *chroma;    /* quantised copy for the fixed-point demod */
};

typedef struct comp_decode_t comp_decode_t;

struct comp_decode_t {
    int standard;
    int width;
    int height;
    int den;
    int16_t sin_q15[COMP_SC_DEN_PAL];
    int32_t cfilt_q16[COMP_DECODE_FILTER_SIZE + 1][4];
    comp_transform2d_t transform;

    /* scratch pool, allocated once at init and reused per frame */
    int nscratch;
    comp_decode_scratch_t *scratch;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

/* nscratch is the maximum number of concurrent frame requests */
int comp_decode_init(comp_decode_t *d, int standard, double threshold, int nscratch);
void comp_decode_free(comp_decode_t *d);

/* demodulate one frame of active-picture composite (GRAY16 at CVBS
 * levels) into YUV444P16; strides are in samples */
void comp_decode_frame(comp_decode_t *d, int frame,
                       const uint16_t *comp, ptrdiff_t comp_stride,
                       uint16_t *dsty, ptrdiff_t ystride,
                       uint16_t *dstu, ptrdiff_t ustride,
                       uint16_t *dstv, ptrdiff_t vstride);

#endif
