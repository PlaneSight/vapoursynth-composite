#ifndef COMP_DECODE_H
#define COMP_DECODE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "subcarrier.h"
#include "transform2d.h"
#include "transform3d.h"

#define COMP_DECODE_FILTER_SIZE 7

typedef struct comp_decode_scratch_t comp_decode_scratch_t;

struct comp_decode_scratch_t {
    int used;
    float *chroma_f;    /* one frame, active raster */
    int16_t *chroma;    /* quantised copy for the fixed-point demod */
    float *tmp3;        /* NTSC: 2D chroma; in 3D mode, 1D+2D of three frames */
};

typedef struct comp_frame_view_t comp_frame_view_t;

struct comp_frame_view_t {
    const uint16_t *data;
    ptrdiff_t stride;
};

typedef struct comp_decode_t comp_decode_t;

struct comp_decode_t {
    int standard;
    int dimensions;      /* 2 or 3 */
    int width;
    int height;          /* full raster height: 576 PAL, 486 NTSC */
    int den;
    int32_t ku, kv;      /* the encoder's chroma scales, Q15 */
    int32_t level_black;
    int32_t luma_num, luma_den;  /* y16 = 4096 + (level - black) * num/den */
    int32_t comb_krange;         /* NTSC 2D comb adaptivity range */
    int16_t sin_q15[COMP_SC_DEN_PAL];
    int32_t cfilt_q16[COMP_DECODE_FILTER_SIZE + 1][4];
    comp_transform2d_t transform;
    comp_transform3d_t transform3;

    /* scratch pool, allocated once at init and reused per frame */
    int nscratch;
    comp_decode_scratch_t *scratch;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

/* nscratch is the maximum number of concurrent frame requests. setup
 * selects the NTSC 7.5 IRE pedestal; threshold is Transform PAL's
 * bin-symmetry ratio; each is ignored by the other standard. */
int comp_decode_init(comp_decode_t *d, int standard, double threshold,
                     int nscratch, int setup, int dimensions);
void comp_decode_free(comp_decode_t *d);

/* source frames needed each side of the decoded frame */
int comp_decode_look(const comp_decode_t *d);

/* Demodulate one frame of active-picture composite (GRAY16 at composite
 * levels) into YUV444P16; strides are in samples. rows is the frame's
 * height and row_off its position in the raster: a 480-line NTSC frame
 * occupies raster rows row_off..row_off+479.
 * views[]/view_frames[] hold 2*look+1 composite frames centred on
 * `frame`, clip edges clamped (with the clamped frame numbers). */
void comp_decode_frame(comp_decode_t *d, int frame, int rows, int row_off,
                       const comp_frame_view_t *views, const int *view_frames,
                       int look,
                       uint16_t *dsty, ptrdiff_t ystride,
                       uint16_t *dstu, ptrdiff_t ustride,
                       uint16_t *dstv, ptrdiff_t vstride);

#endif
