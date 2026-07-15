#ifndef COMP_ENCODE_H
#define COMP_ENCODE_H

#include <stdint.h>

#include "subcarrier.h"

typedef struct comp_encode_t comp_encode_t;

struct comp_encode_t {
    int standard;
    int width;
    int den;
    const int16_t *uv_taps;
    int uv_ntaps;
    int32_t ku, kv;              /* chroma level per Cb/Cr LSB, Q15 */
    int32_t level_black;
    int32_t luma_num, luma_den;  /* level = black + (y - 4096) * num/den */
    int16_t sin_q15[COMP_SC_DEN_PAL];
};

/* setup selects the NTSC 7.5 IRE pedestal; ignored for PAL.
 * Returns 0 on success, nonzero if the standard is not supported. */
int comp_encode_init(comp_encode_t *e, int standard, int setup);

/* modulate one line of YUV444P16 active picture into composite GRAY16 */
void comp_encode_line(const comp_encode_t *e, uint16_t *dst,
                      const uint16_t *srcy, const uint16_t *srcu,
                      const uint16_t *srcv, comp_sc_line_t sc);

#endif
