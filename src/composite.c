/*
 * PAL/NTSC composite video encode/decode for VapourSynth.
 *
 * Encode() and Decode() are currently passthrough stubs: they validate
 * arguments and plumb frames through unchanged.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <VapourSynth4.h>
#include <VSHelper4.h>

enum {
    COMP_STD_PAL,
    COMP_STD_NTSC,
};

typedef struct comp_filter_t comp_filter_t;

struct comp_filter_t {
    VSNode *node;
    VSVideoInfo vi;
    int standard;
};

static const VSFrame *VS_CC comp_get_frame(int n, int activation_reason, void *instance_data,
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
    char msg[128]; \
    snprintf(msg, sizeof(msg), "%s: %s", name, x); \
    vsapi->mapSetError(out, msg); \
    vsapi->freeNode(d.node); \
    return; \
} while (0)

static void VS_CC comp_create(const VSMap *in, VSMap *out, void *user_data, VSCore *core, const VSAPI *vsapi)
{
    const char *name = user_data;
    comp_filter_t d = {0};
    int err;

    d.node = vsapi->mapGetNode(in, "clip", 0, NULL);
    d.vi = *vsapi->getVideoInfo(d.node);

    const char *standard = vsapi->mapGetData(in, "standard", 0, &err);
    if (err)
        standard = "pal";
    if (!strcmp(standard, "pal"))
        d.standard = COMP_STD_PAL;
    else if (!strcmp(standard, "ntsc"))
        d.standard = COMP_STD_NTSC;
    else
        RETERROR("standard must be pal or ntsc");

    if (!vsh_isConstantVideoFormat(&d.vi))
        RETERROR("clip must have constant format and dimensions");

    comp_filter_t *data = malloc(sizeof(*data));
    *data = d;

    VSFilterDependency deps[] = {{ data->node, rpStrictSpatial }};
    vsapi->createVideoFilter(out, name, &data->vi, comp_get_frame, comp_free,
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
                             comp_create, (void *)"Encode", plugin);
    vspapi->registerFunction("Decode",
                             "clip:vnode;"
                             "standard:data:opt;",
                             "clip:vnode;",
                             comp_create, (void *)"Decode", plugin);
}
