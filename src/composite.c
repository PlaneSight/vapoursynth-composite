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
#include <VSHelper4.h>

#include "decode.h"
#include "encode.h"
#include "subcarrier.h"

typedef struct comp_filter_t comp_filter_t;

struct comp_filter_t {
    VSNode *node;
    VSVideoInfo vi;
    int standard;
    comp_encode_t enc;
    comp_decode_t *dec;
};

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

    for (int r = 0; r < f->vi.height; r++)
        comp_encode_line(&f->enc, dstp + r * dstride,
                         srcy + r * ystride, srcu + r * ustride, srcv + r * vstride,
                         comp_sc_line(f->standard, n, r));

    vsapi->freeFrame(src);
    return dst;
}

static const VSFrame *VS_CC comp_decode_get_frame(int n, int activation_reason, void *instance_data,
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

    comp_decode_frame(f->dec, n, f->vi.height, 0,
                      (const uint16_t *)vsapi->getReadPtr(src, 0), vsapi->getStride(src, 0) / 2,
                      (uint16_t *)vsapi->getWritePtr(dst, 0), vsapi->getStride(dst, 0) / 2,
                      (uint16_t *)vsapi->getWritePtr(dst, 1), vsapi->getStride(dst, 1) / 2,
                      (uint16_t *)vsapi->getWritePtr(dst, 2), vsapi->getStride(dst, 2) / 2);

    vsapi->freeFrame(src);
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

static void VS_CC comp_encode_create(const VSMap *in, VSMap *out, void *user_data, VSCore *core, const VSAPI *vsapi)
{
    const char *name = user_data;
    comp_filter_t d = {0};

    d.node = vsapi->mapGetNode(in, "clip", 0, NULL);
    d.vi = *vsapi->getVideoInfo(d.node);

    if (comp_parse_standard(in, vsapi, &d.standard))
        RETERROR("standard must be pal or ntsc");
    if (d.standard != COMP_STD_PAL)
        RETERROR("ntsc is not implemented yet");

    if (!vsh_isConstantVideoFormat(&d.vi))
        RETERROR("clip must have constant format and dimensions");
    if (d.vi.format.colorFamily != cfYUV)
        RETERROR("clip must be YUV");
    if (d.vi.height != COMP_ACTIVE_HEIGHT_PAL)
        RETERROR("pal input must have 576 lines");

    /* resample to the 4xfsc active raster and 4:4:4. The mapping equates
     * the shared time base: BT.601 puts the first active luma sample 132
     * clocks after 0H (BT.601-5 Part A); the 0H-aligned 4xfsc line has
     * the 928-sample active window starting at sample 182. */
    VSPlugin *resize = vsapi->getPluginByID("com.vapoursynth.resize", core);
    if (!resize)
        RETERROR("resize plugin not found");

    const double rho = 540000.0 / 709379.0;  /* 13.5 MHz / PAL 4xfsc */
    const double scale = d.vi.width / 720.0;
    VSMap *args = vsapi->createMap();
    vsapi->mapConsumeNode(args, "clip", d.node, maReplace);
    d.node = NULL;
    vsapi->mapSetInt(args, "format", pfYUV444P16, maReplace);
    vsapi->mapSetInt(args, "width", COMP_ACTIVE_WIDTH_PAL, maReplace);
    vsapi->mapSetInt(args, "height", COMP_ACTIVE_HEIGHT_PAL, maReplace);
    vsapi->mapSetFloat(args, "src_left", scale * (181.5 * rho - 132.0) + 0.5, maReplace);
    vsapi->mapSetFloat(args, "src_width", scale * (COMP_ACTIVE_WIDTH_PAL * rho), maReplace);
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

    comp_encode_init(&d.enc, d.standard, 0);
    vsapi->queryVideoFormat(&d.vi.format, cfGray, stInteger, 16, 0, 0, core);
    d.vi.width = COMP_ACTIVE_WIDTH_PAL;
    d.vi.height = COMP_ACTIVE_HEIGHT_PAL;

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
    if (d.standard != COMP_STD_PAL)
        RETERROR("ntsc is not implemented yet");

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

    if (!vsh_isConstantVideoFormat(&d.vi))
        RETERROR("clip must have constant format and dimensions");
    if (d.vi.format.colorFamily != cfGray || d.vi.format.sampleType != stInteger
        || d.vi.format.bitsPerSample != 16)
        RETERROR("clip must be GRAY16 composite");
    if (d.vi.width != COMP_ACTIVE_WIDTH_PAL || d.vi.height != COMP_ACTIVE_HEIGHT_PAL)
        RETERROR("pal composite input must be 928x576");

    VSPlugin *resize = vsapi->getPluginByID("com.vapoursynth.resize", core);
    if (!resize)
        RETERROR("resize plugin not found");

    VSCoreInfo info;
    vsapi->getCoreInfo(core, &info);

    d.dec = malloc(sizeof(*d.dec));
    if (!d.dec)
        RETERROR("out of memory");
    if (comp_decode_init(d.dec, d.standard, threshold,
                         info.numThreads < 1 ? 1 : info.numThreads, 0)) {
        free(d.dec);
        d.dec = NULL;
        RETERROR("decoder initialisation failed");
    }

    vsapi->queryVideoFormat(&d.vi.format, cfYUV, stInteger, 16, 0, 0, core);

    comp_filter_t *data = malloc(sizeof(*data));
    *data = d;

    VSFilterDependency deps[] = {{ data->node, rpStrictSpatial }};
    VSNode *dec_node = vsapi->createVideoFilter2(name, &data->vi, comp_decode_get_frame, comp_free,
                                                 fmParallel, deps, 1, data, core);
    if (!dec_node) {
        vsapi->mapSetError(out, "Decode: failed to create filter");
        return;
    }

    /* resample back to the BT.601 raster: the exact inverse of Encode's
     * mapping. The 601 window is wider in time than the active raster,
     * so the outermost samples come from edge extension. */
    const double rho = 540000.0 / 709379.0;  /* 13.5 MHz / PAL 4xfsc */
    VSMap *args = vsapi->createMap();
    vsapi->mapConsumeNode(args, "clip", dec_node, maReplace);
    vsapi->mapSetInt(args, "width", width, maReplace);
    vsapi->mapSetInt(args, "height", COMP_ACTIVE_HEIGHT_PAL, maReplace);
    vsapi->mapSetFloat(args, "src_left",
                       132.0 / rho - 181.5 - 0.5 * (720.0 / width) / rho, maReplace);
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

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi)
{
    vspapi->configPlugin("com.ifb.composite", "composite",
                         "PAL/NTSC composite video encoder/decoder",
                         VS_MAKE_VERSION(0, 1), VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("Encode",
                             "clip:vnode;"
                             "standard:data:opt;",
                             "clip:vnode;",
                             comp_encode_create, (void *)"Encode", plugin);
    vspapi->registerFunction("Decode",
                             "clip:vnode;"
                             "standard:data:opt;"
                             "width:int:opt;"
                             "threshold:float:opt;",
                             "clip:vnode;",
                             comp_decode_create, (void *)"Decode", plugin);
}
