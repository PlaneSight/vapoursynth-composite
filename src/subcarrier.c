/*
 * Subcarrier phase generation (Clarke, BBC RD 1986/2, section 2).
 *
 * Field/line bookkeeping matches ld-chroma-encoder's encodeLine(), reduced
 * from double arithmetic to exact integers.
 */

#include <math.h>

#include "subcarrier.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void comp_sc_sin_q15(int den, int16_t *table)
{
    for (int k = 0; k < den; k++) {
        const long v = lrint(sin(2.0 * M_PI * k / den) * 32768.0);
        table[k] = (int16_t)(v > 32767 ? 32767 : v);
    }
}

comp_sc_line_t comp_sc_line(int standard, int frame, int row)
{
    comp_sc_line_t sc;

    if (standard == COMP_STD_PAL) {
        /* frame line 44 is the first active line; even lines are field 1;
         * the 8-field sequence spans 4 frames */
        const int frame_line = 44 + row;
        const int field_id = (frame % 4) * 2 + (frame_line & 1);
        /* complete lines since the start of the sequence */
        const int prev_lines = (field_id / 2) * 625 + (field_id % 2) * 313 + frame_line / 2;

        /* fsc/fH = 709379/2500 [Clarke 2.1.1], so each line advances
         * 1879/2500 of a cycle and each sample 625/2500. Lines sit on a
         * fixed 0H-aligned 1135-sample grid (ld-decode's conventional
         * PAL line layout); the 928-sample active window starts at
         * stored sample 182. */
        sc.phase = (182 * 625 + (prev_lines % 2500) * 1879) % 2500;
        sc.vswitch = (prev_lines & 1) ? -1 : 1;
    } else {
        /* frame line 39 is the first active line; the 4-field sequence
         * spans 2 frames */
        const int frame_line = 39 + row;
        const int field_id = (frame % 2) * 2 + (frame_line & 1);
        const int prev_lines = (field_id / 2) * 525 + (field_id % 2) * 263 + frame_line / 2;

        /* fsc/fH = 455/2: 227.5 cycles per line is 360/720, one sample is
         * 180/720. Lines are stored 0H-aligned (ld-decode convention):
         * 0H precedes stored sample 0 by 57/90 of a sample (SMPTE 244M
         * puts 0H at
         * 33/90 between digital samples 784 and 785), giving +114/720;
         * the sequence starts with the subcarrier at -1/4 cycle (-180/720)
         * so the inverted burst rises through zero [SMPTE 244M]: together
         * -66, or +654 mod 720. The 758-sample active window starts at
         * stored sample 130 (SMPTE 244M digital sample 5). */
        sc.phase = (130 * 180 + 654 + (prev_lines % 720) * 360) % 720;
        sc.vswitch = 1;
    }

    return sc;
}
