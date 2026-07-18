#ifndef COMP_GEOM_H
#define COMP_GEOM_H

/* Raster geometry shared by every frontend. The BT.601 <-> 4xfsc active
 * mapping must be identical across the VapourSynth and Avisynth plugins:
 * both feed the same zimg resampler with these subpixel crop parameters,
 * and the trained thresholds/LUTs were calibrated on that raster, so any
 * drift between frontends would silently invalidate them. Keep the math
 * here, in the host-independent core, and let each frontend call it. */

/* Columns of replicated edge padding added before the inverse resample.
 * The 601 window is wider in time than the active raster, so the outer
 * output samples read a few taps beyond it; padding makes those taps read
 * the edge value rather than mirrored interior picture. */
#define COMP_EDGE_PAD 24

/* Forward (encode) resample: map a `in_width`-wide BT.601 line to the
 * 4xfsc active raster (and 4:4:4). Writes the resampler target width and
 * the subpixel src_left / src_width crop (in input samples) that equate
 * the shared time base. The 0H anchors are EBU Tech 3280-E (PAL) and
 * SMPTE 244M (NTSC); the active-line start is BT.601-5 Part A.
 * `standard` is COMP_STD_PAL or COMP_STD_NTSC. */
void comp_encode_resample_params(int standard, int in_width,
                                 int *target_width,
                                 double *src_left, double *src_width);

/* Inverse (decode) resample: map the 4xfsc active raster back to a
 * `out_width`-wide BT.601 line. src_left is measured from the left edge
 * of a clip padded with COMP_EDGE_PAD replicated columns on each side
 * (comp_encode_resample_params' inverse plus that offset); src_width is
 * the 601 active span in raster samples. */
void comp_decode_resample_params(int standard, int out_width,
                                 double *src_left, double *src_width);

/* Edge-splice bounds (Restore): how many output columns at each end fall
 * outside the active raster and so cannot be reconstructed from it. Given
 * the unpadded inverse mapping, counts the left/right columns whose source
 * center lands before -0.5 or past raster_width-0.5. */
void comp_decode_edge_columns(int standard, int out_width, int raster_width,
                              int *nleft, int *nright);

#endif
