#ifndef COMP_ENCODE_H
#define COMP_ENCODE_H

#include <stdint.h>

#include "subcarrier.h"

typedef struct comp_encode_t comp_encode_t;

struct comp_encode_t {
    int standard;
    int width;
    int den;
    int16_t sin_q15[COMP_SC_DEN_PAL];  /* sin(2*pi*k/den) */
};

/* returns 0 on success, nonzero if the standard is not supported */
int comp_encode_init(comp_encode_t *e, int standard);

/* modulate one line of YUV444P16 active picture into composite GRAY16 at
 * CVBS levels (black 0x4000, white 0xD300, clamped to 0x0100-0xFEFF) */
void comp_encode_line(const comp_encode_t *e, uint16_t *dst,
                      const uint16_t *srcy, const uint16_t *srcu,
                      const uint16_t *srcv, comp_sc_line_t sc);

#endif
