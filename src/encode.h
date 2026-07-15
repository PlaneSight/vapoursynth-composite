#ifndef COMP_ENCODE_H
#define COMP_ENCODE_H

#include <stddef.h>
#include <stdint.h>

#include "subcarrier.h"

typedef struct comp_encode_t comp_encode_t;

struct comp_encode_t {
    int standard;
    int width;
    int den;
    int precomb;
    const int16_t *uv_taps;
    int uv_ntaps;
    int32_t ku, kv;              /* chroma level per Cb/Cr LSB, Q15 */
    int32_t level_black;
    int32_t luma_num, luma_den;  /* level = black + (y - 4096) * num/den */
    int16_t sin_q15[COMP_SC_DEN_PAL];
};

/* setup selects the NTSC 7.5 IRE pedestal (ignored for PAL); precomb
 * low-passes U/V vertically across same-field lines before modulation.
 * Returns 0 on success, nonzero if the standard is not supported. */
int comp_encode_init(comp_encode_t *e, int standard, int setup, int precomb);

/* modulate one line of YUV444P16 active picture into composite GRAY16
 * (without precombing; see comp_encode_frame) */
void comp_encode_line(const comp_encode_t *e, uint16_t *dst,
                      const uint16_t *srcy, const uint16_t *srcu,
                      const uint16_t *srcv, comp_sc_line_t sc);

/* modulate a whole frame, applying the vertical U/V precomb when
 * enabled; strides are in samples, rows sit at raster row_off */
void comp_encode_frame(const comp_encode_t *e, int frame, int rows, int row_off,
                       uint16_t *dst, ptrdiff_t dstride,
                       const uint16_t *srcy, ptrdiff_t ystride,
                       const uint16_t *srcu, ptrdiff_t ustride,
                       const uint16_t *srcv, ptrdiff_t vstride);

#endif
