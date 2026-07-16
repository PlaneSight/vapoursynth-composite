/*
 * PAL/NTSC composite video encode/decode for VapourSynth.
 *
 * Encode() modulates YCbCr to composite; Decode() demodulates it back
 * via 2D Transform PAL (PAL only so far).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <VapourSynth4.h>
#include <VSConstants4.h>
#include <VSHelper4.h>

#include "decode.h"
#include "encode.h"
#include "subcarrier.h"

typedef struct comp_filter_t comp_filter_t;

struct comp_filter_t {
    VSNode *node;
    VSNode *orig_node;   /* Restore: the pre-encode picture at the raster */
    VSVideoInfo vi;
    int standard;
    int in_frames;
    comp_encode_t enc;
    comp_decode_t *dec;
};

/* Where the picture sits in the raster. A 480-line NTSC clip occupies
 * the 486-line BFF raster at rows 4..483 when its content is BFF (the
 * DV convention) or rows 5..484 when TFF (ATSC/RP 202) — the odd offset
 * re-aligns TFF content to the BFF raster. Field order comes from the
 * frame's _FieldBased prop; absent or progressive means BFF. */
static int comp_row_offset(const comp_filter_t *f, const VSFrame *frame, const VSAPI *vsapi)
{
    if (f->standard != COMP_STD_NTSC || f->vi.height != 480)
        return 0;

    int err;
    const int64_t fb = vsapi->mapGetInt(vsapi->getFramePropertiesRO(frame), "_FieldBased", 0, &err);
    return (!err && fb == VSC_FIELD_TOP) ? 5 : 4;
}

static const VSFrame *VS_CC comp_encode_get_frame(int n, int activation_reason, void *instance_data,
                                                  void **frame_data, VSFrameContext *frame_ctx,
                                                  VSCore *core, const VSAPI *vsapi)
{
    comp_filter_t *f = instance_data;
    (void)frame_data;

    if (activation_reason == arInitial) {
        vsapi->requestFrameFilter(n, f->node, frame_ctx);
        return NULL;
    }
    if (activation_reason != arAllFramesReady)
        return NULL;

    const VSFrame *src = vsapi->getFrameFilter(n, f->node, frame_ctx);
    VSFrame *dst = vsapi->newVideoFrame(&f->vi.format, f->vi.width, f->vi.height, src, core);

    const uint16_t *srcy = (const uint16_t *)vsapi->getReadPtr(src, 0);
    const uint16_t *srcu = (const uint16_t *)vsapi->getReadPtr(src, 1);
    const uint16_t *srcv = (const uint16_t *)vsapi->getReadPtr(src, 2);
    const ptrdiff_t ystride = vsapi->getStride(src, 0) / 2;
    const ptrdiff_t ustride = vsapi->getStride(src, 1) / 2;
    const ptrdiff_t vstride = vsapi->getStride(src, 2) / 2;
    uint16_t *dstp = (uint16_t *)vsapi->getWritePtr(dst, 0);
    const ptrdiff_t dstride = vsapi->getStride(dst, 0) / 2;

    comp_encode_frame(&f->enc, n, f->vi.height, comp_row_offset(f, src, vsapi),
                      dstp, dstride, srcy, ystride, srcu, ustride, srcv, vstride);

    vsapi->freeFrame(src);
    return dst;
}

#define COMP_MAX_LOOK COMP_T3D_LOOK

