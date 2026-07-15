/*
 * PAL round trip at the core level: encode YUV444P16 to composite, decode
 * it back, and bound the reconstruction error away from edges.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decode.h"
#include "encode.h"

#define W COMP_ACTIVE_WIDTH_PAL
#define H COMP_ACTIVE_HEIGHT_PAL

static int fail = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        fail = 1; \
    } \
} while (0)

/* 75% color bars in 16-bit Y'CbCr (BT.601) */
static const uint16_t bars[8][3] = {
    { 46080, 32768, 32768 },  /* white 75% */
    { 42322, 21587, 34701 },  /* yellow */
    { 34700, 39812, 22015 },  /* cyan */
    { 30942, 28632, 23948 },  /* green */
    { 19234, 36904, 41588 },  /* magenta */
    { 15476, 25724, 43521 },  /* red */
    {  7854, 43949, 30835 },  /* blue */
    {  4096, 32768, 32768 },  /* black */
};

static uint16_t srcy[H][W], srcu[H][W], srcv[H][W];
static uint16_t comp[H][W];
static uint16_t outy[H][W], outu[H][W], outv[H][W];

int main(void)
{
    comp_encode_t enc;
    comp_decode_t dec;

    CHECK(comp_encode_init(&enc, COMP_STD_PAL, 0) == 0, "encode init");
    CHECK(comp_decode_init(&dec, COMP_STD_PAL, 0.4, 2) == 0, "decode init");
    CHECK(comp_decode_init(&dec, COMP_STD_NTSC, 0.4, 2) != 0, "ntsc must be rejected");
    CHECK(comp_decode_init(&dec, COMP_STD_PAL, 0.4, 2) == 0, "decode re-init");

    /* top half: color bars; bottom half: smooth chroma gradients */
    for (int r = 0; r < H; r++) {
        for (int x = 0; x < W; x++) {
            if (r < H / 2) {
                const int bar = x * 8 / W;
                srcy[r][x] = bars[bar][0];
                srcu[r][x] = bars[bar][1];
                srcv[r][x] = bars[bar][2];
            } else {
                srcy[r][x] = (uint16_t)(24000 + 16000.0 * sin(x * 0.008));
                srcu[r][x] = (uint16_t)(32768 + 9000.0 * sin(x * 0.004));
                srcv[r][x] = (uint16_t)(32768 + 7000.0 * cos(x * 0.003));
            }
        }
    }

    int32_t max_y = 0, max_u = 0, max_v = 0;
    for (int frame = 0; frame < 4; frame++) {
        for (int r = 0; r < H; r++)
            comp_encode_line(&enc, comp[r], srcy[r], srcu[r], srcv[r],
                             comp_sc_line(COMP_STD_PAL, frame, r));

        comp_decode_frame(&dec, frame, comp[0], W,
                          outy[0], W, outu[0], W, outv[0], W);

        /* measure away from transitions: the chroma low-passes smear
         * sharp edges by design, and a transform tile is 32 samples by
         * 16 field lines, so errors from an edge spill that far */
        for (int r = 32; r < H - 32; r++) {
            if (r >= H / 2 - 40 && r < H / 2 + 40)
                continue;  /* bars/gradient content boundary */
            for (int x = 48; x < W - 48; x++) {
                if (r < H / 2) {
                    const int bar = x * 8 / W;
                    const int lo = bar * W / 8, hi = (bar + 1) * W / 8;
                    if (x - lo < 48 || hi - x <= 48)
                        continue;
                }
                const int32_t dy = abs((int)outy[r][x] - (int)srcy[r][x]);
                const int32_t du = abs((int)outu[r][x] - (int)srcu[r][x]);
                const int32_t dv = abs((int)outv[r][x] - (int)srcv[r][x]);
                if (dy > max_y) max_y = dy;
                if (du > max_u) max_u = du;
                if (dv > max_v) max_v = dv;
            }
        }
    }

    printf("test_decode: round-trip max diff Y %d U %d V %d (16-bit)\n",
           max_y, max_u, max_v);
    CHECK(max_y <= 64, "Y round-trip error %d too large", max_y);
    CHECK(max_u <= 64, "U round-trip error %d too large", max_u);
    CHECK(max_v <= 64, "V round-trip error %d too large", max_v);

    comp_decode_free(&dec);

    if (!fail)
        printf("test_decode: all tests passed\n");
    return fail;
}
