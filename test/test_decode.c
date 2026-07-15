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


/* NTSC round trip: 2D comb + demod against the encoder, at both row
 * offsets a 480-line clip can occupy */
static void test_ntsc_roundtrip(int setup, int rows, int row_off)
{
    static uint16_t nsrcy[486][758], nsrcu[486][758], nsrcv[486][758];
    static uint16_t ncomp[486][758];
    static uint16_t nouty[486][758], noutu[486][758], noutv[486][758];
    comp_encode_t enc;
    comp_decode_t dec;
    const int w = COMP_ACTIVE_WIDTH_NTSC;

    CHECK(comp_encode_init(&enc, COMP_STD_NTSC, setup) == 0, "ntsc encode init");
    CHECK(comp_decode_init(&dec, COMP_STD_NTSC, 0.4, 1, setup, 2) == 0, "ntsc decode init");

    for (int r = 0; r < rows; r++) {
        for (int x = 0; x < w; x++) {
            if (r < rows / 2) {
                const int bar = x * 8 / w;
                nsrcy[r][x] = bars[bar][0];
                nsrcu[r][x] = bars[bar][1];
                nsrcv[r][x] = bars[bar][2];
            } else {
                nsrcy[r][x] = (uint16_t)(24000 + 16000.0 * sin(x * 0.008));
                nsrcu[r][x] = (uint16_t)(32768 + 9000.0 * sin(x * 0.004));
                nsrcv[r][x] = (uint16_t)(32768 + 7000.0 * cos(x * 0.003));
            }
        }
    }

    int32_t max_y = 0, max_u = 0, max_v = 0;
    for (int frame = 0; frame < 2; frame++) {
        for (int r = 0; r < rows; r++)
            comp_encode_line(&enc, ncomp[r], nsrcy[r], nsrcu[r], nsrcv[r],
                             comp_sc_line(COMP_STD_NTSC, frame, r + row_off));

        {
            const comp_frame_view_t v = { ncomp[0], w };
            const int vf = frame;
            comp_decode_frame(&dec, frame, 2, rows, row_off, &v, &vf, 0,
                              nouty[0], w, noutu[0], w, noutv[0], w);
        }

        for (int r = 12; r < rows - 12; r++) {
            if (r >= rows / 2 - 12 && r < rows / 2 + 12)
                continue;
            for (int x = 40; x < w - 40; x++) {
                if (r < rows / 2) {
                    const int bar = x * 8 / w;
                    const int lo = bar * w / 8, hi = (bar + 1) * w / 8;
                    if (x - lo < 40 || hi - x <= 40)
                        continue;
                }
                const int32_t dy = abs((int)nouty[r][x] - (int)nsrcy[r][x]);
                const int32_t du = abs((int)noutu[r][x] - (int)nsrcu[r][x]);
                const int32_t dv = abs((int)noutv[r][x] - (int)nsrcv[r][x]);
                if (dy > max_y) max_y = dy;
                if (du > max_u) max_u = du;
                if (dv > max_v) max_v = dv;
            }
        }
    }

    printf("test_decode: ntsc setup=%d rows=%d off=%d max diff Y %d U %d V %d\n",
           setup, rows, row_off, max_y, max_u, max_v);
    CHECK(max_y <= 512, "ntsc Y round-trip error %d too large", max_y);
    CHECK(max_u <= 1024, "ntsc U round-trip error %d too large", max_u);
    CHECK(max_v <= 1024, "ntsc V round-trip error %d too large", max_v);

    comp_decode_free(&dec);
}


/* 3D round trips on static content: every decoded frame draws on its
 * temporal neighbors, so encode a run of frames and decode the middle */
static uint16_t rt_comp[8][COMP_ACTIVE_HEIGHT_PAL][COMP_ACTIVE_WIDTH_PAL];

