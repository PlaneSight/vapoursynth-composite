#ifndef COMP_DECODE_H
#define COMP_DECODE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "encode.h"
#include "subcarrier.h"
#include "transform2d.h"
#include "transform3d.h"

#define COMP_DECODE_FILTER_SIZE 7
#define COMP_EQ_TAPS 31
#define COMP_NARROW_TAPS 13

typedef struct comp_decode_scratch_t comp_decode_scratch_t;

struct comp_decode_scratch_t {
    int used;
    float *chroma_f;    /* one frame, active raster */
    int16_t *chroma;    /* quantised copy for the fixed-point demod */
    float *conf;        /* eq=2: transform chroma-confidence map */
    int16_t *chroma2;   /* hybrid: the comb's chroma estimate */
    uint8_t *mask;      /* hybrid: 1 where the comb's temporal candidate won */
    float *tmp3;        /* NTSC: 2D chroma; in 3D mode, 1D+2D of three frames */
    uint16_t *refine;   /* refine loop: YUV estimate, recomposite, scratch */
};

typedef struct comp_frame_view_t comp_frame_view_t;

struct comp_frame_view_t {
    const uint16_t *data;
    ptrdiff_t stride;
};

typedef struct comp_decode_t comp_decode_t;

struct comp_decode_t {
    int standard;
    int dimensions;      /* 1 (crude notch), 2, or 3 */
    int use_transform;   /* NTSC 3D: 1 transform, 2 comb/transform hybrid */
    int refine;          /* Y-only Landweber iterations, 0 = off */
    int width;
    int height;          /* full raster height: 576 PAL, 486 NTSC */
    int den;
    int32_t ku, kv;      /* the encoder's chroma scales, Q15 */
    int32_t level_black;
    int32_t luma_num, luma_den;  /* y16 = 4096 + (level - black) * num/den */
    int32_t comb_krange;         /* NTSC 2D comb adaptivity range */
    int eq;                      /* equalizer: 0 off, 1 fixed, 2 leak-aware */
    int cti;                     /* luma-guided chroma transient improvement */
    int32_t eq_q15[COMP_EQ_TAPS];
    int32_t narrow_q15[COMP_NARROW_TAPS];  /* eq=2: sub-nominal chroma LP */
    int16_t sin_q15[COMP_SC_DEN_PAL];
    int32_t cfilt_q16[COMP_DECODE_FILTER_SIZE + 1][4];
    comp_transform2d_t transform;
    comp_transform3d_t transform3;
    comp_encode_t enc;   /* for refine resynthesis */

    /* scratch pool, allocated once at init and reused per frame */
    int nscratch;
    comp_decode_scratch_t *scratch;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

/* nscratch is the maximum number of concurrent frame requests. setup
 * selects the NTSC 7.5 IRE pedestal; threshold is Transform PAL's
 * bin-symmetry ratio. eq enables the chroma cascade equalizer; refine
 * is the number of Y-only Landweber refinement iterations against the
 * dimensions=1 model; use_transform selects Transform NTSC separation
 * instead of the comb (1) or a per-sample comb/transform hybrid (2)
 * (NTSC, dimensions=3 only); level selects the
 * transform's amplitude-limiting mode (threshold then unused).
 * eq: 0 off, 1 the fixed cascade inverse, 2 leak-aware (the transform's
 * confidence map steers chroma bandwidth from a sub-nominal low-pass
 * through nominal to the boosted cascade inverse; needs a transform
 * path). evidence > 0 enables the LF-luma prior (PAL transforms only;
 * see transform2d.h). cti resynthesizes chroma edges from coincident
 * luma transitions (dimensions >= 2). */
int comp_decode_init(comp_decode_t *d, int standard, double threshold,
                     int nscratch, int setup, int dimensions, int eq, int refine,
                     int use_transform, int level, double evidence, int cti);
void comp_decode_free(comp_decode_t *d);

/* override the Transform PAL per-bin thresholds after init; n must be
 * COMP_T2D_NTHRESH (dimensions=2) or COMP_T3D_NTHRESH (dimensions=3) */
int comp_decode_set_thresholds(comp_decode_t *d, const double *t, int n);

/* install a trained per-bin soft-gain LUT (transform separations only),
 * replacing the pair test; n must be the bin count * COMP_LUT_K */
int comp_decode_set_lut(comp_decode_t *d, const double *v, int n);

/* source frames needed each side of the decoded frame */
int comp_decode_look(const comp_decode_t *d);

/* Demodulate one frame of active-picture composite (GRAY16 at composite
 * levels) into YUV444P16; strides are in samples. rows is the frame's
 * height and row_off its position in the raster: a 480-line NTSC frame
 * occupies raster rows row_off..row_off+479.
 * views[]/view_frames[] hold 2*look+1 composite frames centered on
 * `frame`, clip edges clamped (with the clamped frame numbers); nframes
 * is the clip length, so out-of-clip fields become black as in the
 * reference implementation.
 * orig_y, when non-NULL, is the luma plane of the pre-encode degraded
 * picture at the raster; d->refine Landweber iterations then deconvolve
 * the crude-decoder model against it (Y only). */
void comp_decode_frame(comp_decode_t *d, int frame, int nframes,
                       int rows, int row_off,
                       const comp_frame_view_t *views, const int *view_frames,
                       int look,
                       const uint16_t *orig_y, ptrdiff_t orig_stride,
                       uint16_t *dsty, ptrdiff_t ystride,
                       uint16_t *dstu, ptrdiff_t ustride,
                       uint16_t *dstv, ptrdiff_t vstride);

#endif
