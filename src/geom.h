#ifndef COMP_GEOM_H
#define COMP_GEOM_H

/* Raster geometry shared by every frontend. The BT.601 <-> 4xfsc active
 * mapping must be identical across the VapourSynth and Avisynth plugins:
 * both feed the same zimg resampler with these subpixel crop parameters,
 * and the trained thresholds/LUTs were calibrated on that raster, so any
 * drift between frontends would silently invalidate them. Keep the math
 * here, in the host-independent core, and let each frontend call it. */

/* Forward (encode) resample: map a `in_width`-wide BT.601 line to the
 * 4xfsc active raster (and 4:4:4). Writes the resampler target width and
 * the subpixel src_left / src_width crop (in input samples) that equate
 * the shared time base. The 0H anchors are EBU Tech 3280-E (PAL) and
 * SMPTE 244M (NTSC); the active-line start is BT.601-5 Part A.
 * `standard` is COMP_STD_PAL or COMP_STD_NTSC. */
void comp_encode_resample_params(int standard, int in_width,
                                 int *target_width,
                                 double *src_left, double *src_width);

#endif
