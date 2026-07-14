/*
 * PAL encode: fixed-point implementation vs a double-precision reference
 * following ld-chroma-encoder's PALEncoder::encodeLine(), plus exact
 * level anchors.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "encode.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fail = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        fail = 1; \
    } \
} while (0)

/* constants from ld-chroma encoder.h */
static const double kB = 0.49211104112248356308804691718185;
static const double kR = 0.87728321993817866838972487283129;

/* ld-chroma palencoder.cpp uvFilterCoeffs */
static const double uv_coeffs[13] = {
    0.00010852890120228184, 0.0011732778293138913, 0.008227778710181127,
    0.03742748297181873, 0.11043962430879829, 0.21139051659718247,
    0.2624655813630064, 0.21139051659718247, 0.11043962430879829,
    0.03742748297181873, 0.008227778710181127, 0.0011732778293138913,
    0.00010852890120228184,
};

/* double-precision reference for one active line, mirroring
 * PALEncoder::encodeLine() without syncs/burst/gates */
static void ref_encode_line(const uint16_t *srcy, const uint16_t *srcu,
                            const uint16_t *srcv, int frame, int row,
                            double *out)
{
    const double cb_scale = (1.0 - 0.114) * kB / (112.0 * 256.0);
    const double cr_scale = (1.0 - 0.299) * kR / (112.0 * 256.0);
    double y[COMP_ACTIVE_WIDTH_PAL], u[COMP_ACTIVE_WIDTH_PAL], v[COMP_ACTIVE_WIDTH_PAL];
    double uflt[COMP_ACTIVE_WIDTH_PAL], vflt[COMP_ACTIVE_WIDTH_PAL];

    for (int x = 0; x < COMP_ACTIVE_WIDTH_PAL; x++) {
        y[x] = (srcy[x] - 16.0 * 256.0) / (219.0 * 256.0);
        u[x] = (srcu[x] - 128.0 * 256.0) * cb_scale;
        v[x] = (srcv[x] - 128.0 * 256.0) * cr_scale;
    }

    for (int x = 0; x < COMP_ACTIVE_WIDTH_PAL; x++) {
        double au = 0.0, av = 0.0;
        for (int j = 0; j < 13; j++) {
            const int k = x + j - 6;
            if (k >= 0 && k < COMP_ACTIVE_WIDTH_PAL) {
                au += uv_coeffs[j] * u[k];
                av += uv_coeffs[j] * v[k];
            }
        }
        uflt[x] = au;
        vflt[x] = av;
    }

    /* subcarrier bookkeeping as in palencoder.cpp (see test_subcarrier.c) */
    const int frame_line = 44 + row;
    const int field_id = (frame * 2 + (frame_line % 2)) % 8;
    const int prev_lines = (field_id / 2) * 625 + (field_id % 2) * 313 + frame_line / 2;
    const double prev_cycles = prev_lines * 283.7516;
    const double vsw = (prev_lines % 2) == 0 ? 1.0 : -1.0;

    for (int x = 0; x < COMP_ACTIVE_WIDTH_PAL; x++) {
        const double a = 2.0 * M_PI * (((182 + x) / 4.0) + prev_cycles);
        const double chroma = uflt[x] * sin(a) + vflt[x] * cos(a) * vsw;
        double level = ((y[x] + chroma) * (0xD300 - 0x4000)) + 0x4000;
        if (level < 0x0100)
            level = 0x0100;
        if (level > 0xFEFF)
            level = 0xFEFF;
        out[x] = level;
    }
}

static uint32_t lcg_state = 12345;

static uint32_t lcg(void)
{
    lcg_state = lcg_state * 1664525 + 1013904223;
    return lcg_state >> 8;
}

int main(void)
{
    comp_encode_t enc;
    uint16_t srcy[COMP_ACTIVE_WIDTH_PAL], srcu[COMP_ACTIVE_WIDTH_PAL], srcv[COMP_ACTIVE_WIDTH_PAL];
    uint16_t dst[COMP_ACTIVE_WIDTH_PAL];
    double ref[COMP_ACTIVE_WIDTH_PAL];

    CHECK(comp_encode_init(&enc, COMP_STD_PAL) == 0, "pal init");
    CHECK(comp_encode_init(&enc, COMP_STD_NTSC) != 0, "ntsc must be rejected");
    CHECK(comp_encode_init(&enc, COMP_STD_PAL) == 0, "pal re-init");

    /* exact luma anchors: black and white map to the CVBS levels */
    for (int x = 0; x < COMP_ACTIVE_WIDTH_PAL; x++) {
        srcy[x] = 16 << 8;
        srcu[x] = srcv[x] = 32768;
    }
    comp_encode_line(&enc, dst, srcy, srcu, srcv, comp_sc_line(COMP_STD_PAL, 0, 0));
    for (int x = 0; x < COMP_ACTIVE_WIDTH_PAL; x++)
        CHECK(dst[x] == 0x4000, "black level: got 0x%04x at %d", dst[x], x);

    for (int x = 0; x < COMP_ACTIVE_WIDTH_PAL; x++)
        srcy[x] = 235 << 8;
    comp_encode_line(&enc, dst, srcy, srcu, srcv, comp_sc_line(COMP_STD_PAL, 0, 0));
    for (int x = 0; x < COMP_ACTIVE_WIDTH_PAL; x++)
        CHECK(dst[x] == 0xD300, "white level: got 0x%04x at %d", dst[x], x);

    /* fixed-point vs double reference over pseudorandom input */
    int32_t max_diff = 0;
    for (int frame = 0; frame < 4; frame++) {
        for (int row = 0; row < COMP_ACTIVE_HEIGHT_PAL; row += 7) {
            for (int x = 0; x < COMP_ACTIVE_WIDTH_PAL; x++) {
                srcy[x] = (uint16_t)(lcg() % 61440);
                srcu[x] = (uint16_t)(4096 + lcg() % 57344);
                srcv[x] = (uint16_t)(4096 + lcg() % 57344);
            }
            comp_encode_line(&enc, dst, srcy, srcu, srcv,
                             comp_sc_line(COMP_STD_PAL, frame, row));
            ref_encode_line(srcy, srcu, srcv, frame, row, ref);
            for (int x = 0; x < COMP_ACTIVE_WIDTH_PAL; x++) {
                const int32_t diff = (int32_t)labs(lrint(ref[x]) - dst[x]);
                if (diff > max_diff)
                    max_diff = diff;
            }
        }
    }
    CHECK(max_diff <= 4, "fixed point deviates from reference by %d > 4 LSB", max_diff);

    /* saturated color must stay inside the legal sample range */
    for (int x = 0; x < COMP_ACTIVE_WIDTH_PAL; x++) {
        srcy[x] = 235 << 8;
        srcu[x] = 61440;
        srcv[x] = 61440;
    }
    comp_encode_line(&enc, dst, srcy, srcu, srcv, comp_sc_line(COMP_STD_PAL, 0, 0));
    for (int x = 0; x < COMP_ACTIVE_WIDTH_PAL; x++)
        CHECK(dst[x] >= 0x0100 && dst[x] <= 0xFEFF,
              "sample 0x%04x outside legal range at %d", dst[x], x);

    if (!fail)
        printf("test_encode: all tests passed (max diff %d)\n", max_diff);
    return fail;
}
