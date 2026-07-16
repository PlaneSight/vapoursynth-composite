#ifndef COMP_TRANSFORM3D_H
#define COMP_TRANSFORM3D_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <fftw3.h>

#include "transform2d.h"  /* COMP_LUT_K */

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
    int standard;
    int level;
    int use_lut;
    float evidence;
    float window[COMP_T3D_ZTILE][COMP_T3D_YTILE][COMP_T3D_XTILE];
    float threshold_sq[COMP_T3D_NTHRESH];
    float lut[COMP_T3D_NTHRESH][COMP_LUT_K];
};

/* PAL chroma is symmetric about (fsc, 72 c/aph, 18.75 Hz); the NTSC
 * variant (after the ld-decode transform-ntsc branch) reflects about
 * (fsc, 120 c/aph, 15 Hz) with luma-reference evidence and a
 * frequency-shaped threshold, since NTSC's shared U/V carrier makes
 * the symmetry only approximate. level selects amplitude limiting
 * instead of the threshold test (threshold then unused). evidence > 0
 * enables the LF-luma prior of US 7,872,689 (PAL only; see
 * transform2d.h). */
int comp_transform3d_init(comp_transform3d_t *t, double threshold, int standard,
                          int level, double evidence);
void comp_transform3d_free(comp_transform3d_t *t);

/* install a trained per-bin soft-gain LUT (COMP_T3D_NTHRESH * COMP_LUT_K
 * values in [0,1], knots innermost), replacing the pair test (and, for
 * NTSC, the shaped threshold and luma-evidence test) */
void comp_transform3d_set_lut(comp_transform3d_t *t, const double *v);

/* Slab cache. A slab holds the accumulated chroma (and confidence)
 * contribution of every tile at one temporal grid position tz to the
 * ZTILE fields those tiles span; the tile grid is anchored at absolute
 * field 0, so a slab's content depends only on (tz, parity) and each
 * slab serves the four output frames whose tiles it provides. Slabs
 * are built once under the cache and reused, cutting the FFT and bin
 * filter work by up to 4x. */
typedef struct {
    int tz;          /* absolute field index of the slab origin */
    int parity;
    int ready;       /* built; slabs are immutable once ready */
    int refs;        /* readers + builder; protects against eviction */
    uint64_t stamp;  /* LRU */
    float *chroma;   /* [ZTILE][field_rows][width] */
    float *conf;     /* likewise; NULL when confidence is not tracked */
} comp_t3d_slab_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    comp_t3d_slab_t *slabs;
    int nslabs;
    int width;       /* capacity; frame calls may use less */
    int field_rows;
    uint64_t counter;
} comp_t3d_cache_t;

/* nslabs >= 2 (a frame draws on two slabs); size it to the number of
 * concurrent frame requests / 2 + 3 so in-flight frames do not evict
 * each other's slabs. width/field_rows are a capacity: frame calls may
 * pass smaller geometry (constant within a clip). with_conf must be
 * set when the frame calls will request confidence maps. */
int comp_t3d_cache_init(comp_t3d_cache_t *c, int nslabs, int width,
                        int field_rows, int with_conf);
void comp_t3d_cache_free(comp_t3d_cache_t *c);

/* Extract the chroma of the two fields of output frame `frame`.
 * fields[] holds views of the absolute field indices [z0, z0 + nfields);
 * out-of-clip fields must be supplied clamped with parity preserved,
 * and the window must cover fields the caller's slabs span so that a
 * slab's content is the same whichever frame builds it. parity gives
 * the raster row parity of even (temporally first) fields: 0 for PAL;
 * NTSC's first field sits on odd rows at row offsets 0/4 and even rows
 * at offset 5. chroma0/chroma1 are the row-parity field buffers
 * matching the raster layout. conf0/conf1, when non-NULL, receive
 * [0,1] chroma-confidence maps (same layout and stride as the chroma
 * buffers). width and field_rows must not exceed the cache capacity. */
void comp_transform3d_frame(const comp_transform3d_t *t, comp_t3d_cache_t *c,
                            const comp_field_view_t *fields, int z0, int nfields,
                            int frame, int parity, int width, int field_rows,
                            float *chroma0, float *chroma1, ptrdiff_t chroma_stride,
                            float *conf0, float *conf1);

#endif