static const VSFrame *VS_CC comp_decode_get_frame(int n, int activation_reason, void *instance_data,
                                                  void **frame_data, VSFrameContext *frame_ctx,
                                                  VSCore *core, const VSAPI *vsapi)
{
    comp_filter_t *f = instance_data;
    const int look = comp_decode_look(f->dec);
    const int last = f->in_frames - 1;
    (void)frame_data;

    if (activation_reason == arInitial) {
        int prev = -1;
        for (int i = -look; i <= look; i++) {
            const int k = VSMIN(VSMAX(n + i, 0), last);
            if (k != prev)
                vsapi->requestFrameFilter(k, f->node, frame_ctx);
            prev = k;
        }
        if (f->orig_node)
            vsapi->requestFrameFilter(n, f->orig_node, frame_ctx);
        return NULL;
    }
    if (activation_reason != arAllFramesReady)
        return NULL;

    const VSFrame *srcs[2 * COMP_MAX_LOOK + 1];
    comp_frame_view_t views[2 * COMP_MAX_LOOK + 1];
    int view_frames[2 * COMP_MAX_LOOK + 1];
    for (int i = 0; i <= 2 * look; i++) {
        const int k = VSMIN(VSMAX(n - look + i, 0), last);
        srcs[i] = vsapi->getFrameFilter(k, f->node, frame_ctx);
        views[i].data = (const uint16_t *)vsapi->getReadPtr(srcs[i], 0);
        views[i].stride = vsapi->getStride(srcs[i], 0) / 2;
        view_frames[i] = k;
    }
    const VSFrame *src = srcs[look];
    const VSFrame *orig = f->orig_node ? vsapi->getFrameFilter(n, f->orig_node, frame_ctx) : NULL;
    VSFrame *dst = vsapi->newVideoFrame(&f->vi.format, f->vi.width, f->vi.height, src, core);

    comp_decode_frame(f->dec, n, f->in_frames, f->vi.height, comp_row_offset(f, src, vsapi),
                      views, view_frames, look,
                      orig ? (const uint16_t *)vsapi->getReadPtr(orig, 0) : NULL,
                      orig ? vsapi->getStride(orig, 0) / 2 : 0,
                      (uint16_t *)vsapi->getWritePtr(dst, 0), vsapi->getStride(dst, 0) / 2,
                      (uint16_t *)vsapi->getWritePtr(dst, 1), vsapi->getStride(dst, 1) / 2,
                      (uint16_t *)vsapi->getWritePtr(dst, 2), vsapi->getStride(dst, 2) / 2);

    if (orig)
        vsapi->freeFrame(orig);
    for (int i = 0; i <= 2 * look; i++)
        vsapi->freeFrame(srcs[i]);
    return dst;
}

static void VS_CC comp_free(void *instance_data, VSCore *core, const VSAPI *vsapi)
{
    comp_filter_t *f = instance_data;
    (void)core;
    if (f->dec) {
        comp_decode_free(f->dec);
        free(f->dec);
    }
    if (f->orig_node)
        vsapi->freeNode(f->orig_node);
    vsapi->freeNode(f->node);
    free(f);
}

#define RETERROR(x) do { \
    char msg[256]; \
    snprintf(msg, sizeof(msg), "%s: %s", name, x); \
    vsapi->mapSetError(out, msg); \
    if (d.node) \
        vsapi->freeNode(d.node); \
    return; \
} while (0)

static int comp_parse_standard(const VSMap *in, const VSAPI *vsapi, int *standard)
{
    int err;
    const char *s = vsapi->mapGetData(in, "standard", 0, &err);
    if (err)
        s = "pal";
    if (!strcmp(s, "pal"))
        *standard = COMP_STD_PAL;
    else if (!strcmp(s, "ntsc"))
        *standard = COMP_STD_NTSC;
    else
        return -1;
    return 0;
}

/* BT.601 horizontal anchors: the first active luma sample is 132 luma
 * clocks after 0H for 625-line systems and 122 for 525-line (BT.601-5
 * Part A). On the 0H-aligned 4xfsc raster the active window starts at
 * stored sample 182 (PAL) or 130 + 57/90 (NTSC, where 0H precedes
 * stored sample 0 by 57/90 of a sample). rho converts 4xfsc sample
 * counts into 13.5 MHz sample counts. */
#define RHO_PAL  (540000.0 / 709379.0)
#define RHO_NTSC (33.0 / 35.0)

static int comp_encode_width(int standard)
{
    return standard == COMP_STD_PAL ? COMP_ACTIVE_WIDTH_PAL : COMP_ACTIVE_WIDTH_NTSC;
}

