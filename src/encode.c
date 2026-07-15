/*
 * PAL and NTSC composite encoding (Clarke, BBC RD 1986/2, sections 3.2
 * and 3.3).
 *
 * Fixed-point port of ld-chroma-encoder's PALEncoder/NTSCEncoder
 * encodeLine() for the active picture only: no syncs, burst, blanking,
 * or edge gating. NTSC uses the wideband-U/V mode. Floating point is
 * used only at init.
 */

#include <math.h>

#include "encode.h"

/* 1.3 MHz low-pass Gaussian for U/V at PAL 4xfsc [Clarke 3.2.4: >= -3 dB
 * at 1.3 MHz, <= -20 dB at 4.0 MHz]: ld-chroma-encoder's 13 taps
 * (scipy.signal.gaussian(13, 1.52), normalized) rounded to Q15. The
 * rounded taps sum to exactly 32768, so flat chroma is preserved. */
static const int16_t uv_taps_pal[13] = {
    4, 38, 270, 1226, 3619, 6927, 8600, 6927, 3619, 1226, 270, 38, 4,
};

/* the same template at NTSC 4xfsc: Clarke's Table 1 nine taps as used by
 * ld-chroma-encoder, rounded to Q15 with the center tap adjusted by +2
 * so the taps sum to exactly 32768 */
static const int16_t uv_taps_ntsc[9] = {
    69, 626, 2959, 7563, 10334, 7563, 2959, 626, 69,
};

/* color-difference weights on full-excursion Cb/Cr
 * [Poynton eq 25.5/28.1 via ld-chroma encoder.h] */
#define KB 0.49211104112248356308804691718185
#define KR 0.87728321993817866838972487283129

/* legal sample range, 10-bit levels in a 16-bit container (value << 6)
 * [EBU Tech 3280; SMPTE 244M] */
#define LEVEL_MIN 0x0100
#define LEVEL_MAX 0xFEFF

static inline int32_t rdiv32(int64_t num, int32_t den)
{
    return (int32_t)((num >= 0 ? num + den / 2 : num - den / 2) / den);
}

int comp_encode_init(comp_encode_t *e, int standard, int setup)
{
    int32_t level_white;

    if (standard == COMP_STD_PAL) {
        e->width = COMP_ACTIVE_WIDTH_PAL;
        e->den = COMP_SC_DEN_PAL;
        e->uv_taps = uv_taps_pal;
        e->uv_ntaps = 13;
        /* black/blanking and white [EBU Tech 3280] */
        e->level_black = 0x4000;
        level_white = 0xD300;
    } else if (standard == COMP_STD_NTSC) {
        e->width = COMP_ACTIVE_WIDTH_NTSC;
        e->den = COMP_SC_DEN_NTSC;
        e->uv_taps = uv_taps_ntsc;
        e->uv_ntaps = 9;
        /* blanking 240 and white 800, as 10-bit << 6 [SMPTE 244M]; the
         * optional 7.5 IRE setup pedestal (SMPTE 170M, 5.6 codes/IRE)
         * lifts black to 282 */
        e->level_black = setup ? 0x4680 : 0x3C00;
        level_white = 0xC800;
    } else {
        return -1;
    }

    e->standard = standard;

    /* luma level slope (white - black) / (219 << 8), reduced by the
     * common factor 256: 147/219 PAL, 140/219 NTSC, 259/438 with setup */
    e->luma_num = (level_white - e->level_black) / 256;
    e->luma_den = 219;
    if (e->luma_num * 256 != level_white - e->level_black) {
        e->luma_num = (level_white - e->level_black) / 128;
        e->luma_den = 438;
    }

    /* chroma level per full-excursion Cb/Cr LSB, Q15 */
    const double span = level_white - e->level_black;
    e->ku = (int32_t)lrint((1.0 - 0.114) * KB / (112.0 * 256.0) * span * 32768.0);
    e->kv = (int32_t)lrint((1.0 - 0.299) * KR / (112.0 * 256.0) * span * 32768.0);

    comp_sc_sin_q15(e->den, e->sin_q15);
    return 0;
}

void comp_encode_line(const comp_encode_t *e, uint16_t *dst,
                      const uint16_t *srcy, const uint16_t *srcu,
                      const uint16_t *srcv, comp_sc_line_t sc)
{
    const int w = e->width;
    const int ntaps = e->uv_ntaps;
    int32_t uf[COMP_ACTIVE_WIDTH_PAL];
    int32_t vf[COMP_ACTIVE_WIDTH_PAL];

    /* low-pass U/V, zero-padded at the active edges (blanking carries no
     * chroma, so this matches filtering the full stored line) */
    for (int x = 0; x < w; x++) {
        int32_t au = 0, av = 0;
        for (int j = 0; j < ntaps; j++) {
            const int k = x + j - ntaps / 2;
            if (k >= 0 && k < w) {
                au += e->uv_taps[j] * (srcu[k] - 32768);
                av += e->uv_taps[j] * (srcv[k] - 32768);
            }
        }
        uf[x] = (au + 16384) >> 15;
        vf[x] = (av + 16384) >> 15;
    }

    /* at 4xfsc the carrier advances exactly 90 degrees per sample, so a
     * line needs only its start-phase sine and cosine, cycled through a
     * 4-sample sign/swap pattern; the V-switch (always +1 for NTSC)
     * folds into the cosine */
    const int32_t sn = e->sin_q15[sc.phase];
    const int32_t cs = e->sin_q15[(sc.phase + e->den / 4) % e->den];
    const int32_t s4[4] = { sn, cs, -sn, -cs };
    const int32_t c4[4] = { sc.vswitch * cs, -sc.vswitch * sn,
                            -sc.vswitch * cs, sc.vswitch * sn };

    for (int x = 0; x < w; x++) {
        const int32_t yl = srcy[x] - 4096;
        const int32_t luma = e->level_black + rdiv32((int64_t)yl * e->luma_num, e->luma_den);

        /* chroma = U sin(a) + V cos(a) Vsw [Clarke 3.1/3.3] */
        const int32_t cu = (uf[x] * e->ku + 16384) >> 15;
        const int32_t cv = (vf[x] * e->kv + 16384) >> 15;
        const int32_t chroma = (cu * s4[x & 3] + cv * c4[x & 3] + 16384) >> 15;

        const int32_t out = luma + chroma;
        dst[x] = (uint16_t)(out < LEVEL_MIN ? LEVEL_MIN :
                            out > LEVEL_MAX ? LEVEL_MAX : out);
    }
}
