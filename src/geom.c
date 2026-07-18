#include "geom.h"

#include "subcarrier.h"

/* rho converts 4xfsc sample counts into 13.5 MHz (BT.601) sample counts:
 * the sample-rate ratio. NTSC 4fsc/13.5 MHz = 35/33 exactly; PAL =
 * 17734475/13500000 = 709379/540000. */
#define RHO_PAL  (540000.0 / 709379.0)
#define RHO_NTSC (33.0 / 35.0)

/* the three per-standard anchors shared by the forward and inverse maps */
static void comp_anchors(int standard, double *rho, double *active0,
                         double *anchor601)
{
    const int pal = standard == COMP_STD_PAL;
    *rho = pal ? RHO_PAL : RHO_NTSC;
    /* where the active window starts on the 0H-aligned 4xfsc raster */
    *active0 = pal ? 182.0 : 130.0 + 57.0 / 90.0;
    /* BT.601 first active luma sample, in luma clocks after 0H */
    *anchor601 = pal ? 132.0 : 122.0;
}

void comp_encode_resample_params(int standard, int in_width,
                                 int *target_width,
                                 double *src_left, double *src_width)
{
    const int pal = standard == COMP_STD_PAL;
    const int width = pal ? COMP_ACTIVE_WIDTH_PAL : COMP_ACTIVE_WIDTH_NTSC;
    double rho, active0, anchor601;
    comp_anchors(standard, &rho, &active0, &anchor601);
    const double scale = in_width / 720.0;

    *target_width = width;
    *src_left = scale * ((active0 - 0.5) * rho - anchor601) + 0.5;
    *src_width = scale * (width * rho);
}

void comp_decode_resample_params(int standard, int out_width,
                                 double *src_left, double *src_width)
{
    double rho, active0, anchor601;
    comp_anchors(standard, &rho, &active0, &anchor601);

    *src_left = COMP_EDGE_PAD + anchor601 / rho - (active0 - 0.5)
                - 0.5 * (720.0 / out_width) / rho;
    *src_width = 720.0 / rho;
}

void comp_decode_edge_columns(int standard, int out_width, int raster_width,
                              int *nleft, int *nright)
{
    double rho, active0, anchor601;
    comp_anchors(standard, &rho, &active0, &anchor601);

    /* the unpadded inverse mapping and its output-sample step */
    const double s_left = anchor601 / rho - (active0 - 0.5)
                          - 0.5 * (720.0 / out_width) / rho;
    const double step = (720.0 / rho) / out_width;

    int nl = 0, nr = 0;
    while (nl < out_width && s_left + (nl + 0.5) * step < -0.5)
        nl++;
    while (nr < out_width && s_left + (out_width - nr - 0.5) * step > raster_width - 0.5)
        nr++;
    *nleft = nl;
    *nright = nr;
}