static void VS_CC comp_encode_create(const VSMap *in, VSMap *out, void *user_data, VSCore *core, const VSAPI *vsapi)
{
    const char *name = user_data;
    comp_filter_t d = {0};
    int err;

    d.node = vsapi->mapGetNode(in, "clip", 0, NULL);
    d.vi = *vsapi->getVideoInfo(d.node);

    if (comp_parse_standard(in, vsapi, &d.standard))
        RETERROR("standard must be pal or ntsc");

    const int setup = !!vsapi->mapGetIntSaturated(in, "setup", 0, &err);
    const int precomb = !!vsapi->mapGetIntSaturated(in, "precomb", 0, &err);

    if (!vsh_isConstantVideoFormat(&d.vi))
        RETERROR("clip must have constant format and dimensions");
    if (d.vi.format.colorFamily != cfYUV)
        RETERROR("clip must be YUV");
    if (d.standard == COMP_STD_PAL) {
        if (d.vi.height != COMP_ACTIVE_HEIGHT_PAL)
            RETERROR("pal input must have 576 lines");
    } else {
        if (d.vi.height != 480 && d.vi.height != COMP_ACTIVE_HEIGHT_NTSC)
            RETERROR("ntsc input must have 480 or 486 lines");
    }

    /* resample to the 4xfsc active raster and 4:4:4, equating the
     * shared time base (see the anchors above); lines map 1:1 */
    VSPlugin *resize = vsapi->getPluginByID("com.vapoursynth.resize", core);
    if (!resize)
        RETERROR("resize plugin not found");

    const int pal = d.standard == COMP_STD_PAL;
    const int width = comp_encode_width(d.standard);
    const double rho = pal ? RHO_PAL : RHO_NTSC;
    const double active0 = pal ? 182.0 : 130.0 + 57.0 / 90.0;
    const double anchor601 = pal ? 132.0 : 122.0;
    const double scale = d.vi.width / 720.0;

    VSMap *args = vsapi->createMap();
    vsapi->mapConsumeNode(args, "clip", d.node, maReplace);
    d.node = NULL;
    vsapi->mapSetInt(args, "format", pfYUV444P16, maReplace);
    vsapi->mapSetInt(args, "width", width, maReplace);
    vsapi->mapSetInt(args, "height", d.vi.height, maReplace);
    vsapi->mapSetFloat(args, "src_left",
                       scale * ((active0 - 0.5) * rho - anchor601) + 0.5, maReplace);
    vsapi->mapSetFloat(args, "src_width", scale * (width * rho), maReplace);
    VSMap *ret = vsapi->invoke(resize, "Spline36", args);
    vsapi->freeMap(args);

    const char *invoke_err = vsapi->mapGetError(ret);
    if (invoke_err) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s: %s", name, invoke_err);
        vsapi->mapSetError(out, msg);
        vsapi->freeMap(ret);
        return;
    }
    d.node = vsapi->mapGetNode(ret, "clip", 0, NULL);
    vsapi->freeMap(ret);

    comp_encode_init(&d.enc, d.standard, setup, precomb);
    vsapi->queryVideoFormat(&d.vi.format, cfGray, stInteger, 16, 0, 0, core);
    d.vi.width = width;

    comp_filter_t *data = malloc(sizeof(*data));
    *data = d;

    VSFilterDependency deps[] = {{ data->node, rpStrictSpatial }};
    vsapi->createVideoFilter(out, name, &data->vi, comp_encode_get_frame, comp_free,
                             fmParallel, deps, 1, data, core);
}

