/*
 * PAL/NTSC composite video encoder for AviSynth+.
 *
 * Encode() modulates YCbCr to composite (GRAY16). It mirrors the
 * VapourSynth frontend (composite.c): it resamples the input to the
 * 4xfsc active raster and 4:4:4 through zimg (avsresize's z_ConvertFormat,
 * the same resampler VapourSynth uses), sharing the geometry with the VS
 * plugin via comp_encode_resample_params so both produce the same raster.
 *
 * This is a C-interface plugin: it exports avisynth_c_plugin_init and
 * resolves the avs_* API by ordinary dynamic linking against the host.
 */

#include <stdlib.h>
#include <string.h>

#include "avisynth_c.h"

#include "encode.h"
#include "geom.h"
#include "subcarrier.h"

typedef struct comp_avs_t comp_avs_t;

struct comp_avs_t {
    comp_encode_t enc;
    int standard;
    int height;   /* raster height of the resampled 4:4:4 clip */
};

/* Where the picture sits in the raster, matching composite.c: a 480-line
 * NTSC clip occupies the 486-line raster at rows 4..483 (BFF, the DV
 * convention) or 5..484 (TFF). Field order comes from the frame's
 * _FieldBased prop (2 = TFF, as in VapourSynth); absent or progressive
 * means BFF. */
static int comp_avs_row_offset(const comp_avs_t *f, AVS_ScriptEnvironment *env,
                               const AVS_VideoFrame *frame)
{
    if (f->standard != COMP_STD_NTSC || f->height != 480)
        return 0;

    const AVS_Map *props = avs_get_frame_props_ro(env, frame);
    int err = 0;
    const int64_t fb = avs_prop_get_int(env, props, "_FieldBased", 0, &err);
    return (!err && fb == 2) ? 5 : 4; /* 2 = TFF, matching VapourSynth */
}

static AVS_VideoFrame *AVSC_CC comp_avs_get_frame(AVS_FilterInfo *fi, int n)
{
    comp_avs_t *f = fi->user_data;
    AVS_ScriptEnvironment *env = fi->env;

    AVS_VideoFrame *src = avs_get_frame(fi->child, n);
    if (!src)
        return NULL;

    AVS_VideoFrame *dst = avs_new_video_frame_p(env, &fi->vi, src);

    const uint16_t *srcy = (const uint16_t *)avs_get_read_ptr_p(src, AVS_PLANAR_Y);
    const uint16_t *srcu = (const uint16_t *)avs_get_read_ptr_p(src, AVS_PLANAR_U);
    const uint16_t *srcv = (const uint16_t *)avs_get_read_ptr_p(src, AVS_PLANAR_V);
    const ptrdiff_t ystride = avs_get_pitch_p(src, AVS_PLANAR_Y) / 2;
    const ptrdiff_t ustride = avs_get_pitch_p(src, AVS_PLANAR_U) / 2;
    const ptrdiff_t vstride = avs_get_pitch_p(src, AVS_PLANAR_V) / 2;
    uint16_t *dstp = (uint16_t *)avs_get_write_ptr_p(dst, AVS_PLANAR_Y);
    const ptrdiff_t dstride = avs_get_pitch_p(dst, AVS_PLANAR_Y) / 2;

    comp_encode_frame(&f->enc, n, f->height, comp_avs_row_offset(f, env, src),
                      dstp, dstride, srcy, ystride, srcu, ustride, srcv, vstride);

    avs_release_video_frame(src);
    return dst;
}

static int AVSC_CC comp_avs_set_cache_hints(AVS_FilterInfo *fi, int cachehints,
                                            int frame_range)
{
    (void)fi; (void)frame_range;
    /* the encoder state is read-only after init and the frame callback
     * writes only its own output frame, so one shared instance is
     * reentrant across the host's thread pool */
    return cachehints == AVS_CACHE_GET_MTMODE ? AVS_MT_NICE_FILTER : 0;
}

static void AVSC_CC comp_avs_free(AVS_FilterInfo *fi)
{
    free(fi->user_data);
}

/* Invoke avsresize's z_ConvertFormat to resample `clip` to the 4xfsc
 * active raster and YUV444P16, with the shared subpixel crop. Returns an
 * error AVS_Value on failure (including avsresize not installed). */
