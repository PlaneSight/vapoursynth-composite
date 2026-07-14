/*
 * PAL/NTSC composite video encode/decode for VapourSynth.
 *
 * Encode() modulates YCbCr to composite (PAL only so far).
 * Decode() is a passthrough stub until M4.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <VapourSynth4.h>
#include <VSHelper4.h>

#include "encode.h"
#include "subcarrier.h"

typedef struct comp_filter_t comp_filter_t;

struct comp_filter_t {
    VSNode *node;
    VSVideoInfo vi;
    int standard;
    comp_encode_t enc;
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

static const VSFrame *VS_CC comp_passthrough_get_frame(int n, int activation_reason, void *instance_data,
                                                       void **frame_data, VSFrameContext *frame_ctx,
                                                       VSCore *core, const VSAPI *vsapi)
{
    comp_filter_t *f = instance_data;
    (void)frame_data;
    (void)core;

    if (activation_reason == arInitial) {
        vsapi->requestFrameFilter(n, f->node, frame_ctx);
        return NULL;
    }
    if (activation_reason != arAllFramesReady)
        return NULL;

    return vsapi->getFrameFilter(n, f->node, frame_ctx);
}

static void VS_CC comp_free(void *instance_data, VSCore *core, const VSAPI *vsapi)
{
    comp_filter_t *f = instance_data;
    (void)core;
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

    comp_encode_init(&d.enc, d.standard);
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

    d.node = vsapi->mapGetNode(in, "clip", 0, NULL);
    d.vi = *vsapi->getVideoInfo(d.node);

    if (comp_parse_standard(in, vsapi, &d.standard))
        RETERROR("standard must be pal or ntsc");

    if (!vsh_isConstantVideoFormat(&d.vi))
        RETERROR("clip must have constant format and dimensions");

    comp_filter_t *data = malloc(sizeof(*data));
    *data = d;

    VSFilterDependency deps[] = {{ data->node, rpStrictSpatial }};
    vsapi->createVideoFilter(out, name, &data->vi, comp_passthrough_get_frame, comp_free,
                             fmParallel, deps, 1, data, core);
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
                             "standard:data:opt;",
                             "clip:vnode;",
                             comp_decode_create, (void *)"Decode", plugin);
}