static void VS_CC comp_decode_create(const VSMap *in, VSMap *out, void *user_data, VSCore *core, const VSAPI *vsapi)
{
    const char *name = user_data;
    comp_filter_t d = {0};
    int err;

    d.node = vsapi->mapGetNode(in, "clip", 0, NULL);
    d.vi = *vsapi->getVideoInfo(d.node);

    if (comp_parse_standard(in, vsapi, &d.standard))
        RETERROR("standard must be pal or ntsc");

    int width = vsapi->mapGetIntSaturated(in, "width", 0, &err);
    if (err)
        width = 720;
    if (width < 16 || width > 8192)
        RETERROR("width must be between 16 and 8192");

    double threshold = vsapi->mapGetFloat(in, "threshold", 0, &err);
    if (err)
        threshold = 0.4;
    if (!(threshold > 0.0 && threshold <= 1.0))
        RETERROR("threshold must be in (0, 1]");

    const int setup = !!vsapi->mapGetIntSaturated(in, "setup", 0, &err);

    int dimensions = vsapi->mapGetIntSaturated(in, "dimensions", 0, &err);
    if (err)
        dimensions = 2;
    if (dimensions < 1 || dimensions > 3)
        RETERROR("dimensions must be 1, 2 or 3");

    int eq = vsapi->mapGetIntSaturated(in, "eq", 0, &err);
    if (err)
        eq = 1;
    if (eq < 0 || eq > 2)
        RETERROR("eq must be 0 (off), 1 (fixed) or 2 (leak-aware)");





    int transform = vsapi->mapGetIntSaturated(in, "transform", 0, &err);
    if (err)
        transform = 0;
    if (transform < 0 || transform > 2)
        RETERROR("transform must be 0 (comb), 1 (transform) or 2 (hybrid)");
    if (transform && d.standard != COMP_STD_NTSC)
        RETERROR("transform applies to ntsc (pal always uses the transform)");
    if (transform && dimensions != 3)
        RETERROR("transform needs dimensions=3");
    if (eq == 2 && !(d.standard == COMP_STD_PAL ? dimensions >= 2 : transform))
        RETERROR("eq=2 needs a transform separation");

    int level = vsapi->mapGetIntSaturated(in, "level", 0, &err);
    if (err)
        level = 0;
    if (level && dimensions < 2)
        RETERROR("level=1 needs dimensions 2 or 3");
    if (level && d.standard == COMP_STD_NTSC && !transform)
        RETERROR("level=1 needs a transform separation for ntsc");

    const int nlut = vsapi->mapNumElements(in, "lut");
    if (nlut > 0) {
        if (d.standard == COMP_STD_NTSC && !transform)
            RETERROR("lut needs a transform separation");
        if (level)
            RETERROR("lut and level are mutually exclusive");
        if (dimensions == 2 && nlut != COMP_T2D_NTHRESH * COMP_LUT_K)
            RETERROR("dimensions=2 needs 1280 lut values (80 bins x 16 knots)");
        if (dimensions == 3 && nlut != COMP_T3D_NTHRESH * COMP_LUT_K)
            RETERROR("dimensions=3 needs 12288 lut values (768 bins x 16 knots)");
        if (dimensions == 1)
            RETERROR("lut needs dimensions 2 or 3");
        if (vsapi->mapNumElements(in, "thresholds") > 0)
            RETERROR("lut and thresholds are mutually exclusive");
    }

    if (!vsh_isConstantVideoFormat(&d.vi))
        RETERROR("clip must have constant format and dimensions");
    if (d.vi.format.colorFamily != cfGray || d.vi.format.sampleType != stInteger
        || d.vi.format.bitsPerSample != 16)
        RETERROR("clip must be GRAY16 composite");
    if (d.standard == COMP_STD_PAL) {
        if (d.vi.width != COMP_ACTIVE_WIDTH_PAL || d.vi.height != COMP_ACTIVE_HEIGHT_PAL)
            RETERROR("pal composite input must be 928x576");
    } else {
        if (d.vi.width != COMP_ACTIVE_WIDTH_NTSC
            || (d.vi.height != 480 && d.vi.height != COMP_ACTIVE_HEIGHT_NTSC))
            RETERROR("ntsc composite input must be 758x480 or 758x486");
    }

    VSPlugin *resize = vsapi->getPluginByID("com.vapoursynth.resize", core);
    if (!resize)
        RETERROR("resize plugin not found");

    VSCoreInfo info;
    vsapi->getCoreInfo(core, &info);

    d.dec = malloc(sizeof(*d.dec));
    if (!d.dec)
        RETERROR("out of memory");
    if (comp_decode_init(d.dec, d.standard, threshold,
                         info.numThreads < 1 ? 1 : info.numThreads, setup,
                         dimensions, eq, 0, transform, level)) {
        free(d.dec);
        d.dec = NULL;
        RETERROR("decoder initialisation failed");
    }

    const int nthresh = vsapi->mapNumElements(in, "thresholds");
    if (nthresh > 0) {
        if (d.standard == COMP_STD_NTSC && !transform)
            RETERROR("thresholds needs a transform separation for ntsc");
        if (level)
            RETERROR("thresholds needs the threshold mode (level=0)");
        if (dimensions == 2 && nthresh != COMP_T2D_NTHRESH)
            RETERROR("dimensions=2 needs 80 thresholds");
        if (dimensions == 3 && nthresh != COMP_T3D_NTHRESH)
            RETERROR("dimensions=3 needs 768 thresholds");
        if (dimensions == 1)
            RETERROR("thresholds needs dimensions 2 or 3");
        const double *tv = vsapi->mapGetFloatArray(in, "thresholds", &err);
        for (int i = 0; i < nthresh; i++)
            if (!(tv[i] > 0.0 && tv[i] <= 1.0))
                RETERROR("thresholds must be in (0, 1]");
        comp_decode_set_thresholds(d.dec, tv, nthresh);
    }

    if (nlut > 0) {
        const double *lv = vsapi->mapGetFloatArray(in, "lut", &err);
        for (int i = 0; i < nlut; i++)
            if (!(lv[i] >= 0.0 && lv[i] <= 1.0))
                RETERROR("lut values must be in [0, 1]");
        comp_decode_set_lut(d.dec, lv, nlut);
    }

    vsapi->queryVideoFormat(&d.vi.format, cfYUV, stInteger, 16, 0, 0, core);
    d.in_frames = d.vi.numFrames;

    comp_filter_t *data = malloc(sizeof(*data));
    *data = d;

    VSFilterDependency deps[] = {{ data->node, dimensions == 3 ? rpGeneral : rpStrictSpatial }};
    VSNode *dec_node = vsapi->createVideoFilter2(name, &data->vi, comp_decode_get_frame, comp_free,
                                                 fmParallel, deps, 1, data, core);
    if (!dec_node) {
        vsapi->mapSetError(out, "Decode: failed to create filter");
        return;
    }

    /* resample back to the BT.601 raster: the exact inverse of Encode's
     * mapping. The 601 window is wider in time than the active raster,
     * so the outermost samples come from edge extension. */
    const int pal = d.standard == COMP_STD_PAL;
    const double rho = pal ? RHO_PAL : RHO_NTSC;
    const double active0 = pal ? 182.0 : 130.0 + 57.0 / 90.0;
    const double anchor601 = pal ? 132.0 : 122.0;
    VSMap *args = vsapi->createMap();
    vsapi->mapConsumeNode(args, "clip", dec_node, maReplace);
    vsapi->mapSetInt(args, "width", width, maReplace);
    vsapi->mapSetInt(args, "height", d.vi.height, maReplace);
    vsapi->mapSetFloat(args, "src_left",
                       anchor601 / rho - (active0 - 0.5) - 0.5 * (720.0 / width) / rho, maReplace);
    vsapi->mapSetFloat(args, "src_width", 720.0 / rho, maReplace);
    VSMap *ret = vsapi->invoke(resize, "Spline36", args);
    vsapi->freeMap(args);

    const char *invoke_err = vsapi->mapGetError(ret);
    if (invoke_err) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s: %s", name, invoke_err);
        vsapi->mapSetError(out, msg);
        vsapi->freeMap(ret);
        return;
    }
    VSNode *res_node = vsapi->mapGetNode(ret, "clip", 0, NULL);
    vsapi->freeMap(ret);
    vsapi->mapConsumeNode(out, "clip", res_node, maReplace);
}


