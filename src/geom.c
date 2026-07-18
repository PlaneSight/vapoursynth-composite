#include "geom.h"

#include "subcarrier.h"

/* rho converts 4xfsc sample counts into 13.5 MHz (BT.601) sample counts:
 * the sample-rate ratio. NTSC 4fsc/13.5 MHz = 35/33 exactly; PAL =
 * 17734475/13500000 = 709379/540000. */
#define RHO_PAL  (540000.0 / 709379.0)
#define RHO_NTSC (33.0 / 35.0)

void comp_encode_resample_params(int standard, int in_width,
                                 int *target_width,
                                 double *src_left, double *src_width)
{
    const int pal = standard == COMP_STD_PAL;
    const int width = pal ? COMP_ACTIVE_WIDTH_PAL : COMP_ACTIVE_WIDTH_NTSC;
    const double rho = pal ? RHO_PAL : RHO_NTSC;
    /* where the active window starts on the 0H-aligned 4xfsc raster */
    const double active0 = pal ? 182.0 : 130.0 + 57.0 / 90.0;
    /* BT.601 first active luma sample, in luma clocks after 0H */
    const double anchor601 = pal ? 132.0 : 122.0;
    const double scale = in_width / 720.0;

    *target_width = width;
    *src_left = scale * ((active0 - 0.5) * rho - anchor601) + 0.5;
    *src_width = scale * (width * rho);
}