static AVS_Value comp_avs_resample(AVS_ScriptEnvironment *env, AVS_Value clip,
                                   int in_width, int in_height, int standard)
{
    if (!avs_function_exists(env, "z_ConvertFormat"))
        return avs_new_value_error(
            "Encode: avsresize (z_ConvertFormat) is required but not loaded");

    int width;
    double src_left, src_width;
    comp_encode_resample_params(standard, in_width, &width, &src_left, &src_width);

    /* z_ConvertFormat(clip, width=, height=, pixel_type=, src_left=,
     * src_width=): one zimg graph does the 4:4:4 + 16-bit conversion and
     * the subpixel crop-resize, the same fused operation the VS plugin
     * gets from resize.Spline36. `clip` is a non-owning element copy;
     * avs_invoke does not consume it. */
    AVS_Value args[6];
    const char *names[6];
    args[0] = clip;                                  names[0] = NULL;
    args[1] = avs_new_value_int(width);              names[1] = "width";
    args[2] = avs_new_value_int(in_height);          names[2] = "height";
    args[3] = avs_new_value_string("YUV444P16");     names[3] = "pixel_type";
    args[4] = avs_new_value_float((float)src_left);  names[4] = "src_left";
    args[5] = avs_new_value_float((float)src_width); names[5] = "src_width";

    AVS_Value argv = avs_new_value_array(args, 6);
    return avs_invoke(env, "z_ConvertFormat", argv, (const char **)names);
}

static AVS_Value AVSC_CC comp_avs_create(AVS_ScriptEnvironment *env,
                                         AVS_Value args, void *user_data)
{
    (void)user_data;

    /* args: clip, standard, setup, precomb */
    AVS_Value clip_v = avs_array_elt(args, 0);

    const char *std_s = avs_defined(avs_array_elt(args, 1))
                        ? avs_as_string(avs_array_elt(args, 1)) : "pal";
    int standard;
    if (!strcmp(std_s, "pal"))
        standard = COMP_STD_PAL;
    else if (!strcmp(std_s, "ntsc"))
        standard = COMP_STD_NTSC;
    else
        return avs_new_value_error("Encode: standard must be pal or ntsc");

    const int setup = avs_defined(avs_array_elt(args, 2))
                      ? avs_as_bool(avs_array_elt(args, 2)) : 0;
    const int precomb = avs_defined(avs_array_elt(args, 3))
                        ? avs_as_bool(avs_array_elt(args, 3)) : 0;

    /* validate the source clip against the same rules as the VS plugin */
    AVS_Clip *src_clip = avs_take_clip(clip_v, env);
    const AVS_VideoInfo *svi = avs_get_video_info(src_clip);
    const int in_width = svi->width;
    const int in_height = svi->height;
    const int is_yuv = avs_is_yuv(svi);
    const int height_ok = standard == COMP_STD_PAL
                          ? in_height == COMP_ACTIVE_HEIGHT_PAL
                          : (in_height == 480 || in_height == COMP_ACTIVE_HEIGHT_NTSC);
    avs_release_clip(src_clip);

    if (!is_yuv)
        return avs_new_value_error("Encode: clip must be YUV");
    if (!height_ok)
        return avs_new_value_error(standard == COMP_STD_PAL
            ? "Encode: pal input must have 576 lines"
            : "Encode: ntsc input must have 480 or 486 lines");

    /* resample to the 4xfsc active raster and 4:4:4 through zimg */
    AVS_Value resampled = comp_avs_resample(env, clip_v, in_width, in_height,
                                            standard);
    if (avs_is_error(resampled))
        return resampled;

    /* build the modulator filter over the resampled clip */
    AVS_FilterInfo *fi;
    AVS_Clip *out = avs_new_c_filter(env, &fi, resampled, 1);
    avs_release_value(resampled);
    if (!out)
        return avs_new_value_error("Encode: failed to create filter");

    comp_avs_t *f = calloc(1, sizeof(*f));
    if (!f)
        return avs_new_value_error("Encode: out of memory");
    f->standard = standard;
    f->height = fi->vi.height;
    comp_encode_init(&f->enc, standard, setup, precomb);

    /* output is GRAY16 composite at the active raster width */
    fi->vi.pixel_type = AVS_CS_Y16;
    fi->get_frame = comp_avs_get_frame;
    fi->set_cache_hints = comp_avs_set_cache_hints;
    fi->free_filter = comp_avs_free;
    fi->user_data = f;

    AVS_Value ret;
    avs_set_to_clip(&ret, out);
    avs_release_clip(out);
    return ret;
}

AVSC_EXPORT const char *AVSC_CC avisynth_c_plugin_init(AVS_ScriptEnvironment *env)
{
    avs_add_function(env, "composite_Encode",
                     "c[standard]s[setup]b[precomb]b",
                     comp_avs_create, NULL);
    return "composite: PAL/NTSC composite video encoder";
}
