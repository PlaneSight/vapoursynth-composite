/*
 * PAL composite encoding (Clarke, BBC RD 1986/2, section 3.2).
 *
 * Fixed-point port of ld-chroma-encoder's PALEncoder::encodeLine() for
 * the active picture only: no syncs, burst, blanking, or edge gating.
 * Floating point is used only to fill the sine table here.
 */

#include <math.h>

#include "encode.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 1.3 MHz low-pass Gaussian for U/V at 4xfsc [Clarke 3.2.4: >= -3 dB at
 * 1.3 MHz, <= -20 dB at 4.0 MHz]: ld-chroma-encoder's 13 taps
 * (scipy.signal.gaussian(13, 1.52), normalised) rounded to Q15. The
 * rounded taps sum to exactly 32768, so flat chroma is preserved. */
#define UV_TAPS 13
static const int16_t uv_filter_q15[UV_TAPS] = {
    4, 38, 270, 1226, 3619, 6927, 8600, 6927, 3619, 1226, 270, 38, 4,
};

/* colour-difference weights on full-excursion Cb/Cr, times the
 * black-to-white span, Q15 per 16-bit Cb/Cr LSB [Poynton eq 25.5/28.1
 * via ld-chroma encoder.h]: round((1-Kb)*kB/28672 * 0x9300 * 32768) and
 * the (1-Kr)*kR equivalent */
#define KU 18752
#define KV 26449

/* 10-bit composite levels in a 16-bit container (value << 6)
 * [EBU Tech 3280] */
#define LEVEL_BLACK 0x4000
#define LEVEL_MIN   0x0100
#define LEVEL_MAX   0xFEFF

int comp_encode_init(comp_encode_t *e, int standard)
{
    if (standard != COMP_STD_PAL)
        return -1;

    e->standard = standard;
    e->width = COMP_ACTIVE_WIDTH_PAL;
    e->den = COMP_SC_DEN_PAL;
    for (int k = 0; k < e->den; k++) {
        const long v = lrint(sin(2.0 * M_PI * k / e->den) * 32768.0);
        e->sin_q15[k] = (int16_t)(v > 32767 ? 32767 : v);
    }
    return 0;
}

void comp_encode_line(const comp_encode_t *e, uint16_t *dst,
                      const uint16_t *srcy, const uint16_t *srcu,
                      const uint16_t *srcv, comp_sc_line_t sc)
{
    const int w = e->width;
    int32_t uf[COMP_ACTIVE_WIDTH_PAL];
    int32_t vf[COMP_ACTIVE_WIDTH_PAL];

    /* low-pass U/V, zero-padded at the active edges (blanking carries no
     * chroma, so this matches filtering the full stored line) */
    for (int x = 0; x < w; x++) {
        int32_t au = 0, av = 0;
        for (int j = 0; j < UV_TAPS; j++) {
            const int k = x + j - UV_TAPS / 2;
            if (k >= 0 && k < w) {
                au += uv_filter_q15[j] * (srcu[k] - 32768);
                av += uv_filter_q15[j] * (srcv[k] - 32768);
            }
        }
        uf[x] = (au + 16384) >> 15;
        vf[x] = (av + 16384) >> 15;
    }

    /* at 4xfsc the carrier advances exactly 90 degrees per sample, so a
     * line needs only its start-phase sine and cosine, cycled through a
     * 4-sample sign/swap pattern; the V-switch folds into the cosine */
    const int32_t sn = e->sin_q15[sc.phase];
    const int32_t cs = e->sin_q15[(sc.phase + e->den / 4) % e->den];
    const int32_t s4[4] = { sn, cs, -sn, -cs };
    const int32_t c4[4] = { sc.vswitch * cs, -sc.vswitch * sn,
                            -sc.vswitch * cs, sc.vswitch * sn };

    for (int x = 0; x < w; x++) {
        /* luma level: (Y - 4096) * 0x9300/0xDB00, exactly 49/73 */
        const int32_t yl = srcy[x] - 4096;
        const int32_t luma = LEVEL_BLACK + (yl * 49 + (yl >= 0 ? 36 : -36)) / 73;

        /* chroma = U sin(a) + V cos(a) Vsw [Clarke 3.1] */
        const int32_t cu = (uf[x] * KU + 16384) >> 15;
        const int32_t cv = (vf[x] * KV + 16384) >> 15;
        const int32_t chroma = (cu * s4[x & 3] + cv * c4[x & 3] + 16384) >> 15;

        const int32_t out = luma + chroma;
        dst[x] = (uint16_t)(out < LEVEL_MIN ? LEVEL_MIN :
                            out > LEVEL_MAX ? LEVEL_MAX : out);
    }
}