/* Restore(): the whole noise-reduction round trip in one filter, with
 * optional Y-only Landweber refinement anchored to the input picture
 * (which Decode alone cannot see). */
static void VS_CC comp_restore_create(const VSMap *in, VSMap *out, void *user_data, VSCore *core, const VSAPI *vsapi)
{
    const char *name = user_data;
    comp_filter_t d = {0};
    int err;

    d.node = vsapi->mapGetNode(in, "clip", 0, NULL);
    d.vi = *vsapi->getVideoInfo(d.node);
    const int src_width = d.vi.width;

    if (comp_parse_standard(in, vsapi, &d.standard))
        RETERROR("standard must be pal or ntsc");
    const int setup = !!vsapi->mapGetIntSaturated(in, "setup", 0, &err);

    int width = vsapi->mapGetIntSaturated(in, "width", 0, &err);
    if (err)
        width = src_width;
    if (width < 16 || width > 8192)
        RETERROR("width must be between 16 and 8192");

    double threshold = vsapi->mapGetFloat(in, "threshold", 0, &err);
    if (err)
        threshold = 0.4;
    if (!(threshold > 0.0 && threshold <= 1.0))
        RETERROR("threshold must be in (0, 1]");

    int dimensions = vsapi->mapGetIntSaturated(in, "dimensions", 0, &err);
    if (err)
        dimensions = 2;
    if (dimensions < 1 || dimensions > 3)
        RETERROR("dimensions must be 1, 2 or 3");

    int eq = vsapi->mapGetIntSaturated(in, "eq", 0, &err);
    if (err)
        eq = 1;
    if (eq < 0 || eq > 2)
        RETERROR("eq must be 0 (off), 1 (fixed) or 2 (leak-aware)");

    int refine = vsapi->mapGetIntSaturated(in, "refine", 0, &err);
    if (err)
        refine = 1;
    if (refine < 0 || refine > 16)
        RETERROR("refine must be between 0 and 16");

    const int precomb = !!vsapi->mapGetIntSaturated(in, "precomb", 0, &err);

    int transform = vsapi->mapGetIntSaturated(in, "transform", 0, &err);
    if (err)
        transform = 0;
    if (transform < 0 || transform > 2)
        RETERROR("transform must be 0 (comb), 1 (transform) or 2 (hybrid)");
    if (transform && d.standard != COMP_STD_NTSC)
        RETERROR("transform applies to ntsc (pal always uses the transform)");
    if (transform && dimensions != 3)
        RETERROR("transform needs dimensions=3");
    if (eq == 2 && !(d.standard == COMP_STD_PAL ? dimensions >= 2 : transform))
        RETERROR("eq=2 needs a transform separation");

    int level = vsapi->mapGetIntSaturated(in, "level", 0, &err);
    if (err)
        level = 0;
    if (level && dimensions < 2)
        RETERROR("level=1 needs dimensions 2 or 3");
    if (level && d.standard == COMP_STD_NTSC && !transform)
        RETERROR("level=1 needs a transform separation for ntsc");

    const int nlut = vsapi->mapNumElements(in, "lut");
    if (nlut > 0) {
        if (d.standard == COMP_STD_NTSC && !transform)
            RETERROR("lut needs a transform separation");
        if (level)
            RETERROR("lut and level are mutually exclusive");
        if (dimensions == 2 && nlut != COMP_T2D_NTHRESH * COMP_LUT_K)
            RETERROR("dimensions=2 needs 1280 lut values (80 bins x 16 knots)");
        if (dimensions == 3 && nlut != COMP_T3D_NTHRESH * COMP_LUT_K)
            RETERROR("dimensions=3 needs 12288 lut values (768 bins x 16 knots)");
        if (dimensions == 1)
            RETERROR("lut needs dimensions 2 or 3");
        if (vsapi->mapNumElements(in, "thresholds") > 0)
            RETERROR("lut and thresholds are mutually exclusive");
    }

    if (!vsh_isConstantVideoFormat(&d.vi))
        RETERROR("clip must have constant format and dimensions");
    if (d.vi.format.colorFamily != cfYUV)
        RETERROR("clip must be YUV");
    if (d.standard == COMP_STD_PAL) {
        if (d.vi.height != COMP_ACTIVE_HEIGHT_PAL)
            RETERROR("pal input must have 576 lines");
    } else {
        if (d.vi.height != 480 && d.vi.height != COMP_ACTIVE_HEIGHT_NTSC)
            RETERROR("ntsc input must have 480 or 486 lines");
    }

    VSPlugin *resize = vsapi->getPluginByID("com.vapoursynth.resize", core);
    if (!resize)
        RETERROR("resize plugin not found");

    /* stage 1: the picture on the 4xfsc raster (also the refine anchor) */
    const int pal = d.standard == COMP_STD_PAL;
    const int rwidth = comp_encode_width(d.standard);
    const double rho = pal ? RHO_PAL : RHO_NTSC;
    const double active0 = pal ? 182.0 : 130.0 + 57.0 / 90.0;
    const double anchor601 = pal ? 132.0 : 122.0;
    const double scale = src_width / 720.0;

    VSMap *args = vsapi->createMap();
    vsapi->mapConsumeNode(args, "clip", d.node, maReplace);
    d.node = NULL;
    vsapi->mapSetInt(args, "format", pfYUV444P16, maReplace);
    vsapi->mapSetInt(args, "width", rwidth, maReplace);
    vsapi->mapSetInt(args, "height", d.vi.height, maReplace);
    vsapi->mapSetFloat(args, "src_left",
                       scale * ((active0 - 0.5) * rho - anchor601) + 0.5, maReplace);
    vsapi->mapSetFloat(args, "src_width", scale * (rwidth * rho), maReplace);
    VSMap *ret = vsapi->invoke(resize, "Spline36", args);
    vsapi->freeMap(args);

    const char *invoke_err = vsapi->mapGetError(ret);
    if (invoke_err) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s: %s", name, invoke_err);
        vsapi->mapSetError(out, msg);
        vsapi->freeMap(ret);
        return;
    }
    VSNode *raster = vsapi->mapGetNode(ret, "clip", 0, NULL);
    vsapi->freeMap(ret);

    /* stage 2: modulator over the raster picture */
    comp_filter_t *edata = malloc(sizeof(*edata));
    memset(edata, 0, sizeof(*edata));
    edata->node = raster;
    edata->vi = *vsapi->getVideoInfo(raster);
    edata->standard = d.standard;
    comp_encode_init(&edata->enc, d.standard, setup, precomb);
    vsapi->queryVideoFormat(&edata->vi.format, cfGray, stInteger, 16, 0, 0, core);

    VSFilterDependency edeps[] = {{ raster, rpStrictSpatial }};
    VSNode *modulated = vsapi->createVideoFilter2(name, &edata->vi, comp_encode_get_frame, comp_free,
                                                  fmParallel, edeps, 1, edata, core);
    if (!modulated) {
        vsapi->mapSetError(out, "Restore: failed to create modulator");
        return;
    }

    /* stage 3: decoder with the raster picture as the refine anchor */
    d.node = modulated;
    d.orig_node = vsapi->addNodeRef(raster);
    d.vi = *vsapi->getVideoInfo(modulated);

    VSCoreInfo info;
    vsapi->getCoreInfo(core, &info);
    d.dec = malloc(sizeof(*d.dec));
    if (!d.dec)
        RETERROR("out of memory");
    if (comp_decode_init(d.dec, d.standard, threshold,
                         info.numThreads < 1 ? 1 : info.numThreads, setup,
                         dimensions, eq, refine, transform, level)) {
        free(d.dec);
        d.dec = NULL;
        RETERROR("decoder initialisation failed");
    }

    const int nthresh = vsapi->mapNumElements(in, "thresholds");
    if (nthresh > 0) {
        if (d.standard == COMP_STD_NTSC && !transform)
            RETERROR("thresholds needs a transform separation for ntsc");
        if (level)
            RETERROR("thresholds needs the threshold mode (level=0)");
        if (dimensions == 2 && nthresh != COMP_T2D_NTHRESH)
            RETERROR("dimensions=2 needs 80 thresholds");
        if (dimensions == 3 && nthresh != COMP_T3D_NTHRESH)
            RETERROR("dimensions=3 needs 768 thresholds");
        if (dimensions == 1)
            RETERROR("thresholds needs dimensions 2 or 3");
        const double *tv = vsapi->mapGetFloatArray(in, "thresholds", &err);
        for (int i = 0; i < nthresh; i++)
            if (!(tv[i] > 0.0 && tv[i] <= 1.0))
                RETERROR("thresholds must be in (0, 1]");
        comp_decode_set_thresholds(d.dec, tv, nthresh);
    }

    if (nlut > 0) {
        const double *lv = vsapi->mapGetFloatArray(in, "lut", &err);
        for (int i = 0; i < nlut; i++)
            if (!(lv[i] >= 0.0 && lv[i] <= 1.0))
                RETERROR("lut values must be in [0, 1]");
        comp_decode_set_lut(d.dec, lv, nlut);
    }

    vsapi->queryVideoFormat(&d.vi.format, cfYUV, stInteger, 16, 0, 0, core);
    d.in_frames = d.vi.numFrames;

    comp_filter_t *data = malloc(sizeof(*data));
    *data = d;

    VSFilterDependency ddeps[] = {{ data->node, dimensions == 3 ? rpGeneral : rpStrictSpatial },
                                  { data->orig_node, rpStrictSpatial }};
    VSNode *dec_node = vsapi->createVideoFilter2(name, &data->vi, comp_decode_get_frame, comp_free,
                                                 fmParallel, ddeps, 2, data, core);
    if (!dec_node) {
        vsapi->mapSetError(out, "Restore: failed to create filter");
        return;
    }

    /* stage 4: back to the caller's raster */
    args = vsapi->createMap();
    vsapi->mapConsumeNode(args, "clip", dec_node, maReplace);
    vsapi->mapSetInt(args, "width", width, maReplace);
    vsapi->mapSetInt(args, "height", d.vi.height, maReplace);
    vsapi->mapSetFloat(args, "src_left",
                       anchor601 / rho - (active0 - 0.5) - 0.5 * (720.0 / width) / rho, maReplace);
    vsapi->mapSetFloat(args, "src_width", 720.0 / rho, maReplace);
    ret = vsapi->invoke(resize, "Spline36", args);
    vsapi->freeMap(args);

    invoke_err = vsapi->mapGetError(ret);
    if (invoke_err) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s: %s", name, invoke_err);
        vsapi->mapSetError(out, msg);
        vsapi->freeMap(ret);
        return;
    }
    VSNode *res_node = vsapi->mapGetNode(ret, "clip", 0, NULL);
    vsapi->freeMap(ret);
    vsapi->mapConsumeNode(out, "clip", res_node, maReplace);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi)
{
    vspapi->configPlugin("com.ifb.composite", "composite",
                         "PAL/NTSC composite video encoder/decoder",
                         VS_MAKE_VERSION(0, 1), VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("Encode",
                             "clip:vnode;"
                             "standard:data:opt;"
                             "setup:int:opt;"
                             "precomb:int:opt;",
                             "clip:vnode;",
                             comp_encode_create, (void *)"Encode", plugin);
    vspapi->registerFunction("Decode",
                             "clip:vnode;"
                             "standard:data:opt;"
                             "width:int:opt;"
                             "threshold:float:opt;"
                             "setup:int:opt;"
                             "dimensions:int:opt;"
                             "eq:int:opt;"
                             "thresholds:float[]:opt;"
                             "transform:int:opt;"
                             "level:int:opt;"
                             "lut:float[]:opt;",
                             "clip:vnode;",
                             comp_decode_create, (void *)"Decode", plugin);
    vspapi->registerFunction("Restore",
                             "clip:vnode;"
                             "standard:data:opt;"
                             "width:int:opt;"
                             "threshold:float:opt;"
                             "setup:int:opt;"
                             "dimensions:int:opt;"
                             "eq:int:opt;"
                             "refine:int:opt;"
                             "thresholds:float[]:opt;"
                             "precomb:int:opt;"
                             "transform:int:opt;"
                             "level:int:opt;"
                             "lut:float[]:opt;",
                             "clip:vnode;",
                             comp_restore_create, (void *)"Restore", plugin);
}