static void test_3d_roundtrip(int standard)
{
    const int pal = standard == COMP_STD_PAL;
    const int w = pal ? COMP_ACTIVE_WIDTH_PAL : COMP_ACTIVE_WIDTH_NTSC;
    const int rows = pal ? COMP_ACTIVE_HEIGHT_PAL : COMP_ACTIVE_HEIGHT_NTSC;
    const int nframes = 8;
    comp_encode_t enc;
    comp_decode_t dec;

    CHECK(comp_encode_init(&enc, standard, 0) == 0, "3d encode init");
    CHECK(comp_decode_init(&dec, standard, 0.4, 1, 0, 3) == 0, "3d decode init");
    const int look = comp_decode_look(&dec);
    CHECK(look == (pal ? 3 : 1), "3d look");

    for (int r = 0; r < rows; r++) {
        for (int x = 0; x < w; x++) {
            if (r < rows / 2) {
                const int bar = x * 8 / w;
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
    for (int frame = 0; frame < nframes; frame++)
        for (int r = 0; r < rows; r++)
            comp_encode_line(&enc, &rt_comp[frame][r][0], srcy[r], srcu[r], srcv[r],
                             comp_sc_line(standard, frame, r));

    int32_t max_y = 0, max_u = 0, max_v = 0;
    int32_t edge_y = 0, edge_u = 0, edge_v = 0;
    for (int frame = 0; frame < nframes; frame++) {
        const int edge = frame < look || frame >= nframes - look;
        comp_frame_view_t views[2 * COMP_T3D_LOOK + 1];
        int view_frames[2 * COMP_T3D_LOOK + 1];
        for (int i = 0; i <= 2 * look; i++) {
            int k = frame - look + i;
            k = k < 0 ? 0 : k >= nframes ? nframes - 1 : k;
            views[i].data = &rt_comp[k][0][0];
            views[i].stride = COMP_ACTIVE_WIDTH_PAL;
            view_frames[i] = k;
        }
        comp_decode_frame(&dec, frame, nframes, rows, 0, views, view_frames, look,
                          outy[0], COMP_ACTIVE_WIDTH_PAL,
                          outu[0], COMP_ACTIVE_WIDTH_PAL,
                          outv[0], COMP_ACTIVE_WIDTH_PAL);

        for (int r = 32; r < rows - 32; r++) {
            if (r >= rows / 2 - 40 && r < rows / 2 + 40)
                continue;
            for (int x = 48; x < w - 48; x++) {
                if (r < rows / 2) {
                    const int bar = x * 8 / w;
                    const int lo = bar * w / 8, hi = (bar + 1) * w / 8;
                    if (x - lo < 48 || hi - x <= 48)
                        continue;
                }
                const int32_t dy = abs((int)outy[r][x] - (int)srcy[r][x]);
                const int32_t du = abs((int)outu[r][x] - (int)srcu[r][x]);
                const int32_t dv = abs((int)outv[r][x] - (int)srcv[r][x]);
                int32_t *py = edge ? &edge_y : &max_y;
                int32_t *pu = edge ? &edge_u : &max_u;
                int32_t *pv = edge ? &edge_v : &max_v;
                if (dy > *py) *py = dy;
                if (du > *pu) *pu = du;
                if (dv > *pv) *pv = dv;
            }
        }
    }

    printf("test_decode: %s 3d max diff Y %d U %d V %d, edge frames Y %d U %d V %d\n",
           pal ? "pal" : "ntsc", max_y, max_u, max_v, edge_y, edge_u, edge_v);
    CHECK(max_y <= 64, "3d Y round-trip error %d too large", max_y);
    CHECK(max_u <= 128, "3d U round-trip error %d too large", max_u);
    CHECK(max_v <= 128, "3d V round-trip error %d too large", max_v);
    CHECK(edge_y <= 512, "3d edge Y error %d too large", edge_y);
    CHECK(edge_u <= 2048, "3d edge U error %d too large", edge_u);
    CHECK(edge_v <= 2048, "3d edge V error %d too large", edge_v);

    comp_decode_free(&dec);
}

int main(void)
{
    comp_encode_t enc;
    comp_decode_t dec;

    CHECK(comp_encode_init(&enc, COMP_STD_PAL, 0) == 0, "encode init");
    CHECK(comp_decode_init(&dec, COMP_STD_PAL, 0.4, 2, 0, 2) == 0, "decode init");

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

        {
            const comp_frame_view_t v = { comp[0], W };
            const int vf = frame;
            comp_decode_frame(&dec, frame, 4, H, 0, &v, &vf, 0,
                              outy[0], W, outu[0], W, outv[0], W);
        }

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

    test_ntsc_roundtrip(0, 486, 0);
    test_ntsc_roundtrip(1, 486, 0);
    test_ntsc_roundtrip(0, 480, 4);
    test_ntsc_roundtrip(0, 480, 5);

    test_3d_roundtrip(COMP_STD_PAL);
    test_3d_roundtrip(COMP_STD_NTSC);

    if (!fail)
        printf("test_decode: all tests passed\n");
    return fail;
}
